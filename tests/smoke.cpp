// ---------------------------------------------------------------------------
// A host in miniature.
//
// Runs every plugin in the bundle through what a DAW does to it, and checks
// what can be checked without ears:
//
//  * every parameter's text reads back as the value it came from;
//  * mono, stereo and mono-to-stereo layouts are all accepted and all run;
//  * random automation at odd block sizes never produces anything but finite
//    audio at a sane level;
//  * the latency the plugin reports is the latency it has - measured by
//    sending an impulse through it bypassed and seeing where it comes out -
//    including after a setting has made the effect itself need less;
//  * asking for more latency than was reported asks the host for a restart;
//  * a saved state loads back into a fresh instance exactly.
//
// Exits non-zero on the first thing that is wrong, and says what.
// ---------------------------------------------------------------------------
#include <clap/clap.h>

#include "plugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// --- the host ----------------------------------------------------------------

struct Host
{
    clap_host_t host{};
    bool restartRequested = false;

    Host()
    {
        host.clap_version = CLAP_VERSION_INIT;
        host.host_data = this;
        host.name = "FastPlugs smoke test";
        host.vendor = "FastPlugs";
        host.url = "";
        host.version = "1";
        host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
        host.request_restart = [](const clap_host_t* h)
            { static_cast<Host*>(h->host_data)->restartRequested = true; };
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t*) {};
    }
};

struct InEvents
{
    std::vector<clap_event_param_value_t> ev;
    clap_input_events_t list{};

    InEvents()
    {
        list.ctx = this;
        list.size = [](const clap_input_events_t* l)
            { return (uint32_t)static_cast<InEvents*>(l->ctx)->ev.size(); };
        list.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t*
            { return &static_cast<InEvents*>(l->ctx)->ev[i].header; };
    }

    void Add(uint32_t time, clap_id id, double v)
    {
        clap_event_param_value_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = v;
        ev.push_back(e);
    }
};

struct OutEvents
{
    int pushed = 0;
    clap_output_events_t list{};
    OutEvents()
    {
        list.ctx = this;
        list.try_push = [](const clap_output_events_t* l, const clap_event_header_t*)
            { ++static_cast<OutEvents*>(l->ctx)->pushed; return true; };
    }
};

// --- one plugin, driven --------------------------------------------------------

struct Instance
{
    Host host;
    const clap_plugin_t* p = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_latency_t* latency = nullptr;
    const clap_plugin_configurable_audio_ports_t* config = nullptr;
    uint32_t inCh = 2, outCh = 2;
    bool active = false;

