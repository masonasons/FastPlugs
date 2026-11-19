// ---------------------------------------------------------------------------
// One of FastRoute's effects, as a CLAP plugin
//
// FastRoute's effects all have the same shape: Start at a rate, Configure
// from a Block, Process interleaved stereo in place, report their latency,
// take a dry/wet mix. So there is one plugin here, parameterised by an
// EffectDef, rather than thirteen.
//
// Three things the host and FastRoute disagree about are settled here:
//
//  * Threads. FastRoute calls Configure with the graph lock held, the same
//    lock its render takes, so an effect may reallocate inside it. The
//    equivalent here is the audio thread itself, so Configure is only ever
//    called from process() (or flush(), which runs where process would).
//
//  * Latency. A host has to be told about a change of latency and restart the
//    plugin to take it, which is a dropout. FastRoute's effects change theirs
//    as they are set - the pitch shifter adds 120 ms the moment it moves off
//    zero, the gate drops its lookahead when its range is zero. So the
//    latency reported is whatever the effect needed when it was activated,
//    and if it later needs less, the difference is made up here with a delay
//    of our own. Only needing more costs a restart, and a setting that would
//    do that over and over is marked not automatable.
//
//  * Restarting. Some effects start threads in Start and are not built to be
//    started twice, so every activation makes a fresh one.
// ---------------------------------------------------------------------------
#include "plugin.h"
#include "denormals.h"
#include "util.h"

