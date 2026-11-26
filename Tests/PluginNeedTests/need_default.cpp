// A plugin that sets nothing gets PLUGIN_INTERFACE_BASELINE (75). Every member added
// after 75 must be absent from its view, so a call to one is a compile error and not
// a read past an older loader's table.
#include "need_checks.h"

static_assert(PLUGIN_NEEDS_INTERFACE == 75, "the default level is 75");
static_assert(PLUGIN_NEEDS_INTERFACE == PLUGIN_INTERFACE_BASELINE, "the default level is the baseline");
static_assert(PLUGIN_DECLARED_INTERFACE == 75, "a plugin that sets nothing declares 75");

// Inside a template, so a missing member makes the requirement false and not a hard error.
template<class T> concept HasImages = requires(T t) { t.Images; };
template<class T> concept HasHostRules = requires(T t) { t.HostRules; };
template<class T> concept HasRegistry = requires(T t) { t.Registry; };
template<class T> concept HasDrawTriangles = requires(T t) { t.DL_AddTexturedTriangles; };
template<class T> concept HasServerAddress = requires(T t) { t.GetServerAddress; };
template<class T> concept HasHostRulesState = requires(T t) { t.GetHostRulesState; };

static_assert(!HasImages<IPluginHooks>,                 "Images (v77) is hidden at the default level");
static_assert(!HasHostRules<IPluginHooks>,              "HostRules (v79) is hidden at the default level");
static_assert(!HasRegistry<IPluginHooks>,               "Registry (v80) is hidden at the default level");
static_assert(!HasDrawTriangles<IModLoaderImGui>,       "DL_AddTexturedTriangles (v78) is hidden at the default level");
static_assert(!HasServerAddress<IPluginNetworkChannel>, "GetServerAddress (v80) is hidden at the default level");
static_assert(!HasHostRulesState<IPluginNetworkChannel>, "GetHostRulesState (v81) is hidden at the default level");