    Instance(const clap_plugin_factory_t* f, const char* id)
    {
        p = f->create_plugin(f, &host.host, id);
        if (!p || !p->init(p)) { p = nullptr; return; }
        params  = (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
        state   = (const clap_plugin_state_t*)p->get_extension(p, CLAP_EXT_STATE);
        latency = (const clap_plugin_latency_t*)p->get_extension(p, CLAP_EXT_LATENCY);
        config  = (const clap_plugin_configurable_audio_ports_t*)
                  p->get_extension(p, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS);
    }
    ~Instance()
    {
        if (!p) return;
        if (active) p->deactivate(p);
        p->destroy(p);
    }

    bool Layout(uint32_t in, uint32_t out)
    {
        clap_audio_port_configuration_request_t r[2] = {};
        r[0].is_input = true;  r[0].port_index = 0; r[0].channel_count = in;
        r[1].is_input = false; r[1].port_index = 0; r[1].channel_count = out;
        if (!config->can_apply_configuration(p, r, 2)) return false;
        if (!config->apply_configuration(p, r, 2)) return false;
        inCh = in; outCh = out;
        return true;
    }

    bool Activate(double rate, uint32_t maxFrames)
    {
        active = p->activate(p, rate, 1, maxFrames);
        if (active) p->start_processing(p);
        return active;
    }

    void Deactivate()
    {
        if (!active) return;
        p->stop_processing(p);
        p->deactivate(p);
        active = false;
    }

    void Set(clap_id id, double v)
    {
        InEvents in;
        in.Add(0, id, v);
        OutEvents out;
        params->flush(p, &in.list, &out.list);
    }

    double Get(clap_id id)
    {
        double v = 0;
        params->get_value(p, id, &v);
        return v;
    }

    // Planar in, planar out, as a host has them.
    void Run(const std::vector<float>& inL, const std::vector<float>& inR,
             std::vector<float>& outL, std::vector<float>& outR,
             uint32_t offset, uint32_t frames, InEvents* events = nullptr)
    {
        float* ip[2] = { const_cast<float*>(inL.data()) + offset,
                         const_cast<float*>(inR.data()) + offset };
        float* op[2] = { outL.data() + offset, outR.data() + offset };

        clap_audio_buffer_t ib{};
        ib.data32 = ip;
        ib.channel_count = inCh;
        clap_audio_buffer_t ob{};
        ob.data32 = op;
        ob.channel_count = outCh;

        InEvents none;
        OutEvents out;
        clap_process_t pr{};
        pr.steady_time = -1;
        pr.frames_count = frames;
        pr.audio_inputs = &ib;
        pr.audio_inputs_count = 1;
        pr.audio_outputs = &ob;
        pr.audio_outputs_count = 1;
        pr.in_events = events ? &events->list : &none.list;
        pr.out_events = &out.list;
        p->process(p, &pr);
    }
};

struct Stream
{
    std::vector<char> data;
    size_t readAt = 0;
    clap_ostream_t out{};
    clap_istream_t in{};
    Stream()
    {
        out.ctx = this;
        out.write = [](const clap_ostream_t* s, const void* b, uint64_t n) -> int64_t
        {
            auto* self = static_cast<Stream*>(s->ctx);
            // A few bytes at a time, as some hosts do.
            const uint64_t k = n < 7 ? n : 7;
            self->data.insert(self->data.end(), (const char*)b, (const char*)b + k);
            return (int64_t)k;
        };
        in.ctx = this;
        in.read = [](const clap_istream_t* s, void* b, uint64_t n) -> int64_t
        {
            auto* self = static_cast<Stream*>(s->ctx);
            uint64_t left = self->data.size() - self->readAt;
            uint64_t k = n < left ? n : left;
            if (k > 5) k = 5;
            std::memcpy(b, self->data.data() + self->readAt, (size_t)k);
            self->readAt += (size_t)k;
            return (int64_t)k;
        };
    }
};

uint32_t g_seed = 12345;
double Rand01()
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) / 16777216.0;
}

std::vector<clap_param_info_t> Params(Instance& in)
{
    std::vector<clap_param_info_t> out;
    const uint32_t n = in.params->count(in.p);
    for (uint32_t i = 0; i < n; ++i)
    {
        clap_param_info_t info{};
        in.params->get_info(in.p, i, &info);
        out.push_back(info);
    }
    return out;
}


// --- the checks -------------------------------------------------------------------

void CheckText(Instance& in)
{
    for (const auto& info : Params(in))
    {
        // A choice built before its list of choices existed - which is what
        // static initialisation order did under the Audio Unit wrapper - has
        // a range that ends below where it starts.
        CHECK(info.max_value > info.min_value, "%s: range %g to %g",
              info.name, info.min_value, info.max_value);
        // An Audio Unit's parameters must keep what they are set to, the
        // Buffer's buttons included.
        const double probe = info.min_value + 0.37 * (info.max_value - info.min_value);
        if (!(info.flags & CLAP_PARAM_IS_BYPASS) && std::strcmp(info.name, "Save to file") != 0)
        {
            in.Set(info.id, probe);
            CHECK(std::fabs(in.Get(info.id) - probe) < 1e-9, "%s: set to %g, holds %g",
                  info.name, probe, in.Get(info.id));
            in.Set(info.id, info.default_value);
        }

        // Three points along each range, not only the default, so a unit's
        // formatting is exercised where it says something different.
        const double pts[3] = { info.min_value, info.default_value, info.max_value };
        for (double v : pts)
        {
            char text[256] = { 0 };
            CHECK(in.params->value_to_text(in.p, info.id, v, text, sizeof(text)),
                  "%s: no text for %g", info.name, v);
            double back = -12345;
            const bool ok = in.params->text_to_value(in.p, info.id, text, &back);
            CHECK(ok, "%s: \"%s\" does not read back", info.name, text);
            if (!ok) continue;
            char again[256] = { 0 };
            in.params->value_to_text(in.p, info.id, back, again, sizeof(again));
            CHECK(std::strcmp(text, again) == 0,
                  "%s: \"%s\" reads back as \"%s\"", info.name, text, again);
        }
    }
}

