#include "effects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fp
{

int ToInt(double v) { return (int)std::lround(v); }

namespace {

// ---------------------------------------------------------------------------
// Frequencies
//
// A host draws every parameter as a straight line from its minimum to its
// maximum. Twenty to twenty thousand Hz on a straight line puts everything
// below a kilohertz into the first five per cent of the travel, which is
// where most of what an EQ is for happens. So the value a host sees is the
// frequency's log2 - a line in octaves - and it reads back as Hz.
// ---------------------------------------------------------------------------
double HzToValue(double hz) { return std::log2(hz); }
int    ValueToHz(double v)  { return (int)std::lround(std::exp2(v)); }

ParamDef P(clap_id id, const char* name, const char* module,
           double min, double max, double def, Unit unit)
{
    return MakeParam(id, name, module, min, max, def, unit);
}

ParamDef Choice(clap_id id, const char* name, const char* module,
                std::vector<std::string> choices, int def)
{
    return MakeChoice(id, name, module, std::move(choices), def);
}

ParamDef Toggle(clap_id id, const char* name, const char* module, bool def)
{
    return P(id, name, module, 0, 1, def ? 1 : 0, Unit::Toggle);
}

ParamDef Fixed(ParamDef p)
{
    p.automatable = false;
    return p;
}

ParamDef Trigger(clap_id id, const char* name, int action)
{
    ParamDef p = P(id, name, "Actions", 0, 1, 0, Unit::Trigger);
    p.action = action;
    return p;
}

// The Speex suppression slider's range. FastRoute keeps it in the dialog
// rather than in model.h, so it is repeated here.
const int kSuppressMin = 3, kSuppressMax = 45;

std::vector<EffectDef> Build()
{
    // FastRoute's own wording, from model.cpp, so a setting reads the same in
    // a host as it does in a FastRoute block.
    //
    // Local, not file scope: the Audio Unit wrapper calls the entry's init -
    // and so this - while the binary's static constructors are still running,
    // and a file scope vector here may not have been built yet. It was not,
    // on the Mac: every choice came out with no choices at all.
    const std::vector<std::string> kParaShapes = {
        "Off", "Peaking", "Low shelf", "High shelf", "Low pass", "High pass" };
    const std::vector<std::string> kPitchQualities = {
        "Best, about 120 ms of delay",
        "Balanced, about 60 ms of delay",
        "Low delay, about 30 ms" };

    const Block d;   // FastRoute's defaults
    std::vector<EffectDef> all;

    auto add = [&all](const char* key, const char* name, const char* desc,
                      std::vector<const char*> features, const char* subtype,
                      std::unique_ptr<Effect> (*make)()) -> EffectDef&
    {
        return AddEffect(all, key, name, desc, std::move(features), subtype, make);
    };

    // --- Denoisers -------------------------------------------------------
    {
        EffectDef& e = add("speex-denoise", "Speex Denoise",
            "SpeexDSP noise suppression, at any sample rate. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_RESTORATION },
            "Spdn", MakeSpeexDenoise);
        e.params = { P(0, "Suppression", "", kSuppressMin, kSuppressMax, d.suppressDb, Unit::Db) };
        e.toBlock = [](const double* v, Block& b) { b.suppressDb = ToInt(v[0]); };
        // FastRoute runs Speex on 10 ms frames, and Speex's FFT is that long.
        // KissFFT can only factor a length into primes up to 17, and on
        // anything else it calls exit() - from inside the host. Every rate a
        // sound card actually runs at is fine; 37 kHz, say, is not.
        e.refuseRate = [](int rate) -> std::string {
            int n = rate / 100;
            if (n < 8) return "Speex Denoise cannot run below 800 Hz.";
            for (int f = 2; f <= 17; ++f)
                while (n % f == 0) n /= f;
            if (n != 1)
                return "Speex Denoise cannot run at " + std::to_string(rate) +
                       " Hz: its 10 ms frame would be a length its FFT cannot "
                       "take. Any standard rate works.";
            return std::string();
        };
    }
    {
        EffectDef& e = add("rnnoise", "RNNoise Denoise",
            "RNNoise neural network noise suppression. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_RESTORATION },
            "RNns", MakeRnnDenoise);
        e.toBlock = [](const double*, Block&) {};
    }

    // --- EQ ----------------------------------------------------------------
    {
        EffectDef& e = add("tone", "Bass and Treble",
            "A low shelf at 100 Hz and a high shelf at 4 kHz. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER },
            "Tone", MakeToneControl);
        e.params = {
            P(0, "Bass",   "", -15, 15, d.bassDb,   Unit::GainDb),
            P(1, "Treble", "", -15, 15, d.trebleDb, Unit::GainDb),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.bassDb = ToInt(v[0]);
            b.trebleDb = ToInt(v[1]);
        };
    }
    {
        EffectDef& e = add("eq10", "10 Band EQ",
            "Ten band graphic equaliser, one octave apart. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER },
            "Eq10", MakeEqualiser);
        for (int i = 0; i < kEqBands; ++i)
        {
            const int hz = EqBandHz()[i];
            char name[32];
            if (hz >= 1000) std::snprintf(name, sizeof(name), "%d kHz", hz / 1000);
            else            std::snprintf(name, sizeof(name), "%d Hz", hz);
            e.params.push_back(P((clap_id)i, name, "Bands", -12, 12, d.eqDb[i], Unit::GainDb));
        }
        e.toBlock = [](const double* v, Block& b) {
            for (int i = 0; i < kEqBands; ++i) b.eqDb[i] = ToInt(v[i]);
        };
    }
    {
        EffectDef& e = add("para-eq", "4 Band Parametric EQ",
            "Four bands, each a peak, a shelf or a pass filter. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
              CLAP_PLUGIN_FEATURE_FILTER },
            "PEq4", MakeParametricEq);
        static const char* const kModules[kParaBands] = { "Band 1", "Band 2", "Band 3", "Band 4" };
        for (int i = 0; i < kParaBands; ++i)
        {
            const std::string band = kModules[i];
            const clap_id base = (clap_id)(i * 4);
            e.params.push_back(Choice(base + 0, (band + " shape").c_str(), kModules[i],
                                      kParaShapes, d.paraType[i]));
            e.params.push_back(P(base + 1, (band + " frequency").c_str(), kModules[i],
                                 HzToValue(kParaHzMin), HzToValue(kParaHzMax),
                                 HzToValue(d.paraHz[i]), Unit::Hz));
            e.params.push_back(P(base + 2, (band + " gain").c_str(), kModules[i],
                                 kParaGainMin, kParaGainMax, d.paraGainDb[i], Unit::GainDb));
            e.params.push_back(P(base + 3, (band + " Q").c_str(), kModules[i],
                                 kParaQMin / 10.0, kParaQMax / 10.0, d.paraQ[i] / 10.0, Unit::Q));
        }
        e.toBlock = [](const double* v, Block& b) {
            for (int i = 0; i < kParaBands; ++i)
            {
                b.paraType[i]   = ToInt(v[i * 4 + 0]);
                b.paraHz[i]     = ValueToHz(v[i * 4 + 1]);
                b.paraGainDb[i] = ToInt(v[i * 4 + 2]);
                b.paraQ[i]      = ToInt(v[i * 4 + 3] * 10.0);
            }
        };
    }

    // --- Channels ------------------------------------------------------------
    {
        EffectDef& e = add("channels", "Channel Mixer",
            "Level and pan for each channel, then width, swap and centre cancel. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_MIXING,
              CLAP_PLUGIN_FEATURE_UTILITY },
            "ChMx", MakeChannelMixer);
        e.params = {
            P(0, "Left level",  "Left",  kChanGainMin, kChanGainMax, d.chanGainDb[0], Unit::ChanLevel),
            P(1, "Left pan",    "Left",  kChanPanMin,  kChanPanMax,  d.chanPan[0],    Unit::Pan),
            P(2, "Right level", "Right", kChanGainMin, kChanGainMax, d.chanGainDb[1], Unit::ChanLevel),
            P(3, "Right pan",   "Right", kChanPanMin,  kChanPanMax,  d.chanPan[1],    Unit::Pan),
            P(4, "Width", "", 0, 200, d.widthPct, Unit::Percent),
            Toggle(5, "Swap channels", "", d.swapChannels),
            Toggle(6, "Centre cancel", "", d.centreCancel),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.chanGainDb[0] = ToInt(v[0]);
            b.chanPan[0]    = ToInt(v[1]);
            b.chanGainDb[1] = ToInt(v[2]);
            b.chanPan[1]    = ToInt(v[3]);
            b.widthPct      = ToInt(v[4]);
            b.swapChannels  = v[5] >= 0.5;
            b.centreCancel  = v[6] >= 0.5;
        };
    }

    // --- Dynamics ------------------------------------------------------------
    {
        EffectDef& e = add("compressor", "Compressor",
            "Stereo linked compressor with a 6 dB soft knee. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR },
            "Comp", MakeCompressor);
        e.params = {
            P(0, "Threshold", "", kCompThreshMin, kCompThreshMax, d.compThreshDb, Unit::Db),
            P(1, "Ratio", "", kCompRatioMin, kCompRatioMax, d.compRatio, Unit::Ratio),
            P(2, "Attack", "", kCompAttackMin, kCompAttackMax, d.compAttackMs, Unit::Ms),
            P(3, "Release", "", kCompReleaseMin, kCompReleaseMax, d.compReleaseMs, Unit::Ms),
            Toggle(4, "Automatic makeup gain", "", d.compAutoMakeup),
            P(5, "Makeup gain", "", 0, kCompMakeupMax, d.compMakeupDb, Unit::GainDb),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.compThreshDb   = ToInt(v[0]);
            b.compRatio      = ToInt(v[1]);
            b.compAttackMs   = ToInt(v[2]);
            b.compReleaseMs  = ToInt(v[3]);
            b.compAutoMakeup = v[4] >= 0.5;
            b.compMakeupDb   = ToInt(v[5]);
        };
    }
    {
        EffectDef& e = add("limiter", "Limiter",
            "Brickwall limiter with lookahead. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_LIMITER },
            "Limt", MakeLimiter);
        e.params = {
            P(0, "Ceiling", "", kLimCeilingMin, kLimCeilingMax, d.limCeilingDb, Unit::Db),
            P(1, "Release", "", kLimReleaseMin, kLimReleaseMax, d.limReleaseMs, Unit::Ms),
            Fixed(P(2, "Lookahead", "", kLimLookaheadMin, kLimLookaheadMax, d.limLookaheadMs, Unit::Ms)),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.limCeilingDb   = ToInt(v[0]);
            b.limReleaseMs   = ToInt(v[1]);
            b.limLookaheadMs = ToInt(v[2]);
        };
    }
    {
        EffectDef& e = add("gate", "Noise Gate",
            "Noise gate with hysteresis, hold and lookahead. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_GATE },
            "Gate", MakeNoiseGate);
        e.params = {
            P(0, "Threshold", "", kGateThreshMin, kGateThreshMax, d.gateThreshDb, Unit::Db),
            P(1, "Range", "", kGateRangeMin, kGateRangeMax, d.gateRangeDb, Unit::Db),
            P(2, "Attack", "", kGateAttackMin, kGateAttackMax, d.gateAttackMs, Unit::Ms),
            P(3, "Hold", "", kGateHoldMin, kGateHoldMax, d.gateHoldMs, Unit::Ms),
            P(4, "Release", "", kGateReleaseMin, kGateReleaseMax, d.gateReleaseMs, Unit::Ms),
            Fixed(P(5, "Lookahead", "", kGateLookaheadMin, kGateLookaheadMax, d.gateLookaheadMs, Unit::Ms)),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.gateThreshDb    = ToInt(v[0]);
            b.gateRangeDb     = ToInt(v[1]);
            b.gateAttackMs    = ToInt(v[2]);
            b.gateHoldMs      = ToInt(v[3]);
            b.gateReleaseMs   = ToInt(v[4]);
            b.gateLookaheadMs = ToInt(v[5]);
        };
    }
    {
        EffectDef& e = add("normaliser", "Normaliser",
            "Brings the level towards a target, with lookahead and a boost limit. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
              CLAP_PLUGIN_FEATURE_MASTERING },
            "Norm", MakeNormaliser);
        e.params = {
            P(0, "Target", "", kNormTargetMin, kNormTargetMax, d.normTargetDb, Unit::Db),
            P(1, "Maximum boost", "", kNormMaxGainMin, kNormMaxGainMax, d.normMaxGainDb, Unit::GainDb),
            P(2, "Release", "", kNormReleaseMin, kNormReleaseMax, d.normReleaseMs, Unit::Ms),
            Fixed(P(3, "Lookahead", "", kNormLookaheadMin, kNormLookaheadMax, d.normLookaheadMs, Unit::Ms)),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.normTargetDb    = ToInt(v[0]);
            b.normMaxGainDb   = ToInt(v[1]);
            b.normReleaseMs   = ToInt(v[2]);
            b.normLookaheadMs = ToInt(v[3]);
        };
    }

    // --- Pitch ---------------------------------------------------------------
    {
        EffectDef& e = add("pitch", "Pitch Shifter",
            "Moves the pitch without changing the speed, using Signalsmith Stretch. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_PITCH_SHIFTER },
            "Ptch", MakePitchShift);
        e.params = {
            P(0, "Semitones", "", kPitchSemitonesMin, kPitchSemitonesMax, d.pitchSemitones, Unit::Semitones),
            P(1, "Cents", "", kPitchCentsMin, kPitchCentsMax, d.pitchCents, Unit::Cents),
            Fixed(Choice(2, "Quality", "", kPitchQualities, d.pitchQuality)),
        };
        e.toBlock = [](const double* v, Block& b) {
            b.pitchSemitones = ToInt(v[0]);
            b.pitchCents     = ToInt(v[1]);
            b.pitchQuality   = ToInt(v[2]);
        };
    }

    // --- Watching and keeping --------------------------------------------------
    {
        EffectDef& e = add("clip-monitor", "Clipping Monitor",
            "Beeps when the signal reaches the threshold. Changes nothing it is given. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_ANALYZER },
            "Clip", MakeClipMonitor);
        e.params = {
            P(0, "Threshold", "", kClipThreshMin, kClipThreshMax, d.clipThreshDb, Unit::ClipThresh),
            P(1, "Least time between beeps", "", kClipIntervalMin, kClipIntervalMax, d.clipIntervalS, Unit::Seconds),
        };
        // A monitor passes the audio through untouched, so there is nothing for
        // a mix to blend. Bypass is what stops it watching.
        e.hasMix = false;
        e.toBlock = [](const double* v, Block& b) {
            b.clipThreshDb  = ToInt(v[0]);
            b.clipIntervalS = ToInt(v[1]);
        };
    }
    {
        EffectDef& e = add("buffer", "Buffer",
            "Keeps the last minutes of audio, rewinds through them, and saves them as a WAV. From FastRoute.",
            { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
              CLAP_PLUGIN_FEATURE_DELAY },
            "Bufr", MakeBuffer);
        e.params = {
            Fixed(P(0, "Length", "", kBufSecondsMin, kBufSecondsMax, d.bufSeconds, Unit::Seconds)),
            P(1, "Skip", "", kBufStepMin, kBufStepMax, d.bufStepSec, Unit::Seconds),
            Trigger(2, "Rewind", BA_REWIND),
            Trigger(3, "Fast forward", BA_FORWARD),
            Trigger(4, "Jump to live", BA_LIVE),
            Trigger(5, "Save to file", BA_SAVE),
        };
        e.isBuffer = true;
        e.toBlock = [](const double* v, Block& b) {
            b.bufSeconds = ToInt(v[0]);
            b.bufStepSec = ToInt(v[1]);
        };
    }

    AddFastPlayEffects(all);

    // --- What every one of them has -------------------------------------------
    for (EffectDef& e : all)
    {
        if (e.hasMix)
            e.params.push_back(P(kMixId, "Mix", "", 0, 100, 100, Unit::Percent));
        e.params.push_back(P(kBypassId, "Bypass", "", 0, 1, 0, Unit::Bypass));

        e.clapId = std::string(FASTPLUGS_ID_PREFIX) + "." + e.key;

        e.desc.clap_version = CLAP_VERSION_INIT;
        e.desc.id = e.clapId.c_str();
        e.desc.name = e.name.c_str();
        e.desc.vendor = FASTPLUGS_VENDOR;
        e.desc.url = FASTPLUGS_URL;
        e.desc.manual_url = FASTPLUGS_URL;
        e.desc.support_url = FASTPLUGS_URL;
        e.desc.version = FASTPLUGS_VERSION;
        e.desc.description = e.description.c_str();
        e.desc.features = e.features.data();
    }
    return all;
}

