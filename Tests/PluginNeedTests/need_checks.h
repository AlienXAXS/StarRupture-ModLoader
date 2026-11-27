// Shared checks for PluginNeedTests. A source file sets the level (or nothing, or
// PLUGIN_INTERFACE_HOST_BUILD) and includes this header; it checks the host tables
// of plugins\plugin_interface.h as that level sees them:
//   - a member added at version S is visible exactly when the level is S or more,
//   - a visible member sits at its frozen offset (members only ever append),
//   - sizeof of each host table ends where its last visible member ends,
//   - structs the plugin allocates are never cut down.
#pragma once

#include "plugins/plugin_interface.h"

#include <cstddef>

#define NEED PLUGIN_NEEDS_INTERFACE

// One row per appended member: struct, member, interface version it was added in, byte offset.
#define NEED_MEMBER_CHECK(S, M, SINCE, OFFSET)                                              \
	namespace check_##S##_##M                                                               \
	{                                                                                       \
		template<class T> concept Visible = requires { &T::M; };                           \
		static_assert(Visible<S> == (NEED >= SINCE), #S "::" #M ": wrong visibility");      \
		template<class T> constexpr size_t OffsetOrExpected()                               \
		{                                                                                   \
			if constexpr (Visible<T>) return offsetof(T, M);                                \
			else return OFFSET;                                                             \
		}                                                                                   \
		static_assert(OffsetOrExpected<S>() == OFFSET, #S "::" #M ": wrong offset");        \
	}

// v71 and v72 members stay unconditional: the allowed range starts at 72.
NEED_MEMBER_CHECK(IPluginGameMenu,      IsPauseMenuOpen,                71, 32)
NEED_MEMBER_CHECK(IPluginUIEvents,      IsOverlayOpen,                  71, 120)
NEED_MEMBER_CHECK(IModLoaderImGui,      IsKeyPressed,                   71, 2024)
NEED_MEMBER_CHECK(IPluginHookScanner,   ResolveField,                   72, 120)
NEED_MEMBER_CHECK(IPluginEngineEvents,  GetSdkGlobals,                  72, 136)
NEED_MEMBER_CHECK(IPluginEngineEvents,  IsGameThread,                   72, 144)
NEED_MEMBER_CHECK(IPluginInputEvents,   IsKeyHeld,                      72, 64)
// Appended after 72.
NEED_MEMBER_CHECK(IPluginHookScanner,   DeclareFeature,                 73, 128)
NEED_MEMBER_CHECK(IPluginEngineEvents,  SetBusyReason,                  73, 152)
NEED_MEMBER_CHECK(IModLoaderImGui,      SetNextWindowSize,              74, 2032)
NEED_MEMBER_CHECK(IModLoaderImGui,      SetNextWindowSizeConstraints,   74, 2040)
NEED_MEMBER_CHECK(IPluginGameMenu,      GetUIState,                     74, 40)
NEED_MEMBER_CHECK(IPluginGameMenu,      RegisterOnUIStateChanged,       74, 48)
NEED_MEMBER_CHECK(IPluginGameMenu,      UnregisterOnUIStateChanged,     74, 56)
NEED_MEMBER_CHECK(IPluginUIEvents,      IsPanelCollapsed,               75, 128)
NEED_MEMBER_CHECK(IPluginUIEvents,      SetPanelCollapsed,              75, 136)
NEED_MEMBER_CHECK(IPluginHooks,         Images,                         77, 192)
NEED_MEMBER_CHECK(IModLoaderImGui,      DL_AddTexturedTriangles,        78, 2048)
NEED_MEMBER_CHECK(IPluginHooks,         HostRules,                      79, 200)
NEED_MEMBER_CHECK(IPluginHooks,         Registry,                       80, 208)
NEED_MEMBER_CHECK(IPluginNetworkChannel, GetServerAddress,              80, 128)
NEED_MEMBER_CHECK(IPluginNetworkChannel, GetHostRulesState,             81, 136)
NEED_MEMBER_CHECK(IPluginNetworkChannel, IsDeniedByHost,                82, 144)
NEED_MEMBER_CHECK(IPluginNetworkChannel, SendPacketToServerPlugin,      83, 152)
NEED_MEMBER_CHECK(IPluginNetworkChannel, SendPacketToClientPlugin,      83, 160)

// The last members that were already there before 72, to show the prefix never moves.
static_assert(offsetof(IPluginHooks, Pak) == 184, "IPluginHooks::Pak moved");
static_assert(offsetof(IPluginNetworkChannel, UnregisterServerReadyCallback) == 120, "IPluginNetworkChannel prefix moved");

// sizeof of a host table is the end of its last visible member.
static_assert(sizeof(IPluginHookScanner)    == (NEED >= 73 ? 136 : 128),                                          "IPluginHookScanner size");
static_assert(sizeof(IPluginEngineEvents)   == (NEED >= 73 ? 160 : 152),                                          "IPluginEngineEvents size");
static_assert(sizeof(IPluginInputEvents)    == 72,                                                                "IPluginInputEvents size");
static_assert(sizeof(IModLoaderImGui)       == (NEED >= 78 ? 2056 : NEED >= 74 ? 2048 : 2032),                     "IModLoaderImGui size");
static_assert(sizeof(IPluginGameMenu)       == (NEED >= 74 ? 64 : 40),                                            "IPluginGameMenu size");
static_assert(sizeof(IPluginUIEvents)       == (NEED >= 75 ? 144 : 128),                                          "IPluginUIEvents size");
static_assert(sizeof(IPluginNetworkChannel) == (NEED >= 83 ? 168 : NEED >= 82 ? 152 : NEED >= 81 ? 144 : NEED >= 80 ? 136 : 128),     "IPluginNetworkChannel size");
static_assert(sizeof(IPluginHooks)          == (NEED >= 80 ? 216 : NEED >= 79 ? 208 : NEED >= 77 ? 200 : 192),     "IPluginHooks size");

// A PluginMapFlags bit, so it is there at every level, 72 included, and changes
// no table above. It shares mapFlags with the map flag and the reload settings
// (bits 8-11).
static_assert(PLUGIN_HANDLES_HOST_DENY == 1u << 12, "PLUGIN_HANDLES_HOST_DENY moved");
static_assert((PLUGIN_HANDLES_HOST_DENY & (PLUGIN_MAP_MIGRATED | 0xF00u)) == 0, "PLUGIN_HANDLES_HOST_DENY overlaps another flag");

// Structs the plugin allocates and the loader reads. PluginInfo is whole at every level;
// PluginPanelDesc has titleBarFn from level 75, which is all the loader reads it for.
static_assert(sizeof(PluginInfo) == 48, "PluginInfo is not truncated");
#if PLUGIN_NEEDS_INTERFACE >= 75
static_assert(offsetof(PluginPanelDesc, titleBarFn) == 24, "PluginPanelDesc::titleBarFn is not truncated");
#endif
