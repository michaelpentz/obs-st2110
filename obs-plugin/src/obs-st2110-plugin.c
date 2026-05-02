/* OBS module entry points for obs-st2110. */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-st2110", "en-US")

extern struct obs_source_info obs_st2110_source_info;

const char *obs_module_name(void)
{
    return "obs-st2110";
}

const char *obs_module_description(void)
{
    return "SMPTE ST 2110-20 receiver source for OBS Studio";
}

bool obs_module_load(void)
{
    obs_register_source(&obs_st2110_source_info);
    return true;
}

void obs_module_unload(void)
{
}
