// ---------------------------------------------------------------------------
// FastPlay's effects
//
// The ones FastRoute does not already have: the echo, the two reverbs, the
// spectral centre canceller and the 3D audio. Each is FastPlay's own class,
// compiled from FastPlay's source as it stands, and each adapter here does what
// FastPlay's EffectChain does around it - the reverb mix law, the presets, the
// dry ramp - using the code cut from effect_chain.cpp at configure time (see
// cmake/dependencies.cmake), so the ranges, defaults and presets are FastPlay's.
//
// Left out, because FastPlugs already has them: the normaliser (FastRoute's own,
// which FastPlay took), the compressor, the three band EQ and stereo width. Also
// left out: the convolution reverb, which needs an impulse response file, and a
// plugin parameter cannot name a file.
// ---------------------------------------------------------------------------
#include "effects.h"

#include "types.h"
#include "reverb/reverb.h"
#include "reverb/efx_reverb.h"
#include "audio/basic_effects.h"
#include "center_cancel.h"
#include "spatial_audio.h"
#include "speakers/presets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// FastPlay's parameter table, reverb presets and mix law, verbatim.
namespace {
#include "fastplay_tables.inc"
}

namespace fp
{
namespace
{

// FastPlay's range and default for one of its parameters.
const ::ParamDef& Spec(ParamId id)
{
    for (const auto& d : g_paramDefs)
        if (d.id == id) return d;
    return g_paramDefs[0];
}

fp::ParamDef From(clap_id id, ParamId fpId, const char* name, Unit unit, bool automatable = true)
{
    const ::ParamDef& s = Spec(fpId);
    fp::ParamDef p = MakeParam(id, name, "", s.minValue, s.maxValue, s.defaultValue, unit);
    p.automatable = automatable;
    return p;
}

// The most an adapter is handed at once. The plugin never hands more.
constexpr size_t kScratch = 4096;

// Interleaved stereo, delayed in place by a fixed number of frames.
class StereoDelay
{
public:
    void Init(size_t frames)
    {
        m_len = frames;
        m_ring.assign((frames + 1) * 2, 0.0f);
        m_pos = 0;
    }

    void Process(float* b, size_t n)
    {
        if (m_len == 0) return;
        const size_t cap = m_len + 1;
        for (size_t i = 0; i < n; ++i)
        {
            m_ring[m_pos * 2 + 0] = b[i * 2 + 0];
            m_ring[m_pos * 2 + 1] = b[i * 2 + 1];
            m_pos = (m_pos + 1) % cap;
            b[i * 2 + 0] = m_ring[m_pos * 2 + 0];
            b[i * 2 + 1] = m_ring[m_pos * 2 + 1];
        }
    }

private:
    std::vector<float> m_ring;
    size_t m_len = 0, m_pos = 0;
};

// What every adapter shares: FastRoute's Effect interface, which the plugin
// drives, and the dry/wet blend the plugin's bypass fades through. FastPlay's
// effects have no such blend of their own, so it is done here, with the dry
// signal delayed by the effect's latency so the two line up.
class FpEffect : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        m_rate = rate;
        m_dryDelay.Init((size_t)Latency());
        m_dry.assign(kScratch * 2, 0.0f);
        return Init(err);
    }

    int LatencyFrames() const override { return Latency(); }

    void Process(float* buf, size_t frames) override
    {
        for (size_t done = 0; done < frames; )
        {
            const size_t n = std::min(frames - done, kScratch);
            float* b = buf + done * 2;
            const float wet = m_wet.load();
            std::memcpy(m_dry.data(), b, n * 2 * sizeof(float));
            m_dryDelay.Process(m_dry.data(), n);   // fed always, so a fade starts clean
            Run(b, n);
            if (wet < 1.0f)
                for (size_t i = 0; i < n * 2; ++i)
                    b[i] = m_dry[i] + (b[i] - m_dry[i]) * wet;
            done += n;
        }
    }

protected:
    virtual bool Init(std::wstring* err) = 0;
    virtual void Run(float* b, size_t n) = 0;
    // Constant, by design: an effect whose latency moved with its settings
    // would have the host restarting it.
    virtual int Latency() const { return 0; }

    int m_rate = 48000;

private:
    StereoDelay m_dryDelay;
    std::vector<float> m_dry;
};

