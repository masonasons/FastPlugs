#pragma once
// ---------------------------------------------------------------------------
// What each of FastRoute's effects looks like from a host: its name, its
// parameters, and how those parameters become the Block that FastRoute's own
// Configure() reads.
//
// Every range and default comes from FastRoute's model.h and from a default
// constructed Block, so a range widened in FastRoute is a range widened here.
// ---------------------------------------------------------------------------
#include <clap/clap.h>

#include <memory>
#include <string>
#include <vector>

#include "audio.h"

namespace fp
{

// How a value reads, and how it is typed back in.
enum class Unit
{
    Db,          // "-20 dB"
    GainDb,      // "+3 dB", "0 dB"
    ChanLevel,   // a gain whose lowest setting is off
    ClipThresh,  // a level whose highest setting is "full scale"
    Percent,
    Ms,
    Seconds,
    Hz,          // the value is log2 of the frequency; see effects.cpp
    Ratio,
    Semitones,
    Cents,
    Pan,
    Q,
    Choice,
    Toggle,
    Trigger,     // a button: any change presses it
    Bypass,
    // FastPlay's, whose settings are not all whole numbers
    HzOff,       // a frequency in Hz whose lowest setting is off
    HighCut,     // a frequency in Hz whose highest setting is off
    HzPlain,     // a frequency in Hz, on a straight line
    SecondsF,    // "1.49 s"
    Multiplier,  // "0.83x"
    Degrees,
    Metres10,    // tenths of a metre, read as metres
    CentreAmount,// -100 extracts the centre, +100 removes it
};

struct ParamDef
{
    clap_id id = 0;
    std::string name;
    std::string module;
    double min = 0, max = 1, def = 0;
    Unit unit = Unit::Db;
    std::vector<std::string> choices;
    // False for a setting that reallocates or changes the latency when it
    // moves. Automating those would restart the plugin on every step.
    bool automatable = true;
    int action = -1;             // the BlockActionId a Trigger presses
    // A choice of preset: picking a different one sets other parameters, as
    // FastPlay's reverb rooms do. See EffectDef::presetValues.
    bool preset = false;
};

struct EffectDef
{
    std::string key;             // the last part of the plugin id
    std::string name;
    std::string description;
    std::vector<const char*> features;   // null terminated
    char auSubtype[5] = { 0 };
    std::unique_ptr<Effect> (*make)() = nullptr;
    std::vector<ParamDef> params;
    // FastRoute's effects: writes the parameters into `b`, which is handed to
    // the effect's Configure. `v` is indexed like `params`.
    void (*toBlock)(const double* v, Block& b) = nullptr;
    // Everything else: the parameters go to the effect directly.
    void (*apply)(Effect& fx, const double* v) = nullptr;
    // A preset parameter at `index` has moved to `choice`: write what that
    // preset sets into `v` (indexed like `params`, holding the current values).
    void (*presetValues)(int index, int choice, double* v) = nullptr;
    // How long the effect goes on sounding after its input stops, in frames.
    uint32_t (*tailFrames)(const double* v, int rate) = nullptr;
    bool hasMix = true;
    bool isBuffer = false;
    // Why the effect cannot run at this rate, or empty when it can. Checked
    // before Start, for an effect whose library would otherwise abort the
    // whole host rather than return an error.
    std::string (*refuseRate)(int rate) = nullptr;

    std::string clapId;
    clap_plugin_descriptor_t desc{};
};

// Shared by every plugin, so a host's bypass and a mix knob are always found
// in the same place.
constexpr clap_id kMixId    = 900;
constexpr clap_id kBypassId = 901;

const std::vector<EffectDef>& Effects();

// FastPlay's effects, added to the list (fastplay_fx.cpp).
void AddFastPlayEffects(std::vector<EffectDef>& all);

// The same constructor helpers effects.cpp uses, for fastplay_fx.cpp.
ParamDef MakeParam(clap_id id, const char* name, const char* module,
                   double min, double max, double def, Unit unit);
ParamDef MakeChoice(clap_id id, const char* name, const char* module,
                    std::vector<std::string> choices, int def);
EffectDef& AddEffect(std::vector<EffectDef>& all, const char* key, const char* name,
                     const char* desc, std::vector<const char*> features,
                     const char* subtype, std::unique_ptr<Effect> (*make)());

// Index of `id` in `def.params`, or -1.
int ParamIndex(const EffectDef& def, clap_id id);

std::string ValueText(const ParamDef& p, double v);
bool TextValue(const ParamDef& p, const char* text, double* out);

// Rounds the way FastRoute's integer settings round, so what a value reads
// as and what the effect is given always agree.
int ToInt(double v);

}  // namespace fp
