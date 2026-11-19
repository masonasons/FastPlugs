#pragma once
#include <clap/clap.h>

#include "effects.h"

namespace fp
{
// One CLAP plugin around one of FastRoute's effects. The caller owns nothing:
// the host destroys it through clap_plugin::destroy.
const clap_plugin_t* CreatePlugin(const clap_host_t* host, const EffectDef& def);
}

// The entry, as a static library sees it, so the exported clap_entry and the
// smoke test can both reach it.
bool fastplugs_entry_init(const char* plugin_path);
void fastplugs_entry_deinit(void);
const void* fastplugs_entry_get_factory(const char* factory_id);
