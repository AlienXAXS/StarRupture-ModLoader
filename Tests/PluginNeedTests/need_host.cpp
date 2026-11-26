// The loader's own build: PLUGIN_INTERFACE_HOST_BUILD makes it see every member.
#define PLUGIN_INTERFACE_HOST_BUILD
#include "need_checks.h"

static_assert(PLUGIN_NEEDS_INTERFACE == PLUGIN_INTERFACE_VERSION_MAX, "the host build sees every member");

// The accessors' offsets are the real member offsets.
static_assert(PLUGIN_OPT_OFFSET_IMAGES == offsetof(IPluginHooks, Images), "Images offset");
static_assert(PLUGIN_OPT_OFFSET_ADD_TEXTURED_TRIANGLES == offsetof(IModLoaderImGui, DL_AddTexturedTriangles), "DL_AddTexturedTriangles offset");
static_assert(PLUGIN_OPT_OFFSET_HOST_RULES == offsetof(IPluginHooks, HostRules), "HostRules offset");
static_assert(PLUGIN_OPT_OFFSET_REGISTRY == offsetof(IPluginHooks, Registry), "Registry offset");
static_assert(PLUGIN_OPT_OFFSET_SERVER_ADDRESS == offsetof(IPluginNetworkChannel, GetServerAddress), "GetServerAddress offset");
static_assert(PLUGIN_OPT_OFFSET_HOST_RULES_STATE == offsetof(IPluginNetworkChannel, GetHostRulesState), "GetHostRulesState offset");
static_assert(PLUGIN_OPTIONAL_INTERFACE == PLUGIN_INTERFACE_VERSION_MAX, "the host build has every accessor");
#include "opt_checks.h"