std::string Lower(const char* s)
{
    std::string out;
    for (; s && *s; ++s) out += (char)std::tolower((unsigned char)*s);
    return out;
}

// The first number in `s`, if there is one.
bool FirstNumber(const std::string& s, double* out)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (std::isdigit((unsigned char)c) || c == '.' ||
            ((c == '-' || c == '+') && i + 1 < s.size() &&
             (std::isdigit((unsigned char)s[i + 1]) || s[i + 1] == '.')))
        {
            char* end = nullptr;
            const double v = std::strtod(s.c_str() + i, &end);
            if (end != s.c_str() + i) { *out = v; return true; }
        }
    }
    return false;
}

std::string Fmt(const char* fmt, double a)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, a);
    return buf;
}

std::string FmtI(const char* fmt, int a)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, a);
    return buf;
}

}  // namespace

ParamDef MakeParam(clap_id id, const char* name, const char* module,
                   double min, double max, double def, Unit unit)
{
    ParamDef p;
    p.id = id;
    p.name = name;
    p.module = module;
    p.min = min;
    p.max = max;
    p.def = def < min ? min : (def > max ? max : def);
    p.unit = unit;
    return p;
}

ParamDef MakeChoice(clap_id id, const char* name, const char* module,
                    std::vector<std::string> choices, int def)
{
    ParamDef p = MakeParam(id, name, module, 0, (double)choices.size() - 1, def, Unit::Choice);
    p.choices = std::move(choices);
    return p;
}