// Where an impulse comes out, with the effect bypassed so only its delay is
// being measured.
int ImpulseDelay(Instance& in, double rate)
{
    const uint32_t block = 256;
    const uint32_t total = (uint32_t)(rate * 0.6);
    const uint32_t at = 1000;
    std::vector<float> iL(total, 0.0f), iR(total, 0.0f), oL(total, 0.0f), oR(total, 0.0f);
    iL[at] = iR[at] = 0.5f;
    for (uint32_t o = 0; o < total; o += block)
        in.Run(iL, iR, oL, oR, o, std::min(block, total - o));
    uint32_t best = 0;
    float peak = 0.0f;
    for (uint32_t i = 0; i < total; ++i)
        if (std::fabs(oL[i]) > peak) { peak = std::fabs(oL[i]); best = i; }
    if (peak < 0.05f) return -1;
    return (int)best - (int)at;
}

void CheckLatency(const clap_plugin_factory_t* f, const char* id, double rate,
                  const char* label, void (*setup)(Instance&))
{
    Instance in(f, id);
    in.Set(901, 1.0);                       // bypass
    if (setup) setup(in);
    CHECK(in.Activate(rate, 512), "%s: activate failed at %g", id, rate);
    if (!in.active) return;
    const int reported = (int)in.latency->get(in.p);
    const int measured = ImpulseDelay(in, rate);
    // RNNoise away from 48 kHz runs through two resamplers, whose delay is
    // only whole to within a sample or two.
    const int slack = std::strstr(id, "rnnoise") ? 3 : 0;
    CHECK(measured >= 0 && std::abs(measured - reported) <= slack,
          "%s %s at %g Hz: reports %d frames of latency, measures %d",
          id, label, rate, reported, measured);
}

