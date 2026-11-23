#pragma once
// sr-aobscan --carry: the function keys that have no row for the exe's build,
// resolved the way the loader does at run time.
//
// With no row for the running build the loader decides a key with the row of
// the latest other build the map has a pattern for, rescanned on the running exe
// (VersionMap::Select over the loader's own lookup, mirrored in observed.cpp;
// the loader's own registry rows through ResolveLoaderEntry). The plugin's own
// pattern and rows are not known here, and at run time they join the
// agreement step, so a plugin's hook can still fail where the carry matches.
// A row for the new build made from the answer lets the loader read it
// directly instead. This asks the same question of every key the map holds, so
// a gate can tell the keys that carry from the ones that need a new pattern.

#include "memory_scanner/version_map.h"

#include <cstdint>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace AobScan
{
	enum class CarryResult { Match, None, Ambiguous };

	struct CarryRow
	{
		std::string key;
		int32_t     kind = 0;          // as the map's rows keep it
		uint32_t    fp   = 0;          // 0 for a global key
		uint32_t    fromCL  = 0;       // whose row's pattern was tried; 0 when the map has none to try
		size_t      matches = 0;       // what that pattern found in the exe
		bool        hasRva  = false;
		uint32_t    rva     = 0;       // only for a Match
		CarryResult result  = CarryResult::None;
		std::string reason;            // for None and Ambiguous
		// The locator a row for this build is made from: the tried row's.
		std::string pattern;
		int32_t     resultOffset    = 0;
		int32_t     followRel32At   = -1;
		int32_t     followRipDispAt = -1;
		// A global key's absent rows on a build newer than the tried row bind
		// the owners they list, and the loader then uses no row for those.
		std::string owners;
	};

	struct Carry
	{
		uint32_t cl = 0;
		size_t   keys = 0;             // distinct keys (with kind and fp) in the map
		size_t   withRow = 0;          // skipped: the map has a row for cl
		size_t   absentRow = 0;        // of those, a row that says the key is absent on cl
		std::vector<CarryRow> rows;
	};

	// Every key of VersionMap::ActiveMap() with no row for exe.cl, resolved on
	// the image mapped at `module`. A site key's owner is the part of the key
	// before '/'; a global key's owner is unknown, so no absent row binds it.
	Carry ScanCarry(HMODULE module, const VersionMap::Exe& exe);

	std::string CarryJson(const Carry& carry);
}
