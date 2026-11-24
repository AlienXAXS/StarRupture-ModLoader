// Shared checks for the optional-member accessors of plugins\plugin_interface.h. A source file sets
// PLUGIN_NEEDS_INTERFACE and PLUGIN_OPTIONAL_INTERFACE (or neither) and includes this header; each
// accessor must exist exactly when PLUGIN_OPTIONAL_INTERFACE is at least the level of its member.
#pragma once

#include "plugins/plugin_interface.h"

#define OPT PLUGIN_OPTIONAL_INTERFACE

// Inside a template, so a missing function makes the requirement false and not a hard error.
template<class T> concept HasOptImages      = requires(const T* t) { PluginOptImages(t); };
template<class T> concept HasOptDrawTris    = requires(const T* t) { PluginOptDL_AddTexturedTriangles(t); };
template<class T> concept HasOptHostRules   = requires(const T* t) { PluginOptHostRules(t); };
template<class T> concept HasOptRegistry    = requires(const T* t) { PluginOptRegistry(t); };
template<class T> concept HasOptServerAddr  = requires(const T* t) { PluginOptServerAddress(t); };

static_assert(HasOptImages<IPluginHooks>             == (OPT >= 77), "PluginOptImages exists from level 77");
static_assert(HasOptDrawTris<IModLoaderImGui>        == (OPT >= 78), "PluginOptDL_AddTexturedTriangles exists from level 78");
static_assert(HasOptHostRules<IPluginHooks>          == (OPT >= 79), "PluginOptHostRules exists from level 79");
static_assert(HasOptRegistry<IPluginHooks>           == (OPT >= 80), "PluginOptRegistry exists from level 80");
static_assert(HasOptServerAddr<IPluginNetworkChannel> == (OPT >= 80), "PluginOptServerAddress exists from level 80");

// The offsets the accessors read, against the table as laid out in the header.
static_assert(PLUGIN_OPT_OFFSET_IMAGES                 == 192,  "Images offset");
static_assert(PLUGIN_OPT_OFFSET_ADD_TEXTURED_TRIANGLES == 2048, "DL_AddTexturedTriangles offset");
static_assert(PLUGIN_OPT_OFFSET_HOST_RULES             == 200,  "HostRules offset");
static_assert(PLUGIN_OPT_OFFSET_REGISTRY               == 208,  "Registry offset");
static_assert(PLUGIN_OPT_OFFSET_SERVER_ADDRESS         == 128,  "GetServerAddress offset");

// The export macro and the level helpers are there at every level.
static_assert(sizeof(PluginHostInfo) == 8, "PluginHostInfo layout");
