#pragma once
// What the loader does with each kind of plugin DLL already released, on a
// running CL-127004 and a running CL-130045: the admission checks
// (VersionMap::OffBuildRefusal, VersionMap::LayoutRefusal) and whether the
// Game-SDK globals are handed out (VersionMap::SdkGlobalsWithheld).
//
// Compiled into two programs. VersionMapTests builds the loader for CL-127004
// (its fixture map), which is how v1.26.1 is built; PluginClassTests builds it
// for CL-130045. Both must give the same answer for every plugin, so the
// expected table below does not depend on which one it runs in.
//
// What the startup layout check finds on the build it runs on is the other
// input: with the table for CL-127004 a 130045 game has one changed class
// (UCrEnviroWaveTimerSubsystem, TimerActor moved); with the table for CL-130045
// neither game has any, TimerActor being resolved per build. Both are set here
// the way core/init_thread.cpp sets them.
#include "hooks/game/version_map_built.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_interface.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PluginClassMatrix
{
	struct PluginClass
	{
		const char* name;
		int         interfaceVersion;
		uint32_t    builtCL;      // PluginInfo::gameBuiltCL, 0 = not declared
		uint32_t    flags;        // PluginInfo::mapFlags
		bool        askedGlobals; // PluginInit took GetSdkGlobals
		bool        on127004;     // expected: loads on a running 127004
		bool        on130045;     // expected: loads on a running 130045
	};

	constexpr uint32_t kM = PLUGIN_MAP_MIGRATED;

	// A: not migrated, built on 127004. B: migrated, built on 127004.
	// C: built on 130045, not migrated and migrated.
	inline const PluginClass kClasses[] = {
		{ "A  v71, no tail (counts as 127004)",         71, 0,      0,  false, true,  false },
		{ "A  v72+, declares 127004",                   72, 127004, 0,  false, true,  false },
		{ "B  v72+, declares 127004, MIGRATED, globals", 72, 127004, kM, true,  true,  true  },
		{ "B  v72+, declares 127004, MIGRATED, no globals", 72, 127004, kM, false, true,  false },
		{ "C  v72+, declares 130045",                   72, 130045, 0,  false, false, true  },
		{ "C  v72+, declares 130045, MIGRATED, globals", 72, 130045, kM, true,  true,  true  },
		{ "C  v72+, declares 130045, MIGRATED, no globals", 72, 130045, kM, false, false, true  },
	};

	inline PluginInfo InfoOf(const PluginClass& c)
	{
		PluginInfo i = { "T", "1", "t", "t", c.interfaceVersion, PLUGIN_TARGET_CLIENT };
		i.gameBuiltCL = c.builtCL;
		i.mapFlags    = c.flags;
		return i;
	}

	// The reason a plugin is turned away on `running`, "" when it loads: the two
	// checks the plugin manager makes (before PluginInit it has not asked for
	// the globals yet, after it has; the answer for the final state is this).
	inline std::string Refusal(uint32_t running, const PluginClass& c)
	{
		const VersionMap::Exe exe{ running, 0, 0 };
		const VersionMap::PluginGameBuild built = VersionMap::ReadPluginGameBuild(InfoOf(c));
		std::string why = VersionMap::OffBuildRefusal(exe, c.interfaceVersion, built, c.askedGlobals);
		if (why.empty())
			why = VersionMap::LayoutRefusal(exe, c.interfaceVersion, built);
		return why;
	}

	// The classes the layout check calls changed on `running` for the loader built
	// for kBuiltCL. The table is the SDK's, so a member that sits elsewhere on
	// the game's build counts unless it is resolved per build.
	inline void SetLayoutChangesFor(uint32_t running)
	{
		if (VersionMap::kBuiltCL == 127004 && running == 130045)
			VersionMap::SetLayoutChanges({ "UCrEnviroWaveTimerSubsystem", "/Script/Chimera.UCrEnviroWaveTimerSubsystem" },
				{ "/Script/Chimera.CrEnviroWaveTimerSubsystem" });
		else
			VersionMap::SetLayoutChanges({});
	}

	template <class CheckFn>
	void Run(CheckFn check)
	{
		for (const uint32_t running : { 127004u, 130045u })
		{
			SetLayoutChangesFor(running);
			for (const PluginClass& c : kClasses)
			{
				const std::string why = Refusal(running, c);
				const bool expected = running == 127004 ? c.on127004 : c.on130045;
				const std::string what = std::string("plugin class ") + c.name + " on " + std::to_string(running)
					+ (expected ? ": loads" : ": refused");
				check(why.empty() == expected, what.c_str());
			}
		}
		VersionMap::SetLayoutChanges({});

		// A class that really changed on the loader's own build still turns away a plugin
		// that declares that build without the flag (its layouts are the loader's list).
		const uint32_t k = VersionMap::kBuiltCL;
		VersionMap::SetLayoutChanges({ "ACrX", "/Script/Chimera.ACrX" });
		const VersionMap::Exe onK{ k, 0, 0 };
		PluginClass own = { "declares the loader's own build", 72, k, 0, false, false, false };
		const VersionMap::PluginGameBuild builtOwn = VersionMap::ReadPluginGameBuild(InfoOf(own));
		check(!VersionMap::LayoutRefusal(onK, 72, builtOwn).empty(),
			"a changed class on the loader's own build refuses a plugin built for it without the flag");
		own.flags = kM;
		check(VersionMap::LayoutRefusal(onK, 72, VersionMap::ReadPluginGameBuild(InfoOf(own))).empty(),
			"and admits it with the flag");
		VersionMap::SetLayoutChanges({});

		// The Game-SDK globals are handed out on the loader's own build from its compiled
		// offsets, and on any other build only when the version map's rows found all
		// three. That is the one thing that depends on which build the loader is made
		// for: a running 127004 is now an off-build for a loader made for 130045.
		const auto withheld = [](uint32_t running, bool gworld)
		{
			return !VersionMap::SdkGlobalsWithheld(VersionMap::Exe{ running, 0, 0 }, 0x1000, 0x2000, gworld ? 0x3000 : 0).empty();
		};
		for (const uint32_t running : { 127004u, 130045u })
		{
			const bool offBuild = running != VersionMap::kBuiltCL;
			check(!withheld(running, true), (std::string("globals handed out on ") + std::to_string(running) + " when all three are found").c_str());
			check(withheld(running, false) == offBuild,
				(std::string("GWorld missing on ") + std::to_string(running) + (offBuild ? ": withheld (off-build)" : ": still handed out (the loader's own build)")).c_str());
		}
		check(VersionMap::SdkGlobalsWithheld(VersionMap::Exe{ 127004, 0, 0 }, 0, 0x2000, 0x3000).find("GObjects") != std::string::npos,
			"GObjects missing: withheld on any build");
	}
}