// ---------------------------------------------------------------------------
// Echo
// ---------------------------------------------------------------------------
class EchoFx : public FpEffect
{
public:
    // EffectChain::Impl::UpdateEcho.
    void Apply(const double* v)
    {
        ::audio::Echo::Params p;
        const float mix = (float)v[2] / 100.0f;
        p.dry = 1.0f - mix;
        p.wet = mix;
        p.feedback = (float)v[1] / 100.0f;
        p.delay = (float)v[0] / 1000.0f;
        p.stereo = true;
        m_echo.Set(p);
    }

protected:
    bool Init(std::wstring*) override { return true; }
    void Run(float* b, size_t n) override { m_echo.Process(b, (int)n, m_rate); }

private:
    ::audio::Echo m_echo;
};

// ---------------------------------------------------------------------------
// The reverbs. FastPlay runs both as one effect with an algorithm switch; here
// they are two plugins, each the same code path EffectChain's ReverbProc takes
// for its algorithm.
// ---------------------------------------------------------------------------
template <typename Engine>
class ReverbBase : public FpEffect
{
protected:
    bool Init(std::wstring*) override
    {
        m_engine.init(m_rate);
        for (auto* v : { &m_inL, &m_inR, &m_outL, &m_outR }) v->assign(kScratch, 0.0f);
        return true;
    }

    // The first settings start it from silence, as ResetReverb does.
    template <typename Params>
    void Set(const Params& p, float dry)
    {
        m_engine.set_params(p);
        m_dryTarget = dry;
        if (!m_primed)
        {
            m_engine.reset();
            m_dryCurrent = dry;
            m_primed = true;
        }
    }

    void Run(float* b, size_t n) override
    {
        for (size_t i = 0; i < n; ++i)
        {
            m_inL[i] = b[i * 2];
            m_inR[i] = b[i * 2 + 1];
        }
        std::fill(m_outL.begin(), m_outL.begin() + (long)n, 0.0f);
        std::fill(m_outR.begin(), m_outR.begin() + (long)n, 0.0f);
        m_engine.process(m_inL.data(), m_inR.data(), m_outL.data(), m_outR.data(), (int)n);

        // The dry gain ramps across the block, so moving the mix does not click.
        const float dry0 = m_dryCurrent, step = (m_dryTarget - m_dryCurrent) / (float)n;
        for (size_t i = 0; i < n; ++i)
        {
            const float dry = dry0 + step * (float)(i + 1);
            b[i * 2]     = b[i * 2] * dry + m_outL[i];
            b[i * 2 + 1] = b[i * 2 + 1] * dry + m_outR[i];
        }
        m_dryCurrent = m_dryTarget;
    }

    Engine m_engine;

private:
    std::vector<float> m_inL, m_inR, m_outL, m_outR;
    float m_dryTarget = 1.0f, m_dryCurrent = 1.0f;
    bool m_primed = false;
};

// Parameter order for the simple reverb.
enum { RV_ROOM, RV_MIX, RV_SIZE, RV_DAMP, RV_WIDTH, RV_PREDELAY, RV_LOWCUT, RV_HIGHCUT };

class ReverbFx : public ReverbBase<fastplay::audio::Reverb>
{
public:
    // The simple half of EffectChain::Impl::UpdateReverb.
    void Apply(const double* v)
    {
        const SimpleReverbPreset& room =
            g_simpleReverbPresets[clamp_val(ToInt(v[RV_ROOM]), 0, g_simpleReverbPresetCount - 1)];
        float dry, wet;
        ReverbMixGains((float)v[RV_MIX], dry, wet);
        fastplay::audio::ReverbParams sp;
        sp.room_size = (float)v[RV_SIZE] / 100.0f;
        sp.damping = (float)v[RV_DAMP] / 100.0f;
        sp.width = (float)v[RV_WIDTH] / 100.0f;
        sp.pre_delay_ms = (float)v[RV_PREDELAY];
        sp.low_cut_hz = (float)v[RV_LOWCUT];
        sp.high_cut_hz = v[RV_HIGHCUT] >= kReverbHighCutOff ? 0.0f : (float)v[RV_HIGHCUT];
        sp.wet = wet * room.level / 3.0f;   // the reverb's wet is scaled so 1/3 is unity
        sp.dry = 0.0f;                      // the dry signal is mixed here
        Set(sp, dry);
    }
};