EffectDef& AddEffect(std::vector<EffectDef>& all, const char* key, const char* name,
                     const char* desc, std::vector<const char*> features,
                     const char* subtype, std::unique_ptr<Effect> (*make)())
{
    EffectDef e;
    e.key = key;
    e.name = name;
    e.description = desc;
    e.features = std::move(features);
    e.features.push_back(CLAP_PLUGIN_FEATURE_STEREO);
    e.features.push_back(CLAP_PLUGIN_FEATURE_MONO);
    e.features.push_back(nullptr);
    std::memcpy(e.auSubtype, subtype, 4);
    e.make = make;
    all.push_back(std::move(e));
    return all.back();
}

const std::vector<EffectDef>& Effects()
{
    static const std::vector<EffectDef> all = Build();
    return all;
}

int ParamIndex(const EffectDef& def, clap_id id)
{
    for (size_t i = 0; i < def.params.size(); ++i)
        if (def.params[i].id == id) return (int)i;
    return -1;
}

std::string ValueText(const ParamDef& p, double v)
{
    const int i = ToInt(v);
    switch (p.unit)
    {
    case Unit::Db:        return FmtI("%d dB", i);
    case Unit::GainDb:    return i == 0 ? "0 dB" : FmtI("%+d dB", i);
    case Unit::ChanLevel: return i <= (int)p.min ? "Off" : (i == 0 ? "0 dB" : FmtI("%+d dB", i));
    case Unit::ClipThresh:return i >= 0 ? "Full scale" : FmtI("%d dB", i);
    case Unit::Percent:   return FmtI("%d%%", i);
    case Unit::Ms:        return FmtI("%d ms", i);
    case Unit::Seconds:   return i == 1 ? "1 second" : FmtI("%d seconds", i);
    case Unit::Hz:
    {
        const int hz = ValueToHz(v);
        return hz < 1000 ? FmtI("%d Hz", hz) : Fmt("%.1f kHz", hz / 1000.0);
    }
    case Unit::Ratio:     return i <= 1 ? "1:1, no compression" : FmtI("%d:1", i);
    case Unit::Semitones: return i == 0 ? "0 semitones"
                               : (i == 1 || i == -1) ? FmtI("%+d semitone", i)
                               : FmtI("%+d semitones", i);
    case Unit::Cents:     return i == 0 ? "0 cents" : FmtI("%+d cents", i);
    case Unit::Pan:
        // FastRoute's PanText: how far over, and which way, in words.
        if (i <= (int)p.min) return "Hard left";
        if (i >= (int)p.max) return "Hard right";
        if (i == 0) return "Centre";
        return i < 0 ? FmtI("Left %d", -i) : FmtI("Right %d", i);
    case Unit::Q:         return Fmt("%.1f", std::round(v * 10.0) / 10.0);
    case Unit::Choice:
    {
        const int c = std::max(0, std::min(i, (int)p.choices.size() - 1));
        return p.choices.empty() ? std::string() : p.choices[(size_t)c];
    }
    case Unit::Toggle:
    case Unit::Trigger:   return v >= 0.5 ? "On" : "Off";
    case Unit::Bypass:    return v >= 0.5 ? "Bypassed" : "Active";
    case Unit::HzOff:
    case Unit::HighCut:
    case Unit::HzPlain:
    {
        if (p.unit == Unit::HzOff && i <= (int)p.min) return "Off";
        if (p.unit == Unit::HighCut && i >= (int)p.max) return "Off";
        return i < 1000 ? FmtI("%d Hz", i) : Fmt("%.1f kHz", i / 1000.0);
    }
    case Unit::SecondsF:  return Fmt("%.2f s", v);
    case Unit::Multiplier:return Fmt("%.2fx", v);
    case Unit::Degrees:   return i == 1 || i == -1 ? FmtI("%d degree", i) : FmtI("%d degrees", i);
    case Unit::Metres10:
    {
        const double m = std::round(v) / 10.0;
        return m == 0.0 ? "0.0 m" : Fmt("%+.1f m", m);
    }
    case Unit::CentreAmount:
        if (i == 0) return "Off";
        // FastPlay's spelling, as the plugin's own name has it.
        return i > 0 ? FmtI("Remove center %d%%", i) : FmtI("Extract center %d%%", -i);
    }
    return std::string();
}

