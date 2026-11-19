// The one exported symbol. Compiled into each format's binary separately, so
// the CLAP, the VST3 and the AU each carry their own copy of the entry.
#include <clap/clap.h>

bool fastplugs_entry_init(const char* plugin_path);
void fastplugs_entry_deinit(void);
const void* fastplugs_entry_get_factory(const char* factory_id);

extern "C"
{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    fastplugs_entry_init,
    fastplugs_entry_deinit,
    fastplugs_entry_get_factory,
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