// Parameter order for the advanced reverb.
enum { AR_ENV, AR_MIX, AR_DECAY, AR_HFRATIO, AR_DENSITY, AR_DIFFUSION,
       AR_REFL, AR_LATE, AR_REFLDELAY, AR_LATEDELAY };

class AdvancedReverbFx : public ReverbBase<fastplay::audio::EfxReverb>
{
public:
    // The advanced half of EffectChain::Impl::UpdateReverb.
    void Apply(const double* v)
    {
        fastplay::audio::EfxReverbParams ap =
            g_advancedReverbPresets[clamp_val(ToInt(v[AR_ENV]), 0, g_advancedReverbPresetCount - 1)].params;
        float dry, wet;
        ReverbMixGains((float)v[AR_MIX], dry, wet);
        ap.decay_time = (float)v[AR_DECAY];
        ap.decay_hf_ratio = (float)v[AR_HFRATIO];
        ap.density = (float)v[AR_DENSITY] / 100.0f;
        ap.diffusion = (float)v[AR_DIFFUSION] / 100.0f;
        ap.reflections_gain = std::pow(10.0f, (float)v[AR_REFL] / 20.0f);
        ap.late_reverb_gain = std::pow(10.0f, (float)v[AR_LATE] / 20.0f);
        ap.reflections_delay = (float)v[AR_REFLDELAY] / 1000.0f;
        ap.late_reverb_delay = (float)v[AR_LATEDELAY] / 1000.0f;
        ap.gain *= wet;
        Set(ap, dry);
    }
};

// ---------------------------------------------------------------------------
// Centre cancel
//
// FastPlay's is spectral when it removes the centre - an FFT of 4096 with 75%
// overlap, so 4095 frames late - and a plain time domain blend, with no delay
// at all, when it extracts it or does nothing. A latency that came and went as
// the amount crossed zero would restart the plugin every time, so it is 4095
// always: the two quick paths are delayed here to match.
// ---------------------------------------------------------------------------
class CentreCancelFx : public FpEffect
{
public:
    void Apply(const double* v) { m_amount = (float)v[0] / 100.0f; }

protected:
    static constexpr int kFft = 4096;   // CenterCancelProcessor::Init's default

    bool Init(std::wstring* err) override
    {
        if (!m_cc.Init(m_rate, kFft))
        {
            if (err) *err = L"Center Cancel could not be started.";
            return false;
        }
        m_delay.Init((size_t)Latency());
        m_out.assign(kScratch * 2, 0.0f);
        m_tmp.assign(kScratch * 2, 0.0f);
        return true;
    }

    int Latency() const override { return kFft - 1; }

    void Run(float* b, size_t n) override
    {
        int outFrames = 0;
        if (m_amount > 0.0f)
        {
            // Coming back to removal: what the FFT was holding is from before,
            // and would be played out first. It starts again from silence.
            if (!m_cancelling) { m_cc.Reset(); m_cancelling = true; }
            // The delay is kept fed, so going the other way is clean.
            std::memcpy(m_tmp.data(), b, n * 2 * sizeof(float));
            m_delay.Process(m_tmp.data(), n);
            m_cc.SetAmount(m_amount);
            m_cc.ProcessFloat(b, (int)n, m_out.data(), outFrames);
        }
        else
        {
            m_cancelling = false;
            m_delay.Process(b, n);
            m_cc.SetAmount(m_amount);
            m_cc.ProcessFloat(b, (int)n, m_out.data(), outFrames);
        }
        const size_t got = std::min((size_t)std::max(outFrames, 0), n);
        std::memcpy(b, m_out.data(), got * 2 * sizeof(float));
        std::fill(b + got * 2, b + n * 2, 0.0f);
    }

private:
    CenterCancelProcessor m_cc;
    StereoDelay m_delay;
    std::vector<float> m_out, m_tmp;
    float m_amount = 0.0f;
    bool m_cancelling = false;
};

// ---------------------------------------------------------------------------
// 3D audio
//
// Binaural and 5.1 come out one HRTF block (128 frames) late; the simulated
// rooms render in place. The rooms are delayed here by the same block, so the
// latency stays put whichever mode is chosen.
// ---------------------------------------------------------------------------
enum { SP_MODE, SP_BLEND, SP_WIDTH, SP_ROTATION, SP_REAR, SP_X, SP_Y, SP_Z,
       SP_SUB, SP_CROSSOVER, SP_CONE, SP_BASS };