#include <clapwrapper/wrapper_host.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fp
{
namespace
{

// While the mix is moving, the buffer is handed over in pieces this small,
// so a bypass fades rather than steps.
constexpr uint32_t kRampChunk   = 32;
constexpr double   kRampSeconds = 0.02;
// The most handed to an effect at once. Hosts may promise blocks of any size;
// this keeps the scratch buffer bounded whatever they say.
constexpr uint32_t kMaxChunk    = 4096;

std::string Utf8(const std::wstring& w)
{
    std::string out;
    for (size_t i = 0; i < w.size(); ++i)
    {
        unsigned long c = (unsigned long)w[i];
        // UTF-16 on Windows: join a surrogate pair.
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size())
        {
            const unsigned long lo = (unsigned long)w[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800)
        {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void CopyString(char* dst, size_t cap, const std::string& src)
{
    if (cap == 0) return;
    const size_t n = std::min(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

class Plugin
{
public:
    Plugin(const clap_host_t* host, const EffectDef& def)
        : m_host(host), m_def(def), m_count(def.params.size()),
          m_values(new std::atomic<double>[def.params.size()]),
          m_snap(def.params.size(), 0.0)
    {
        for (size_t i = 0; i < m_count; ++i) m_values[i].store(def.params[i].def);
        m_presetScratch.assign(m_count, 0.0);
        m_mixIndex = ParamIndex(def, kMixId);
        m_bypassIndex = ParamIndex(def, kBypassId);

        m_clap.desc = &def.desc;
        m_clap.plugin_data = this;
        m_clap.init = [](const clap_plugin_t* p) { return Self(p)->Init(); };
        m_clap.destroy = [](const clap_plugin_t* p) { delete Self(p); };
        m_clap.activate = [](const clap_plugin_t* p, double sr, uint32_t mn, uint32_t mx)
            { return Self(p)->Activate(sr, mn, mx); };
        m_clap.deactivate = [](const clap_plugin_t* p) { Self(p)->Deactivate(); };
        m_clap.start_processing = [](const clap_plugin_t*) { return true; };
        m_clap.stop_processing = [](const clap_plugin_t*) {};
        // FastRoute's effects have no reset of their own, and a fresh one
        // would mean allocating - and for two of them starting a thread - on
        // the audio thread. What they hold is a few milliseconds of history,
        // which plays out by itself.
        m_clap.reset = [](const clap_plugin_t*) {};
        m_clap.process = [](const clap_plugin_t* p, const clap_process_t* pr)
            { return Self(p)->Process(pr); };
        m_clap.get_extension = [](const clap_plugin_t* p, const char* id)
            { return Self(p)->Extension(id); };
        m_clap.on_main_thread = [](const clap_plugin_t* p) { Self(p)->OnMainThread(); };
    }

    const clap_plugin_t* Clap() const { return &m_clap; }

private:
    static Plugin* Self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

    // --- lifecycle -----------------------------------------------------------

    bool Init()
    {
        m_hostParams  = static_cast<const clap_host_params_t*>(m_host->get_extension(m_host, CLAP_EXT_PARAMS));
        m_hostLatency = static_cast<const clap_host_latency_t*>(m_host->get_extension(m_host, CLAP_EXT_LATENCY));
        m_hostLog     = static_cast<const clap_host_log_t*>(m_host->get_extension(m_host, CLAP_EXT_LOG));
        m_hostTail    = static_cast<const clap_host_tail_t*>(m_host->get_extension(m_host, CLAP_EXT_TAIL));

        // clap-wrapper's Audio Units turn a stepped parameter of more than two
        // values into an Indexed one, and an Indexed parameter is expected to
        // come with the list of its names - which the wrapper does not
        // provide. Under an Audio Unit a choice is therefore an ordinary
        // parameter, whose values still read as names one at a time.
        const auto* wrapper = static_cast<const clap_wrapper_host_information_t*>(
            m_host->get_extension(m_host, CLAP_WRAPPER_HOST_INFORMATION));
        if (wrapper && wrapper->get_wrapper_flavor)
        {
            const char* flavor = wrapper->get_wrapper_flavor(m_host);
            m_audioUnit = flavor && (!std::strcmp(flavor, CLAP_WRAPPER_HOST_FLAVOR_AUV2) ||
                                        !std::strcmp(flavor, CLAP_WRAPPER_HOST_FLAVOR_AUV3));
        }
        return true;
    }

    bool Activate(double sampleRate, uint32_t /*minFrames*/, uint32_t maxFrames)
    {
        m_rate = (int)std::lround(sampleRate);
        if (m_rate <= 0) return false;
        m_chunk = std::max<uint32_t>(1, std::min(maxFrames, kMaxChunk));

        if (m_def.refuseRate)
        {
            const std::string why = m_def.refuseRate(m_rate);
            if (!why.empty())
            {
                Log(CLAP_LOG_ERROR, std::wstring(why.begin(), why.end()));
                return false;
            }
        }

        std::unique_ptr<Effect> fx = m_def.make();
        std::wstring err;
        if (!fx || !fx->Start(m_rate, &err))
        {
            Log(CLAP_LOG_ERROR, err.empty() ? L"The effect could not be started." : err);
            return false;
        }
        if (m_def.isBuffer)
        {
            // Saving goes where a Recorder block would put it, as far as a
            // plugin can say: there is no folder setting, because a host has
            // no way to offer a folder picker for a parameter.
            RecordSettings rs;
            rs.folder = DefaultSaveFolder();
            rs.nameTemplate = L"{session} {date} {time}";
            rs.sessionName = L"Buffer";
            rs.bits = 24;
            fx->ConfigureRecording(rs);
        }

        m_fx = std::move(fx);
        m_buf.assign((size_t)m_chunk * kChannels, 0.0f);

        // Configured with any restart request held back: whatever latency this
        // asks for is the latency being activated, not a change to one.
        m_restartAsked = true;
        m_activating = true;
        m_dirty = false;
        ConfigureNow();
        m_activating = false;
        m_restartAsked = false;

        const uint32_t need = (uint32_t)std::max(0, m_fx->LatencyFrames());
        if (need != m_latency)
        {
            m_latency = need;
            if (m_hostLatency) m_hostLatency->changed(m_host);
        }
        m_pad.assign(((size_t)m_latency + 1) * kChannels, 0.0f);
        m_padPos = 0;
        m_padDelay = 0;

        m_mix = m_mixTarget;      // no fade in from nothing
        return true;
    }

    void Deactivate()
    {
        // Joins whatever threads the effect started.
        m_fx.reset();
    }

    // --- parameters ----------------------------------------------------------

    void UpdateMixTarget()
    {
        const bool bypass = m_bypassIndex >= 0 && m_values[m_bypassIndex].load() >= 0.5;
        const double mix = m_mixIndex >= 0 ? m_values[m_mixIndex].load() / 100.0 : 1.0;
        m_mixTarget = bypass ? 0.0f : (float)mix;
    }

    // Audio thread, or main thread while active and not processing.
    void ConfigureNow()
    {
        for (size_t i = 0; i < m_count; ++i) m_snap[i] = m_values[i].load();
        if (m_def.toBlock)
        {
            m_def.toBlock(m_snap.data(), m_block);
            m_fx->Configure(m_block);
        }
        else if (m_def.apply)
        {
            m_def.apply(*m_fx, m_snap.data());
        }
        UpdateMixTarget();

        // A reverb's tail follows its size, an echo's its feedback. The host
        // is told when that changes, so it does not cut one off.
        // Only from the audio thread, which is the one CLAP allows; during
        // activation, on the main thread, the host reads the tail afresh.
        const uint32_t tail = TailFrames();
        if (tail != m_lastTail)
        {
            m_lastTail = tail;
            if (!m_activating && m_hostTail && m_hostTail->changed) m_hostTail->changed(m_host);
        }

        const uint32_t need = (uint32_t)std::max(0, m_fx->LatencyFrames());
        if (need > m_latency)
        {
            // More than the host is compensating for. Until it restarts us
            // this is late, which is the best that can be done.
            m_padDelay = 0;
            if (!m_restartAsked)
            {
                m_restartAsked = true;
                m_host->request_restart(m_host);
            }
        }
        else
        {
            m_padDelay = m_latency - need;
        }
    }

    void ApplyParam(const clap_event_param_value_t* ev, const clap_output_events_t* out)
    {
        const int idx = ParamIndex(m_def, ev->param_id);
        if (idx < 0) return;
        const ParamDef& d = m_def.params[(size_t)idx];
        const double v = std::max(d.min, std::min(d.max, ev->value));

        if (d.preset && m_def.presetValues)
        {
            // Only a different choice loads a preset. Hosts send a parameter's
            // current value back now and then - after restoring a session, say -
            // and that must not undo the settings the session restored.
            const int was = ToInt(m_values[idx].exchange(v));
            const int now = ToInt(v);
            m_dirty = true;
            // Not in an Audio Unit. An AU's parameters must each keep what they
            // were set to, and the wrapper hands a batch of them over in an
            // order of its own - auval sets them all and then finds the room
            // has overwritten the rest, and Logic will not load an AU that
            // fails auval. There the choice is the room's character alone.
            if (now != was && !m_audioUnit) LoadPreset(idx, now, out, ev->header.time);
            return;
        }

        if (d.unit == Unit::Trigger)
        {
            // A button, which no plugin format has. Hosts draw it as a
            // switch, so every change of the switch is a press, either way.
            // The value is kept as set rather than put back to Off: an Audio
            // Unit's parameters must hold what they were given, and a switch
            // that flips back by itself is one a host may or may not redraw.
            const double old = m_values[idx].exchange(v);
            if (v != old && m_fx) m_fx->DoAction(d.action);
            return;
        }

        m_values[idx].store(v);
        if (idx == m_mixIndex || idx == m_bypassIndex) { UpdateMixTarget(); return; }
        m_dirty = true;
    }

    // A preset choice has moved: the parameters it sets are set, and the host is
    // told, both in the event stream - which is how a host's own controls
    // follow - and by a rescan on the main thread, for a host that ignores
    // values coming back from a plugin.
    void LoadPreset(int idx, int choice, const clap_output_events_t* out, uint32_t time)
    {
        for (size_t i = 0; i < m_count; ++i) m_presetScratch[i] = m_values[i].load();
        m_def.presetValues(idx, choice, m_presetScratch.data());
        bool changed = false;
        for (size_t i = 0; i < m_count; ++i)
        {
            if ((int)i == idx) continue;
            const ParamDef& d = m_def.params[i];
            const double v = std::max(d.min, std::min(d.max, m_presetScratch[i]));
            if (v == m_values[i].load()) continue;
            m_values[i].store(v);
            if (out) PushValue(out, time, d.id, v);
            changed = true;
        }
        if (changed)
        {
            m_rescanPending = true;
            m_host->request_callback(m_host);
        }
    }

    static void PushValue(const clap_output_events_t* out, uint32_t time, clap_id id, double v)
    {
        clap_event_param_value_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.header.flags = 0;
        e.param_id = id;
        e.cookie = nullptr;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = v;
        out->try_push(out, &e.header);
    }

    void OnMainThread()
    {
        if (m_rescanPending.exchange(false) && m_hostParams)
            m_hostParams->rescan(m_host, CLAP_PARAM_RESCAN_VALUES);
    }

    uint32_t TailFrames() const
    {
        if (!m_def.tailFrames || m_count > kMaxParams) return 0;
        double v[kMaxParams];
        for (size_t i = 0; i < m_count; ++i) v[i] = m_values[i].load();
        return m_def.tailFrames(v, m_rate);
    }

    void Flush(const clap_input_events_t* in, const clap_output_events_t* out)
    {
        const uint32_t n = in ? in->size(in) : 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const clap_event_header_t* h = in->get(in, i);
            if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
                ApplyParam(reinterpret_cast<const clap_event_param_value_t*>(h), out);
        }
        if (m_fx && m_dirty.exchange(false)) ConfigureNow();
    }

    // --- audio -----------------------------------------------------------------

    clap_process_status Process(const clap_process_t* p)
    {
        if (!m_fx) return CLAP_PROCESS_ERROR;
        NoDenormals ftz;
        const uint32_t frames = p->frames_count;
        const clap_input_events_t* in = p->in_events;
        const uint32_t n = in ? in->size(in) : 0;

        uint32_t pos = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const clap_event_header_t* h = in->get(in, i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
                continue;
            const uint32_t t = std::min(h->time, frames);
            if (t > pos) { Render(p, pos, t); pos = t; }
            ApplyParam(reinterpret_cast<const clap_event_param_value_t*>(h), p->out_events);
        }
        if (pos < frames) Render(p, pos, frames);
        return CLAP_PROCESS_CONTINUE;
    }

    void Render(const clap_process_t* p, uint32_t start, uint32_t end)
    {
        if (m_dirty.exchange(false)) ConfigureNow();

        const clap_audio_buffer_t* ib = p->audio_inputs_count ? &p->audio_inputs[0] : nullptr;
        clap_audio_buffer_t* ob = p->audio_outputs_count ? &p->audio_outputs[0] : nullptr;
        const float* inL = nullptr;
        const float* inR = nullptr;
        if (ib && ib->data32 && ib->channel_count > 0)
        {
            inL = ib->data32[0];
            inR = ib->channel_count > 1 ? ib->data32[1] : inL;   // mono feeds both sides
        }
        if (ob) ob->constant_mask = 0;

        float* b = m_buf.data();
        while (start < end)
        {
            const bool ramping = m_mix != m_mixTarget;
            const uint32_t k = std::min(end - start, ramping ? kRampChunk : m_chunk);

            for (uint32_t i = 0; i < k; ++i)
            {
                b[i * 2 + 0] = inL ? inL[start + i] : 0.0f;
                b[i * 2 + 1] = inR ? inR[start + i] : 0.0f;
            }

            if (ramping)
            {
                const float step = (float)(k / (kRampSeconds * m_rate));
                if (m_mix < m_mixTarget) m_mix = std::min(m_mixTarget, m_mix + step);
                else                     m_mix = std::max(m_mixTarget, m_mix - step);
            }
            m_fx->SetMix(m_mix);
            m_fx->Process(b, k);
            if (m_padDelay) Pad(b, k);

            if (ob && ob->data32)
            {
                if (ob->channel_count == 1)
                {
                    float* o = ob->data32[0];
                    for (uint32_t i = 0; i < k; ++i)
                        o[start + i] = 0.5f * (b[i * 2] + b[i * 2 + 1]);
                }
                else if (ob->channel_count >= 2)
                {
                    float* oL = ob->data32[0];
                    float* oR = ob->data32[1];
                    for (uint32_t i = 0; i < k; ++i)
                    {
                        oL[start + i] = b[i * 2 + 0];
                        oR[start + i] = b[i * 2 + 1];
                    }
                    for (uint32_t c = 2; c < ob->channel_count; ++c)
                        std::memset(ob->data32[c] + start, 0, k * sizeof(float));
                }
            }
            start += k;
        }
    }

    // The latency the effect no longer needs, still paid, so what the host
    // compensates for stays true.
    void Pad(float* b, uint32_t k)
    {
        const size_t cap = m_pad.size() / kChannels;
        for (uint32_t i = 0; i < k; ++i)
        {
            m_pad[m_padPos * 2 + 0] = b[i * 2 + 0];
            m_pad[m_padPos * 2 + 1] = b[i * 2 + 1];
            const size_t rd = (m_padPos + cap - m_padDelay) % cap;
            b[i * 2 + 0] = m_pad[rd * 2 + 0];
            b[i * 2 + 1] = m_pad[rd * 2 + 1];
            m_padPos = (m_padPos + 1) % cap;
        }
    }

    // --- state -----------------------------------------------------------------
    //
    // Text, one parameter a line, so a session saved by this version still
    // loads after parameters are added. Values are written as millionths in
    // an integer, because a host running in a locale with a decimal comma
    // would otherwise write a file another machine cannot read.

    bool SaveState(const clap_ostream_t* s)
    {
        std::string out = "FastPlugs 1\n" + m_def.key + "\n";
        for (size_t i = 0; i < m_count; ++i)
        {
            const ParamDef& d = m_def.params[i];
            char line[64];
            std::snprintf(line, sizeof(line), "%u %lld\n", (unsigned)d.id,
                          (long long)std::llround(m_values[i].load() * 1e6));
            out += line;
        }
        const char* p = out.data();
        size_t left = out.size();
        while (left > 0)
        {
            const int64_t w = s->write(s, p, left);
            if (w <= 0) return false;
            p += w;
            left -= (size_t)w;
        }
        return true;
    }

    bool LoadState(const clap_istream_t* s)
    {
        std::string in;
        char chunk[1024];
        for (;;)
        {
            const int64_t r = s->read(s, chunk, sizeof(chunk));
            if (r < 0) return false;
            if (r == 0) break;
            in.append(chunk, (size_t)r);
            if (in.size() > (1u << 20)) return false;
        }
        if (in.compare(0, 10, "FastPlugs ") != 0) return false;

        // Everything not mentioned goes back to its default, so loading a
        // state is the same whatever this instance was set to before.
        for (size_t i = 0; i < m_count; ++i) m_values[i].store(m_def.params[i].def);

        size_t at = in.find('\n');
        at = (at == std::string::npos) ? in.size() : in.find('\n', at + 1);   // skip the key
        while (at != std::string::npos && at < in.size())
        {
            const size_t next = in.find('\n', at + 1);
            const std::string line = in.substr(at + 1, next == std::string::npos
                                                       ? std::string::npos : next - at - 1);
            at = next;
            char* end = nullptr;
            const unsigned long id = std::strtoul(line.c_str(), &end, 10);
            if (end == line.c_str()) continue;
            const char* num = end;
            const long long raw = std::strtoll(num, &end, 10);
            if (end == num) continue;
            const int idx = ParamIndex(m_def, (clap_id)id);
            if (idx < 0) continue;
            const ParamDef& d = m_def.params[(size_t)idx];
            m_values[idx].store(std::max(d.min, std::min(d.max, (double)raw / 1e6)));
        }

        // The audio thread picks this up on its next block; inactive, the
        // next activation does.
        m_dirty = true;
        if (m_hostParams) m_hostParams->rescan(m_host, CLAP_PARAM_RESCAN_VALUES);
        return true;
    }

    // --- extensions --------------------------------------------------------------

    const void* Extension(const char* id)
    {
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &s_state;
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_ports;
        if (!std::strcmp(id, CLAP_EXT_TAIL) && m_def.tailFrames) return &s_tail;
        if (!std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS) ||
            !std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT))
            return &s_configurable;
        return nullptr;
    }

    bool ParamInfo(uint32_t index, clap_param_info_t* info) const
    {
        if (index >= m_count) return false;
        const ParamDef& d = m_def.params[index];
        std::memset(info, 0, sizeof(*info));
        info->id = d.id;
        uint32_t f = 0;
        if (d.automatable) f |= CLAP_PARAM_IS_AUTOMATABLE;
        switch (d.unit)
        {
        case Unit::Choice:
            if (!m_audioUnit) f |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
            break;
        case Unit::Toggle:
        case Unit::Trigger: f |= CLAP_PARAM_IS_STEPPED; break;
        case Unit::Bypass:  f |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_BYPASS; break;
        default: break;
        }
        info->flags = f;
        info->cookie = nullptr;
        CopyString(info->name, sizeof(info->name), d.name);
        CopyString(info->module, sizeof(info->module), d.module);
        info->min_value = d.min;
        info->max_value = d.max;
        info->default_value = d.def;
        return true;
    }

    bool PortInfo(uint32_t index, bool isInput, clap_audio_port_info_t* info) const
    {
        if (index != 0) return false;
        const uint32_t cc = isInput ? m_inChannels : m_outChannels;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        CopyString(info->name, sizeof(info->name), isInput ? "Input" : "Output");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = cc;
        info->port_type = cc == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        // Input and output may share memory - the input is copied out before
        // anything is written back - but only when they are the same shape.
        info->in_place_pair = (m_inChannels == m_outChannels) ? 0 : CLAP_INVALID_ID;
        return true;
    }

    // Mono and stereo, in any combination. FastRoute's effects are stereo, so
    // a mono input is fed to both sides and a mono output is their average -
    // which for every effect here but the channel mixer is exactly the one
    // channel processed, since both sides were the same going in.
    static bool AcceptableRequest(const clap_audio_port_configuration_request_t& r)
    {
        if (r.port_index != 0) return false;
        return r.channel_count == 1 || r.channel_count == 2;
    }

    void Log(clap_log_severity sev, const std::wstring& msg)
    {
        LogError(msg);
        if (m_hostLog) m_hostLog->log(m_host, sev, Utf8(msg).c_str());
    }

    // --- the extension tables ----------------------------------------------------

    static const clap_plugin_params_t s_params;
    static const clap_plugin_state_t s_state;
    static const clap_plugin_latency_t s_latency;
    static const clap_plugin_audio_ports_t s_ports;
    static const clap_plugin_configurable_audio_ports_t s_configurable;
    static const clap_plugin_tail_t s_tail;

    // More than any effect here has, so a tail can be worked out on the
    // audio thread without allocating.
    static constexpr size_t kMaxParams = 64;

    // --- members -------------------------------------------------------------------

    const clap_host_t* m_host;
    const EffectDef& m_def;
    clap_plugin_t m_clap{};
    const clap_host_params_t*  m_hostParams = nullptr;
    const clap_host_latency_t* m_hostLatency = nullptr;
    const clap_host_log_t*     m_hostLog = nullptr;
    const clap_host_tail_t*    m_hostTail = nullptr;
    uint32_t m_lastTail = 0;
    bool m_activating = false;
    std::atomic<bool> m_rescanPending{ false };
    std::vector<double> m_presetScratch;
    bool m_audioUnit = false;          // wrapped as an Audio Unit; see Init

    const size_t m_count;
    std::unique_ptr<std::atomic<double>[]> m_values;   // read anywhere
    std::vector<double> m_snap;        // a consistent copy, for toBlock
    int m_mixIndex = -1, m_bypassIndex = -1;
    std::atomic<bool> m_dirty{ true };

    uint32_t m_inChannels = 2, m_outChannels = 2;

    std::unique_ptr<Effect> m_fx;
    Block m_block;
    int m_rate = kDefaultRate;
    uint32_t m_chunk = 512;
    std::vector<float> m_buf;          // interleaved, as FastRoute's bus is

    float m_mix = 1.0f, m_mixTarget = 1.0f;

    uint32_t m_latency = 0;            // what the host was told
    bool m_restartAsked = false;
    std::vector<float> m_pad;
    size_t m_padPos = 0;
    uint32_t m_padDelay = 0;
};

const clap_plugin_params_t Plugin::s_params = {
    [](const clap_plugin_t* p) { return (uint32_t)Self(p)->m_count; },
    [](const clap_plugin_t* p, uint32_t i, clap_param_info_t* info) { return Self(p)->ParamInfo(i, info); },
    [](const clap_plugin_t* p, clap_id id, double* v)
    {
        const int idx = ParamIndex(Self(p)->m_def, id);
        if (idx < 0) return false;
        *v = Self(p)->m_values[idx].load();
        return true;
    },
    [](const clap_plugin_t* p, clap_id id, double v, char* out, uint32_t cap)
    {
        const int idx = ParamIndex(Self(p)->m_def, id);
        if (idx < 0 || cap == 0) return false;
        CopyString(out, cap, ValueText(Self(p)->m_def.params[(size_t)idx], v));
        return true;
    },
    [](const clap_plugin_t* p, clap_id id, const char* text, double* v)
    {
        const int idx = ParamIndex(Self(p)->m_def, id);
        if (idx < 0 || !text) return false;
        return TextValue(Self(p)->m_def.params[(size_t)idx], text, v);
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
        { Self(p)->Flush(in, out); },
};

const clap_plugin_state_t Plugin::s_state = {
    [](const clap_plugin_t* p, const clap_ostream_t* s) { return Self(p)->SaveState(s); },
    [](const clap_plugin_t* p, const clap_istream_t* s) { return Self(p)->LoadState(s); },
};

const clap_plugin_latency_t Plugin::s_latency = {
    [](const clap_plugin_t* p) { return Self(p)->m_latency; },
};

const clap_plugin_tail_t Plugin::s_tail = {
    [](const clap_plugin_t* p) { return Self(p)->TailFrames(); },
};

const clap_plugin_audio_ports_t Plugin::s_ports = {
    [](const clap_plugin_t*, bool) -> uint32_t { return 1; },
    [](const clap_plugin_t* p, uint32_t i, bool in, clap_audio_port_info_t* info)
        { return Self(p)->PortInfo(i, in, info); },
};

const clap_plugin_configurable_audio_ports_t Plugin::s_configurable = {
    [](const clap_plugin_t*, const clap_audio_port_configuration_request_t* r, uint32_t n)
    {
        for (uint32_t i = 0; i < n; ++i)
            if (!AcceptableRequest(r[i])) return false;
        return true;
    },
    [](const clap_plugin_t* p, const clap_audio_port_configuration_request_t* r, uint32_t n)
    {
        for (uint32_t i = 0; i < n; ++i)
            if (!AcceptableRequest(r[i])) return false;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (r[i].is_input) Self(p)->m_inChannels = r[i].channel_count;
            else               Self(p)->m_outChannels = r[i].channel_count;
        }
        return true;
    },
};

}  // namespace

const clap_plugin_t* CreatePlugin(const clap_host_t* host, const EffectDef& def)
{
    return (new Plugin(host, def))->Clap();
}

}  // namespace fp