void CheckRun(const clap_plugin_factory_t* f, const char* id, double rate,
              uint32_t inCh, uint32_t outCh)
{
    Instance in(f, id);
    CHECK(in.Layout(inCh, outCh), "%s: refused %u in, %u out", id, inCh, outCh);
    CHECK(in.Activate(rate, 1024), "%s: activate failed at %g", id, rate);
    if (!in.active) return;

    const auto params = Params(in);
    const uint32_t total = (uint32_t)(rate * 1.5);
    std::vector<float> iL(total), iR(total), oL(total, 0.0f), oR(total, 0.0f);
    for (uint32_t i = 0; i < total; ++i)
    {
        const double t = i / rate;
        iL[i] = (float)(0.4 * std::sin(2 * 3.14159265 * 220 * t) + 0.05 * (Rand01() - 0.5));
        iR[i] = (float)(0.4 * std::sin(2 * 3.14159265 * 330 * t) + 0.05 * (Rand01() - 0.5));
    }

    // Odd block sizes, a parameter change landing inside most blocks.
    const uint32_t sizes[] = { 1, 37, 512, 1024, 100, 3, 777 };
    uint32_t o = 0, s = 0;
    while (o < total)
    {
        const uint32_t n = std::min(sizes[s++ % 7], total - o);
        InEvents ev;
        if (n > 1 && Rand01() < 0.5)
        {
            const auto& info = params[(size_t)(Rand01() * params.size()) % params.size()];
            // Save writes a file into the user's Documents; that is checked
            // by hand, not on every build.
            if (std::strcmp(info.name, "Save to file") != 0 &&
                std::strcmp(info.name, "Bypass") != 0)
            {
                double v = info.min_value + Rand01() * (info.max_value - info.min_value);
                if (info.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
                ev.Add((uint32_t)(Rand01() * n), info.id, v);
            }
        }
        in.Run(iL, iR, oL, oR, o, n, &ev);
        o += n;
    }

    double worst = 0;
    bool finite = true;
    for (uint32_t i = 0; i < total; ++i)
    {
        if (!std::isfinite(oL[i]) || (outCh > 1 && !std::isfinite(oR[i]))) finite = false;
        worst = std::max(worst, (double)std::fabs(oL[i]));
        if (outCh > 1) worst = std::max(worst, (double)std::fabs(oR[i]));
    }
    CHECK(finite, "%s %u>%u at %g: produced a NaN or infinity", id, inCh, outCh, rate);
    // Loud is allowed: four parametric bands at +18 dB on one frequency is
    // +72 dB, which takes this input past 1500. What is not allowed is a
    // filter running away, which goes far past anything settings explain.
    CHECK(worst < 10000.0, "%s %u>%u at %g: peak of %g", id, inCh, outCh, rate, worst);
}

void CheckState(const clap_plugin_factory_t* f, const char* id)
{
    Instance a(f, id);
    const auto params = Params(a);
    for (const auto& info : params)
    {
        double v = info.min_value + Rand01() * (info.max_value - info.min_value);
        if (info.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
        a.Set(info.id, v);
    }
    Stream s;
    CHECK(a.state->save(a.p, &s.out), "%s: save failed", id);

    Instance b(f, id);
    CHECK(b.state->load(b.p, &s.in), "%s: load failed", id);
    for (const auto& info : params)
    {
        const double va = a.Get(info.id), vb = b.Get(info.id);
        CHECK(std::fabs(va - vb) < 1e-5, "%s: %s saved as %g, loaded as %g",
              id, info.name, va, vb);
    }
}

}  // namespace

int main()
{
    if (!fastplugs_entry_init("")) { std::printf("entry init failed\n"); return 1; }
    const auto* f = (const clap_plugin_factory_t*)fastplugs_entry_get_factory(CLAP_PLUGIN_FACTORY_ID);
    const uint32_t count = f->get_plugin_count(f);
    std::printf("%u plugins\n", count);

    for (uint32_t i = 0; i < count; ++i)
    {
        const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
        std::printf("%-22s %s\n", d->name, d->id);
        const int before = g_failures;

        {
            Instance in(f, d->id);
            CHECK(in.p && in.params && in.state && in.latency && in.config,
                  "%s: missing an extension", d->id);
            if (!in.p) continue;
            CheckText(in);
        }

        for (double rate : { 44100.0, 48000.0 })
        {
            CheckRun(f, d->id, rate, 2, 2);
            CheckLatency(f, d->id, rate, "as it starts", nullptr);
        }
        CheckRun(f, d->id, 48000.0, 1, 1);
        CheckRun(f, d->id, 96000.0, 1, 2);
        CheckState(f, d->id);

        // Rates no sound card runs at. A plugin may refuse one; what it may
        // not do is take the host down with it.
        for (double rate : { 1000.0, 7350.0, 37000.0, 44100.5, 384000.0 })
        {
            Instance in(f, d->id);
            if (!in.Activate(rate, 256)) continue;
            std::vector<float> iL(4096, 0.1f), iR(4096, -0.1f), oL(4096), oR(4096);
            for (uint32_t o = 0; o < 4096; o += 256) in.Run(iL, iR, oL, oR, o, 256);
            bool finite = true;
            for (float v : oL) if (!std::isfinite(v)) finite = false;
            CHECK(finite, "%s at %g Hz: produced a NaN or infinity", d->id, rate);
        }

        if (g_failures == before) std::printf("  ok\n");
    }

    // Latency that changes with a setting, which is where it goes wrong.
    std::printf("Latency as settings change\n");
    const int before = g_failures;
    const std::string prefix = std::string(f->get_plugin_descriptor(f, 0)->id);
    const std::string base = prefix.substr(0, prefix.rfind('.') + 1);

    {
        // The pitch shifter only delays while it is shifting. Started at a
        // shift, its latency is reported; brought back to zero, the effect
        // stops delaying and the plugin has to make the difference up.
        Instance in(f, (base + "pitch").c_str());
        in.Set(901, 1.0);
        in.Set(0, 3.0);
        in.Activate(48000.0, 512);
        const int reported = (int)in.latency->get(in.p);
        CHECK(reported > 1000, "pitch at +3: reports only %d frames", reported);
        in.Set(0, 0.0);
        const int measured = ImpulseDelay(in, 48000.0);
        CHECK(measured == reported, "pitch back at 0: reports %d, measures %d", reported, measured);
        CHECK(!in.host.restartRequested, "pitch back at 0: asked for a restart it did not need");
    }
    {
        // Started at no shift, moving off zero needs latency it does not
        // have, which is a restart.
        Instance in(f, (base + "pitch").c_str());
        in.Activate(48000.0, 512);
        CHECK(in.latency->get(in.p) == 0, "pitch at 0 reports latency");
        in.Set(0, 5.0);
        CHECK(in.host.restartRequested, "pitch moved off 0: no restart requested");
        in.Deactivate();
        in.Set(901, 1.0);
        in.Activate(48000.0, 512);
        const int reported = (int)in.latency->get(in.p);
        const int measured = ImpulseDelay(in, 48000.0);
        CHECK(reported > 1000 && measured == reported,
              "pitch after restart: reports %d, measures %d", reported, measured);
    }
    {
        // Lookahead: lowered below where it started, the plugin pads.
        Instance in(f, (base + "limiter").c_str());
        in.Set(901, 1.0);
        in.Set(2, 20.0);
        in.Activate(48000.0, 512);
        const int reported = (int)in.latency->get(in.p);
        in.Set(2, 2.0);
        const int measured = ImpulseDelay(in, 48000.0);
        CHECK(reported == 960 && measured == reported,
              "limiter lookahead 20 then 2 ms: reports %d, measures %d", reported, measured);
    }
    if (g_failures == before) std::printf("  ok\n");

    // The buffer's Save writes a real file into Documents/FastPlugs, so it is
    // only exercised when asked for: FASTPLUGS_TEST_SAVE=1.
    if (std::getenv("FASTPLUGS_TEST_SAVE"))
    {
        std::printf("Buffer save\n");
        Instance in(f, (base + "buffer").c_str());
        in.Activate(48000.0, 512);
        const uint32_t total = 48000 * 2;
        std::vector<float> iL(total), iR(total), oL(total), oR(total);
        for (uint32_t i = 0; i < total; ++i)
            iL[i] = iR[i] = (float)(0.25 * std::sin(2 * 3.14159265 * 440 * i / 48000.0));
        for (uint32_t o = 0; o < total; o += 512)
            in.Run(iL, iR, oL, oR, o, std::min<uint32_t>(512, total - o));
        in.Set(5, 1.0);   // Save to file
        // The save happens on the buffer's own thread, polled every 50 ms.
        // Deactivating joins that thread, which is the wait.
        for (int i = 0; i < 20; ++i)
            for (uint32_t o = 0; o < 4800; o += 480) in.Run(iL, iR, oL, oR, o, 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        in.Deactivate();
        std::printf("  saved into %ls\n", DefaultSaveFolder().c_str());
    }

    fastplugs_entry_deinit();
    std::printf(g_failures ? "%d FAILED\n" : "All passed\n", g_failures);
    return g_failures ? 1 : 0;
}