class SpatialFx : public FpEffect
{
public:
    // EffectChain::Impl::ApplySpatialSettings.
    void Apply(const double* v)
    {
        SpatialAudio& s = *m_spatial;
        const SpatialMode was = s.GetMode();
        const int mode = ToInt(v[SP_MODE]);
        if (mode >= 2)
        {
            s.SetRoomPreset(mode - 2);
            s.SetMode(SpatialMode::Speakers);
        }
        else
        {
            s.SetMode(mode == 1 ? SpatialMode::Surround51 : SpatialMode::Binaural);
        }
        // SetMode empties the output queue but does not prime it again, so
        // after a change of mode the HRTF modes ran with no latency at all -
        // and, until whole blocks had built up, handed back blocks the length
        // of whatever was left over without processing them. Initialize puts
        // the block of silence back (the filters it keeps, the rate being the
        // same), which keeps the latency at the block the host was told of.
        if (s.GetMode() != was) s.Initialize(m_rate);
        s.SetRearCenter(v[SP_REAR] >= 0.5);
        s.SetSubwoofer(v[SP_SUB] >= 0.5);
        s.SetCrossover((float)v[SP_CROSSOVER]);
        s.SetConeNoise((float)v[SP_CONE] / 100.0f);
        s.SetBass((float)v[SP_BASS]);
        s.SetWidth((float)v[SP_WIDTH]);
        s.SetRotation((float)v[SP_ROTATION]);
        s.SetListenerOffset((float)v[SP_X], (float)v[SP_Y], (float)v[SP_Z]);
        m_blend = (float)v[SP_BLEND] / 100.0f;
    }

protected:
    bool Init(std::wstring* err) override
    {
        m_spatial.reset(new SpatialAudio());
        if (!m_spatial->Initialize(m_rate))
        {
            if (err) *err = m_spatial->GetLastError();
            return false;
        }
        m_delay.Init((size_t)Latency());
        return true;
    }

    int Latency() const override { return SpatialAudio::FRAME_SIZE; }

    void Run(float* b, size_t n) override
    {
        // At no blend FastPlay leaves the sound alone, and so undelayed; the
        // rooms render in place. Both are made up to the HRTF modes' block.
        const bool delayed = m_blend <= 0.0f || m_spatial->GetMode() == SpatialMode::Speakers;
        if (delayed && !m_wasDelayed) m_delay.Init((size_t)Latency());   // nothing stale
        m_wasDelayed = delayed;

        if (m_blend > 0.0f) m_spatial->Process(b, (int)n, m_blend);
        if (delayed) m_delay.Process(b, n);
    }

private:
    std::unique_ptr<SpatialAudio> m_spatial;
    StereoDelay m_delay;
    float m_blend = 1.0f;
    bool m_wasDelayed = false;
};

template <typename T>
std::unique_ptr<Effect> Make() { return std::unique_ptr<Effect>(new T()); }

template <typename T>
void ApplyTo(Effect& fx, const double* v) { static_cast<T&>(fx).Apply(v); }

uint32_t Frames(double seconds, int rate)
{
    if (seconds < 0) seconds = 0;
    if (seconds > 600) seconds = 600;
    return (uint32_t)(seconds * rate);
}

}  // namespace

