// Version object map rows for the dedicated server exe. Compiled into the
// Server build only (memory_scanner/version_map.cpp); the Client build never
// reads it.
//
// Kept by hand: the generated header (version_map.generated.h) is the client's
// and is rewritten whole on every game update, so the server's rows live in a
// file of their own. The records have the shapes the generated header declares,
// and one block per build, so a tool can write this file the same way later.
//
// Every row is an address on one exe, identified by its CL, PE time stamp and
// image size (the loader serves a row only on an exe that matches all three).
// A row has no pattern: a plugin's own pattern stays its fallback. Each rva was
// taken from a scan of that exe with the pattern the plugin ships, which matched
// exactly once at a function start.
#pragma once

#include "hooks/game/version_map.generated.h"

namespace VersionMap
{

// The server's build here is the one the loader's start check was written for.
static_assert(kBuiltCL == 127004, "the server table is measured on CL-127004; move it with kBuiltCL");

inline constexpr std::array<Build, 1> kServerBuilds = {{
	{ 127004, 0x2E3595FF, 0x0E2A8000, "server steam_build 25642172" },
}};

inline constexpr std::array<FunctionRow, 14> kServerFunctionRows = {{
	// FCrCraftingFragment::StaticStruct
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06B89D90, nullptr },
	// FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05E015D0, nullptr },
	// FMassEntityManager::IsEntityValid
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03C250C0, nullptr },
	// UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06CD5E40, nullptr },
	// UCrBuildingCustomNameSubsystem::GetBuildingCustomNameByReplicationHelper
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06D28640, nullptr },
	// UCrBuildingFunctionLibrary::AddTemperatureToEntity
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06D16DE0, nullptr },
	// UCrMassEnviroWaveSubsystem::AddEntitiesToWave
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06EFA200, nullptr },
	// UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06F16310, nullptr },
	// UCrMassPersistentIDSubsystem::GetOrAddIDForHandle
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05DF5410, nullptr },
	// UCrMassPersistentIDSubsystem::SetIDHandlePair
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05DF77C0, nullptr },
	// UCrSaveSubsystem::GetSaveData
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06EC3B30, nullptr },
	// UCrSaveSubsystem::OnPreLoadMap
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06ED29E0, nullptr },
	// UMassSignalSubsystem::SignalEntities
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05D7E340, nullptr },
	// UWorld::GetSubsystem<UMassEntitySubsystem>
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03BE9E90, nullptr },
}};

inline constexpr std::array<FieldRow, 0> kServerFieldRows = {};

inline constexpr std::array<Index, 14> kServerFunctions = {{
	{ "FCrCraftingFragment::StaticStruct", 0, 1 },
	{ "FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper", 1, 1 },
	{ "FMassEntityManager::IsEntityValid", 2, 1 },
	{ "UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding", 3, 1 },
	{ "UCrBuildingCustomNameSubsystem::GetBuildingCustomNameByReplicationHelper", 4, 1 },
	{ "UCrBuildingFunctionLibrary::AddTemperatureToEntity", 5, 1 },
	{ "UCrMassEnviroWaveSubsystem::AddEntitiesToWave", 6, 1 },
	{ "UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave", 7, 1 },
	{ "UCrMassPersistentIDSubsystem::GetOrAddIDForHandle", 8, 1 },
	{ "UCrMassPersistentIDSubsystem::SetIDHandlePair", 9, 1 },
	{ "UCrSaveSubsystem::GetSaveData", 10, 1 },
	{ "UCrSaveSubsystem::OnPreLoadMap", 11, 1 },
	{ "UMassSignalSubsystem::SignalEntities", 12, 1 },
	{ "UWorld::GetSubsystem<UMassEntitySubsystem>", 13, 1 },
}};

inline constexpr std::array<Index, 0> kServerFields = {};

inline constexpr std::array<AbsentField, 0> kServerAbsentFields = {};

}