bool TextValue(const ParamDef& p, const char* text, double* out)
{
    const std::string s = Lower(text);
    double v = 0;
    bool ok = false;

    switch (p.unit)
    {
    case Unit::Choice:
        // The whole name first, so a name that begins another earlier in the
        // list ("Hall" after "Hallway") still reads back as itself.
        for (size_t c = 0; c < p.choices.size() && !ok; ++c)
            if (Lower(p.choices[c].c_str()) == s) { v = (double)c; ok = true; }
        for (size_t c = 0; c < p.choices.size() && !ok; ++c)
        {
            const std::string name = Lower(p.choices[c].c_str());
            if (!s.empty() && name.compare(0, s.size(), s) == 0) { v = (double)c; ok = true; }
        }
        if (!ok && FirstNumber(s, &v)) { v = std::round(v); ok = true; }
        break;

    case Unit::Toggle:
    case Unit::Trigger:
    case Unit::Bypass:
        if (s == "on" || s == "yes" || s == "true" || s == "1" || s == "bypassed")
            { v = 1; ok = true; }
        else if (s == "off" || s == "no" || s == "false" || s == "0" || s == "active")
            { v = 0; ok = true; }
        break;

    case Unit::Pan:
        if (s.find("cent") != std::string::npos || s == "c") { v = 0; ok = true; break; }
        if (s.find("left") != std::string::npos || (!s.empty() && s[0] == 'l'))
        {
            double n = 100;
            if (s.find("hard") == std::string::npos) FirstNumber(s, &n);
            v = -std::fabs(n); ok = true;
        }
        else if (s.find("right") != std::string::npos || (!s.empty() && s[0] == 'r'))
        {
            double n = 100;
            if (s.find("hard") == std::string::npos) FirstNumber(s, &n);
            v = std::fabs(n); ok = true;
        }
        else ok = FirstNumber(s, &v);
        break;

    case Unit::ChanLevel:
        if (s.find("off") != std::string::npos) { v = p.min; ok = true; }
        else ok = FirstNumber(s, &v);
        break;

    case Unit::ClipThresh:
        if (s.find("full") != std::string::npos) { v = 0; ok = true; }
        else ok = FirstNumber(s, &v);
        break;

    case Unit::Hz:
    {
        double hz = 0;
        if (FirstNumber(s, &hz))
        {
            if (s.find('k') != std::string::npos) hz *= 1000.0;
            if (hz > 0) { v = HzToValue(hz); ok = true; }
        }
        break;
    }

    case Unit::HzOff:
    case Unit::HighCut:
    case Unit::HzPlain:
        if (s.find("off") != std::string::npos)
        {
            v = (p.unit == Unit::HighCut) ? p.max : p.min;
            ok = (p.unit != Unit::HzPlain);
            break;
        }
        ok = FirstNumber(s, &v);
        if (ok && s.find('k') != std::string::npos) v *= 1000.0;
        break;

    case Unit::Metres10:
        ok = FirstNumber(s, &v);
        v *= 10.0;
        break;

    case Unit::CentreAmount:
        if (s.find("off") != std::string::npos) { v = 0; ok = true; break; }
        ok = FirstNumber(s, &v);
        if (ok && s.find("extract") != std::string::npos) v = -std::fabs(v);
        else if (ok && (s.find("remove") != std::string::npos ||
                        s.find("cancel") != std::string::npos)) v = std::fabs(v);
        break;

    default:
        ok = FirstNumber(s, &v);
        break;
    }

    if (!ok) return false;
    *out = std::max(p.min, std::min(p.max, v));
    return true;
}

}  // namespace fp
