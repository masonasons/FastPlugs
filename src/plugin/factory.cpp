// ---------------------------------------------------------------------------
// The factory: every effect in one bundle.
//
// Alongside the CLAP factory are the two the wrapper reads when it builds the
// other formats. The AUv2 one gives each effect its four letter subtype -
// without it the wrapper would hash the CLAP id into one, which works but
// reads as nonsense in auval - and it is read at build time, when the wrapper
// writes the component's Info.plist. The VST3 one names the vendor; the class
// ids are derived from the CLAP ids, so they are as stable as those are.
// ---------------------------------------------------------------------------
#include "plugin.h"

#include <clapwrapper/auv2.h>
#include <clapwrapper/vst3.h>

#include <cstring>

namespace
{

uint32_t Count(const clap_plugin_factory_t*)
{
    return (uint32_t)fp::Effects().size();
}

const clap_plugin_descriptor_t* Descriptor(const clap_plugin_factory_t*, uint32_t index)
{
    const auto& all = fp::Effects();
    return index < all.size() ? &all[index].desc : nullptr;
}

const clap_plugin_t* Create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (!host || !id || !clap_version_is_compatible(host->clap_version)) return nullptr;
    for (const fp::EffectDef& e : fp::Effects())
        if (e.clapId == id) return fp::CreatePlugin(host, e);
    return nullptr;
}

const clap_plugin_factory_t s_factory = { Count, Descriptor, Create };

bool Auv2Info(const clap_plugin_factory_as_auv2*, uint32_t index, clap_plugin_info_as_auv2_t* info)
{
    const auto& all = fp::Effects();
    if (index >= all.size()) return false;
    std::memcpy(info->au_type, "aufx", 5);
    std::memcpy(info->au_subt, all[index].auSubtype, 4);
    info->au_subt[4] = 0;
    return true;
}

const clap_plugin_factory_as_auv2_t s_auv2 = {
    FASTPLUGS_AU_MANUFACTURER,
    FASTPLUGS_VENDOR,
    Auv2Info,
};

const clap_plugin_factory_as_vst3_t s_vst3 = {
    FASTPLUGS_VENDOR,
    FASTPLUGS_URL,
    nullptr,    // no contact address
    nullptr,    // class ids derived from the CLAP ids
    nullptr,    // nothing older to stand in for
};

}  // namespace

bool fastplugs_entry_init(const char*)
{
    fp::Effects();   // built once, here, rather than on a host's first scan
    return true;
}

void fastplugs_entry_deinit(void) {}

const void* fastplugs_entry_get_factory(const char* id)
{
    if (!id) return nullptr;
    if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
    if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_AUV2)) return &s_auv2;
    if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_VST3)) return &s_vst3;
    return nullptr;
}