void AddFastPlayEffects(std::vector<EffectDef>& all)
{
    // --- Echo ----------------------------------------------------------------
    {
        EffectDef& e = AddEffect(all, "echo", "Echo",
            "A stereo echo whose repeats bounce between left and right. From FastPlay.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY },
            "Echo", Make<EchoFx>);
        e.params = {
            From(0, ParamId::EchoDelay,    "Delay",    Unit::Ms),
            From(1, ParamId::EchoFeedback, "Feedback", Unit::Percent),
            From(2, ParamId::EchoMix,      "Mix",      Unit::Percent),
        };
        e.hasMix = false;
        e.apply = ApplyTo<EchoFx>;
        e.tailFrames = [](const double* v, int rate) {
            const double delay = v[0] / 1000.0, fb = v[1] / 100.0;
            // Repeats until they are 60 dB down.
            const double repeats = fb > 0.001 ? std::log(1e-3) / std::log(fb) : 0.0;
            return Frames(delay * (repeats + 1.0), rate);
        };
    }

    // --- Reverb --------------------------------------------------------------
    {
        std::vector<std::string> rooms;
        for (const auto& p : g_simpleReverbPresets) rooms.push_back(p.name);

        EffectDef& e = AddEffect(all, "reverb", "Reverb",
            "FastPlay's reverb: a room to start from, then size, damping, width and filters. From FastPlay.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB },
            "Rvrb", Make<ReverbFx>);
        fp::ParamDef room = MakeChoice(RV_ROOM, "Room", "", rooms, ToInt(Spec(ParamId::ReverbPreset).defaultValue));
        room.preset = true;
        e.params = {
            room,
            From(RV_MIX,      ParamId::ReverbMix,      "Mix",       Unit::Percent),
            From(RV_SIZE,     ParamId::ReverbRoom,     "Size",      Unit::Percent),
            From(RV_DAMP,     ParamId::ReverbDamp,     "Damping",   Unit::Percent),
            From(RV_WIDTH,    ParamId::ReverbWidth,    "Width",     Unit::Percent),
            From(RV_PREDELAY, ParamId::ReverbPreDelay, "Pre-delay", Unit::Ms),
            From(RV_LOWCUT,   ParamId::ReverbLowCut,   "Low cut",   Unit::HzOff),
            From(RV_HIGHCUT,  ParamId::ReverbHighCut,  "High cut",  Unit::HighCut),
        };
        e.hasMix = false;
        e.apply = ApplyTo<ReverbFx>;
        // EffectChain::Impl::ApplySimpleReverbPreset.
        e.presetValues = [](int, int choice, double* v) {
            const SimpleReverbPreset& p =
                g_simpleReverbPresets[clamp_val(choice, 0, g_simpleReverbPresetCount - 1)];
            v[RV_SIZE] = p.room * 100.0f;
            v[RV_DAMP] = p.damping * 100.0f;
            v[RV_WIDTH] = p.width * 100.0f;
            v[RV_PREDELAY] = p.preDelayMs;
            v[RV_LOWCUT] = p.lowCutHz;
            v[RV_HIGHCUT] = p.highCutHz > 0 ? p.highCutHz : kReverbHighCutOff;
        };
        e.tailFrames = [](const double* v, int rate) {
            // FastPlay's decay_seconds (src/reverb/reverb.cpp), which that file
            // keeps private: 0.6 s at size 0, 11 s at size 1.
            const double fb = v[RV_SIZE] / 100.0 * 0.28 + 0.7;
            const double decay = 3.0 * (1378.0 / 44100.0) / -std::log10(fb);
            return Frames(decay + v[RV_PREDELAY] / 1000.0, rate);
        };
    }

    // --- Advanced reverb -------------------------------------------------------
    {
        std::vector<std::string> envs;
        for (const auto& p : g_advancedReverbPresets) envs.push_back(p.name);

        EffectDef& e = AddEffect(all, "advanced-reverb", "Advanced Reverb",
            "FastPlay's EFX reverb, with OpenAL's environments to start from. From FastPlay.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB },
            "ARvb", Make<AdvancedReverbFx>);
        fp::ParamDef env = MakeChoice(AR_ENV, "Environment", "", envs,
                                      ToInt(Spec(ParamId::AdvReverbPreset).defaultValue));
        env.preset = true;
        e.params = {
            env,
            From(AR_MIX,       ParamId::AdvReverbMix,         "Mix",                     Unit::Percent),
            From(AR_DECAY,     ParamId::AdvReverbDecay,       "Decay",                   Unit::SecondsF),
            From(AR_HFRATIO,   ParamId::AdvReverbHFRatio,     "High frequency decay",    Unit::Multiplier),
            From(AR_DENSITY,   ParamId::AdvReverbDensity,     "Density",                 Unit::Percent),
            From(AR_DIFFUSION, ParamId::AdvReverbDiffusion,   "Diffusion",               Unit::Percent),
            From(AR_REFL,      ParamId::AdvReverbReflections, "Reflections level",       Unit::GainDb),
            From(AR_LATE,      ParamId::AdvReverbLate,        "Tail level",              Unit::GainDb),
            From(AR_REFLDELAY, ParamId::AdvReverbReflDelay,   "Reflections delay",       Unit::Ms),
            From(AR_LATEDELAY, ParamId::AdvReverbLateDelay,   "Tail delay",              Unit::Ms),
        };
        e.hasMix = false;
        e.apply = ApplyTo<AdvancedReverbFx>;
        // EffectChain::Impl::ApplyAdvancedReverbPreset.
        e.presetValues = [](int, int choice, double* v) {
            const fastplay::audio::EfxReverbParams& p =
                g_advancedReverbPresets[clamp_val(choice, 0, g_advancedReverbPresetCount - 1)].params;
            v[AR_DECAY] = p.decay_time;
            v[AR_HFRATIO] = p.decay_hf_ratio;
            v[AR_DENSITY] = p.density * 100.0f;
            v[AR_DIFFUSION] = p.diffusion * 100.0f;
            v[AR_REFL] = GainToDb(p.reflections_gain, -60.0f, 10.0f);
            v[AR_LATE] = GainToDb(p.late_reverb_gain, -60.0f, 20.0f);
            v[AR_REFLDELAY] = p.reflections_delay * 1000.0f;
            v[AR_LATEDELAY] = p.late_reverb_delay * 1000.0f;
        };
        e.tailFrames = [](const double* v, int rate) {
            // Decay is the time to fall 60 dB; a high frequency ratio above one
            // makes the top end last that much longer.
            return Frames(v[AR_DECAY] * std::max(1.0, v[AR_HFRATIO]) +
                          (v[AR_REFLDELAY] + v[AR_LATEDELAY]) / 1000.0, rate);
        };
    }

    // --- Center cancel ---------------------------------------------------------
    {
        EffectDef& e = AddEffect(all, "center-cancel", "Center Cancel",
            "Removes what is panned to the centre, usually the vocal, or keeps only that. From FastPlay.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
              CLAP_PLUGIN_FEATURE_MIXING },
            "CCnc", Make<CentreCancelFx>);
        e.params = { From(0, ParamId::CenterCancel, "Amount", Unit::CentreAmount) };
        e.hasMix = false;
        e.apply = ApplyTo<CentreCancelFx>;
    }

    // --- 3D audio -----------------------------------------------------------
    {
        std::vector<std::string> modes = { "Binaural", "5.1 Surround" };
        for (int i = 0; i < speakers::kRoomPresetCount; ++i)
            modes.push_back(speakers::RoomPresetName(i));

        EffectDef& e = AddEffect(all, "3d-audio", "3D Audio",
            "For headphones: virtual speakers around you, or real ones in a simulated room. From FastPlay.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
              CLAP_PLUGIN_FEATURE_SURROUND },
            "3DAu", Make<SpatialFx>);
        fp::ParamDef mode = MakeChoice(SP_MODE, "Mode", "", modes,
                                       ToInt(Spec(ParamId::SpatialMode).defaultValue));
        // Changing room rebuilds the whole simulation.
        mode.automatable = false;
        e.params = {
            mode,
            From(SP_BLEND,     ParamId::SpatialBlend,      "Blend",               Unit::Percent),
            From(SP_WIDTH,     ParamId::SpatialWidth,      "Speaker spread",      Unit::Degrees),
            From(SP_ROTATION,  ParamId::SpatialRotation,   "Rotation",            Unit::Degrees),
            From(SP_REAR,      ParamId::SpatialRearCenter, "Rear center speaker", Unit::Toggle),
            From(SP_X,         ParamId::SpatialX,          "Listener across",     Unit::Metres10),
            From(SP_Y,         ParamId::SpatialY,          "Listener forward",    Unit::Metres10),
            From(SP_Z,         ParamId::SpatialZ,          "Listener up",         Unit::Metres10),
            From(SP_SUB,       ParamId::SpatialSub,        "Subwoofer",           Unit::Toggle, false),
            From(SP_CROSSOVER, ParamId::SpatialCrossover,  "Crossover",           Unit::HzPlain, false),
            From(SP_CONE,      ParamId::SpatialConeNoise,  "Cone noise",          Unit::Percent),
            From(SP_BASS,      ParamId::SpatialBass,       "Bass",                Unit::GainDb),
        };
        e.hasMix = false;
        e.apply = ApplyTo<SpatialFx>;
        e.tailFrames = [](const double* v, int rate) {
            // The simulated rooms ring on; the HRTF modes barely do.
            return Frames(ToInt(v[SP_MODE]) >= 2 ? 4.0 : 0.05, rate);
        };
    }
}

}  // namespace fp
