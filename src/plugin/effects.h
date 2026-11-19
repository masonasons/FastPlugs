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
    // Writes the parameters into `b`. `v` is indexed like `params`.
    void (*toBlock)(const double* v, Block& b) = nullptr;
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

// Index of `id` in `def.params`, or -1.
int ParamIndex(const EffectDef& def, clap_id id);

std::string ValueText(const ParamDef& p, double v);
bool TextValue(const ParamDef& p, const char* text, double* out);

// Rounds the way FastRoute's integer settings round, so what a value reads
// as and what the effect is given always agree.
int ToInt(double v);

}  // namespace fp
