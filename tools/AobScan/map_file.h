#pragma once
// sr-aobscan --map: reads a version map to resolve against, in place of the
// one compiled into the loader. See map_file.cpp.

#include "memory_scanner/version_map.h"

#include <string>

namespace AobScan
{
	// A generated header (hooks/game/version_map.generated.h), read as the
	// whole map. False with `error` set when it cannot be read or parsed. A
	// signed feed is read by the loader's own VersionFeed::BuildFromCache.
	bool ReadMapFile(const std::wstring& path, VersionMap::MapData& out, std::string& error);
}
