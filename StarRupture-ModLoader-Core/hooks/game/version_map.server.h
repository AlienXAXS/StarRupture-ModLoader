// Version object map rows for the dedicated server exe. Compiled into the
// Server build only (memory_scanner/version_map.cpp); the Client build never
// reads it.
//
// Kept by hand: the generated header (version_map.generated.h) is the client's
// and is rewritten whole on every game update, so the server's rows live in a
// file of their own. The records have the shapes the generated header declares,
// and one block per build, so a tool can write this file the same way later.
//
// Every row is for one exe, identified by its CL, PE time stamp and image size
// (the loader serves a row only on an exe that matches all three).
//
// Function rows are addresses (no pattern: a plugin's own pattern stays its
// fallback). A function row is the start of the one PDB symbol with that exact
// name; FName::AppendString is the exception, as the PDB has three overloads of
// that name: its row is the FString& one the SDK calls, the start the loader's
// own pattern for it matches. A global row (kind 4, or the loader's scoped
// GObjects key, kind 6) is the address of the variable; the GObjects row is
// GUObjectArray plus the offset of ObjObjects. The loader's own requests for
// GObjects, GNames, FName::AppendString and GWorld read these four rows (the
// Game-SDK globals it hands to plugins); the client's patterns for them match
// this exe once each and land on the same addresses. 15 of the function rows
// were first found by scanning the exe with the plugins' own patterns, and are
// the same addresses as their PDB symbols.
//
// Field rows and size rows (a size row is a class-key row: the key is the class
// name, offset 0) are the layout measured from the server's PDB for the exe. A
// member's size is the size a plugin reads it at, as in the client table:
// FTransform::Translation is a 0x20-byte vector register in the PDB and is read
// as a 0x18-byte FVector, so its row says 0x18. A bool member has bool mask 0xFF.
#pragma once

#include "hooks/game/version_map_built.h"

namespace VersionMap
{

// The server's build here is the one the loader's start check was written for.
static_assert(kBuiltCL == 127004, "the server table is measured on CL-127004; move it with kBuiltCL");

inline constexpr std::array<Build, 1> kServerBuilds = {{
	{ 127004, 0x2E3595FF, 0x0E2A8000, "server steam_build 25642172" },
}};

inline constexpr std::array<FunctionRow, 30> kServerFunctionRows = {{
	// ACrGameModeBase::GetProfessionForNewPlayer
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06E7DBB0, nullptr },
	// ACrGameModeBase::PreLogin
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06E92390, nullptr },
	// ACrGameStateBase::UpdatePause
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06EA2AF0, nullptr },
	// FCrCraftingFragment::StaticStruct
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06B89D90, nullptr },
	// FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05E015D0, nullptr },
	// FJsonObjectConverter::UStructToJsonObjectString
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x02945D00, nullptr },
	// FMassEntityManager::InternalGetFragmentDataPtr
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03C24BC0, nullptr },
	// FMassEntityManager::IsEntityValid
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03C250C0, nullptr },
	// FName::AppendString
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x0142A250, nullptr },
	// FPaths::ProjectSavedDir
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x013F7D90, nullptr },
	// GNames (NamePoolData)
	{ 127004, 0x2E3595FF, 0x0E2A8000, 4, 0x00000000, false, nullptr, 0, -1, -1, 0x0D2BD600, nullptr },
	// GWorld
	{ 127004, 0x2E3595FF, 0x0E2A8000, 4, 0x00000000, false, nullptr, 0, -1, -1, 0x0D50EE38, nullptr },
	// ModLoader/GObjects (GUObjectArray.ObjObjects)
	{ 127004, 0x2E3595FF, 0x0E2A8000, 6, 0xD7B4F6BF, false, nullptr, 0, -1, -1, 0x0D3A1A50, nullptr },
	// UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06CD5E40, nullptr },
	// UCrBuildingCustomNameSubsystem::GetBuildingCustomNameByReplicationHelper
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06D28640, nullptr },
	// UCrBuildingFunctionLibrary::AddTemperatureToEntity
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06D16DE0, nullptr },
	// UCrDedicatedServerSettingsComp::ParseSettings
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06E8F2B0, nullptr },
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
	// UCrSaveSubsystem::SaveNextSaveGame
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06EDB070, nullptr },
	// UMassSignalSubsystem::SignalEntities
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05D7E340, nullptr },
	// UMassSignalSubsystem::SignalEntity
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05D7E6C0, nullptr },
	// UObject::ProcessEvent
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x016F4C00, nullptr },
	// UReplicationGraph::AddNetworkActor
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05AD7530, nullptr },
	// UReplicationGraph::RemoveNetworkActor
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05AF2730, nullptr },
	// UWorld::GetSubsystem<UMassEntitySubsystem>
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03BE9E90, nullptr },
}};

inline constexpr std::array<FieldRow, 110> kServerFieldRows = {{
	// ACrGameStateBase::WaveTimerActor
	{ 127004, 0x318, 0x8, 0x00, nullptr },
	// ACrGatherableSpawnersRepActor::RepEnviroWaveStageChange
	{ 127004, 0x2AD, 0x1, 0x00, nullptr },
	// ACrGatherableSpawnersRepActor::RepEnviroWaveTypeChange
	{ 127004, 0x2AC, 0x1, 0x00, nullptr },
	// ACrGatherableSpawnersRepActor::RepGlobalGatherablePCGSeed
	{ 127004, 0x2A8, 0x4, 0x00, nullptr },
	// ACrPCGActorBase::LocalSeed
	{ 127004, 0x2B8, 0x4, 0x00, nullptr },
	// ACrPCGGatherableSpawnerVolume::LocalSeed
	{ 127004, 0x2E8, 0x4, 0x00, nullptr },
	// ACrPCGVolume::LocalSeed
	{ 127004, 0x2E8, 0x4, 0x00, nullptr },
	// ACrWaveTimerActor::NextPhase
	{ 127004, 0x2AC, 0x4, 0x00, nullptr },
	// ACrWaveTimerActor::NextTime
	{ 127004, 0x2A8, 0x4, 0x00, nullptr },
	// ACrWaveTimerActor::bPause
	{ 127004, 0x2B0, 0x1, 0xFF, nullptr },
	// AGameStateBase::PlayerArray
	{ 127004, 0x2C0, 0x10, 0x00, nullptr },
	// APlayerState::CompressedPing
	{ 127004, 0x2B0, 0x1, 0x00, nullptr },
	// APlayerState::PlayerNamePrivate
	{ 127004, 0x348, 0x10, 0x00, nullptr },
	// AWorldSettings::TimeDilation
	{ 127004, 0x3E0, 0x4, 0x00, nullptr },
	// AuItemRecipeData_GetOutputItem
	{ 127004, 0x0, 0x30, 0x00, nullptr },
	// AuItemRecipeData_GetOutputItem::ReturnValue
	{ 127004, 0x0, 0x30, 0x00, nullptr },
	// FAuAPMassFragment
	{ 127004, 0x0, 0x50, 0x00, nullptr },
	// FAuSimpleItem
	{ 127004, 0x0, 0x30, 0x00, nullptr },
	// FAuSimpleItem::Count
	{ 127004, 0x28, 0x4, 0x00, nullptr },
	// FAuSimpleItem::ItemDataBase
	{ 127004, 0x20, 0x8, 0x00, nullptr },
	// FBaseCoreReplicationSaveData
	{ 127004, 0x0, 0x38, 0x00, nullptr },
	// FBuildingCustomNameSaveData
	{ 127004, 0x0, 0x58, 0x00, nullptr },
	// FCrAntennaSaveData
	{ 127004, 0x0, 0x50, 0x00, nullptr },
	// FCrBaseCoreSaveData
	{ 127004, 0x0, 0x28, 0x00, nullptr },
	// FCrBuildingGraphData::NodeDatas
	{ 127004, 0x50, 0x50, 0x00, nullptr },
	// FCrBuildingStabilitySubsystemState::BuildingFoundationData
	{ 127004, 0x150, 0x50, 0x00, nullptr },
	// FCrBuildingStabilitySubsystemState::GraphData
	{ 127004, 0x0, 0xB0, 0x00, nullptr },
	// FCrCharacterPlayerBaseSaveDataPerPlayer
	{ 127004, 0x0, 0x6F0, 0x00, nullptr },
	// FCrCharacterPlayersBaseSaveData
	{ 127004, 0x0, 0x50, 0x00, nullptr },
	// FCrCraftingFragment::CraftingMultiplier
	{ 127004, 0x70, 0x4, 0x00, nullptr },
	// FCrCraftingFragment::CurrentRecipe
	{ 127004, 0x8, 0x8, 0x00, nullptr },
	// FCrCraftingFragment::SelectedRecipe
	{ 127004, 0x0, 0x8, 0x00, nullptr },
	// FCrEntitySaveData
	{ 127004, 0x0, 0x90, 0x00, nullptr },
	// FCrEntitySaveData::FragmentValues
	{ 127004, 0x80, 0x10, 0x00, nullptr },
	// FCrEntitySaveData::Tags
	{ 127004, 0x70, 0x10, 0x00, nullptr },
	// FCrEnviroWaveSettings
	{ 127004, 0x0, 0x2C, 0x00, nullptr },
	// FCrEnviroWaveSettings::PreWaveDuration
	{ 127004, 0xC, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveEndPosition
	{ 127004, 0x8, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveFadeoutBurningDuration
	{ 127004, 0x14, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveFadeoutFadingDuration
	{ 127004, 0x18, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveFadeoutFireWaveDuration
	{ 127004, 0x10, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveGrowbackMoonPhaseDuration
	{ 127004, 0x1C, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveGrowbackRegrowthDuration
	{ 127004, 0x24, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveGrowbackRegrowthStartDuration
	{ 127004, 0x20, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WavePreWaveExplosionDuration
	{ 127004, 0x28, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveSpeed
	{ 127004, 0x0, 0x4, 0x00, nullptr },
	// FCrEnviroWaveSettings::WaveStartPosition
	{ 127004, 0x4, 0x4, 0x00, nullptr },
	// FCrGameVersion
	{ 127004, 0x0, 0xC, 0x00, nullptr },
	// FCrGridEntityHandle
	{ 127004, 0x0, 0x18, 0x00, nullptr },
	// FCrGridEntityHandle::Data
	{ 127004, 0x8, 0x10, 0x00, nullptr },
	// FCrGridEntityHandle::EntityHandle
	{ 127004, 0x0, 0x8, 0x00, nullptr },
	// FCrMassActorReplicationHelper
	{ 127004, 0x0, 0x18, 0x00, nullptr },
	// FCrMassActorReplicationHelper::Actor
	{ 127004, 0xC, 0x8, 0x00, nullptr },
	// FCrMassEntityReplicationHelper
	{ 127004, 0x0, 0x18, 0x00, nullptr },
	// FCrMassSaveData::StabilitySubsystemState
	{ 127004, 0x50, 0x1A0, 0x00, nullptr },
	// FCrMassTemperatureFragment::CurrentHeat
	{ 127004, 0x0, 0x4, 0x00, nullptr },
	// FCrMassTemperatureFragment::LastUpdateTime
	{ 127004, 0x78, 0x4, 0x00, nullptr },
	// FCrPlayersMapMenuState
	{ 127004, 0x0, 0x50, 0x00, nullptr },
	// FCrSaveGameData
	{ 127004, 0x0, 0xE0, 0x00, nullptr },
	// FCrSaveGameData::GameVersion
	{ 127004, 0xB0, 0xC, 0x00, nullptr },
	// FCrSaveGameData::Level
	{ 127004, 0x50, 0x10, 0x00, nullptr },
	// FCrSaveGameData::Timestamp
	{ 127004, 0xA0, 0x10, 0x00, nullptr },
	// FCrSaveGameData::WorldAudioTimeSeconds
	{ 127004, 0x78, 0x8, 0x00, nullptr },
	// FCrSaveGameData::WorldRealTimeSeconds
	{ 127004, 0x70, 0x8, 0x00, nullptr },
	// FCrSaveGameData::WorldTimeSeconds
	{ 127004, 0x60, 0x8, 0x00, nullptr },
	// FCrSaveGameData::WorldUnpausedTimeSeconds
	{ 127004, 0x68, 0x8, 0x00, nullptr },
	// FCrZiplineReplicatorSaveData
	{ 127004, 0x0, 0x50, 0x00, nullptr },
	// FCrZiplineSaveData
	{ 127004, 0x0, 0x50, 0x00, nullptr },
	// FGameStateSaveData
	{ 127004, 0x0, 0x490, 0x00, nullptr },
	// FHttpServerRequest::Verb
	{ 127004, 0x20, 0x2, 0x00, nullptr },
	// FMassActorFragment::Actor
	{ 127004, 0x0, 0x8, 0x00, nullptr },
	// FTransform::Translation
	{ 127004, 0x20, 0x18, 0x00, nullptr },
	// FTransformFragment::Transform
	{ 127004, 0x0, 0x60, 0x00, nullptr },
	// FUObjectItem::SerialNumber
	{ 127004, 0x10, 0x4, 0x00, nullptr },
	// UAuItemDataBase::ItemName
	{ 127004, 0x2E0, 0x10, 0x00, nullptr },
	// UAuItemDataBase::UniqueItemName
	{ 127004, 0x2F0, 0x8, 0x00, nullptr },
	// UAuItemDataBaseSubsystem::ItemDataBaseArray
	{ 127004, 0x30, 0x10, 0x00, nullptr },
	// UCrDedicatedServerSettingsComp::SaveGameInterval
	{ 127004, 0xD8, 0x4, 0x00, nullptr },
	// UCrDedicatedServerSettingsComp::SaveGameName
	{ 127004, 0xC8, 0x10, 0x00, nullptr },
	// UCrDedicatedServerSettingsComp::SessionName
	{ 127004, 0xB8, 0x10, 0x00, nullptr },
	// UCrDedicatedServerSettingsComp::bLoadSavedGame
	{ 127004, 0xDD, 0x1, 0xFF, nullptr },
	// UCrDedicatedServerSettingsComp::bStartNewGame
	{ 127004, 0xDC, 0x1, 0xFF, nullptr },
	// UCrEnviroWaveSubsystem::CurrentFadeoutSubstage
	{ 127004, 0x160, 0x1, 0x00, nullptr },
	// UCrEnviroWaveSubsystem::CurrentGrowbackSubstage
	{ 127004, 0x180, 0x1, 0x00, nullptr },
	// UCrEnviroWaveSubsystem::CurrentPreWaveSubstage
	{ 127004, 0x1A0, 0x1, 0x00, nullptr },
	// UCrEnviroWaveTimerSubsystem::NextWaveTimer
	{ 127004, 0x7C, 0x4, 0x00, nullptr },
	// UCrEnviroWaveTimerSubsystem::TimerActor
	{ 127004, 0x90, 0x8, 0x00, nullptr },
	// UCrEnviroWaveTimerSubsystem::bStopWaves
	{ 127004, 0x83, 0x1, 0xFF, nullptr },
	// UCrEnviroWaveTimerSubsystem::bWaitingForNextWave
	{ 127004, 0x80, 0x1, 0xFF, nullptr },
	// UCrGameInstance::ServerSessionName
	{ 127004, 0x290, 0x10, 0x00, nullptr },
	// UCrMassPersistentIDSubsystem::HandleIDMap
	{ 127004, 0x80, 0x50, 0x00, nullptr },
	// UCrMassPersistentIDSubsystem::IDHandleMap
	{ 127004, 0x30, 0x50, 0x00, nullptr },
	// UCrMassPersistentIDSubsystem::MaxID
	{ 127004, 0xD0, 0x4, 0x00, nullptr },
	// UCrReplicationGraph::ClassRepNodePolicies
	{ 127004, 0x5E0, 0x80, 0x00, nullptr },   // the whole TClassMap: the TMap inside it starts 0x30 on
	// UCrSaveSubsystem::OnAfterSave
	{ 127004, 0x1D8, 0x18, 0x00, nullptr },
	// UCrSaveSubsystem::OnPreSaveStart
	{ 127004, 0x160, 0x18, 0x00, nullptr },
	// UCrSaveSubsystem::SaveData
	{ 127004, 0x30, 0xE0, 0x00, nullptr },
	// UMassActorSubsystem::ActorManager
	{ 127004, 0x38, 0x10, 0x00, nullptr },
	// UMassEntitySubsystem::EntityManager
	{ 127004, 0x38, 0x10, 0x00, nullptr },
	// UMassSignalSubsystem::CachedWorld
	{ 127004, 0xB0, 0x8, 0x00, nullptr },
	// UNetDriver::ClientConnections
	{ 127004, 0x100, 0x10, 0x00, nullptr },
	// UObject::ClassPrivate
	{ 127004, 0x10, 0x8, 0x00, nullptr },
	// UObject::InternalIndex
	{ 127004, 0xC, 0x4, 0x00, nullptr },
	// UObject::OuterPrivate
	{ 127004, 0x20, 0x8, 0x00, nullptr },
	// UWorld::AudioTimeSeconds
	{ 127004, 0x850, 0x8, 0x00, nullptr },
	// UWorld::GameState
	{ 127004, 0x1B0, 0x8, 0x00, nullptr },
	// UWorld::NetDriver
	{ 127004, 0x38, 0x8, 0x00, nullptr },
	// UWorld::RealTimeSeconds
	{ 127004, 0x848, 0x8, 0x00, nullptr },
	// UWorld::TimeSeconds
	{ 127004, 0x838, 0x8, 0x00, nullptr },
	// UWorld::UnpausedTimeSeconds
	{ 127004, 0x840, 0x8, 0x00, nullptr },
}};

inline constexpr std::array<Index, 30> kServerFunctions = {{
	{ "ACrGameModeBase::GetProfessionForNewPlayer", 0, 1 },
	{ "ACrGameModeBase::PreLogin", 1, 1 },
	{ "ACrGameStateBase::UpdatePause", 2, 1 },
	{ "FCrCraftingFragment::StaticStruct", 3, 1 },
	{ "FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper", 4, 1 },
	{ "FJsonObjectConverter::UStructToJsonObjectString", 5, 1 },
	{ "FMassEntityManager::InternalGetFragmentDataPtr", 6, 1 },
	{ "FMassEntityManager::IsEntityValid", 7, 1 },
	{ "FName::AppendString", 8, 1 },
	{ "FPaths::ProjectSavedDir", 9, 1 },
	{ "GNames (NamePoolData)", 10, 1 },
	{ "GWorld", 11, 1 },
	{ "ModLoader/GObjects (GUObjectArray.ObjObjects)", 12, 1 },
	{ "UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding", 13, 1 },
	{ "UCrBuildingCustomNameSubsystem::GetBuildingCustomNameByReplicationHelper", 14, 1 },
	{ "UCrBuildingFunctionLibrary::AddTemperatureToEntity", 15, 1 },
	{ "UCrDedicatedServerSettingsComp::ParseSettings", 16, 1 },
	{ "UCrMassEnviroWaveSubsystem::AddEntitiesToWave", 17, 1 },
	{ "UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave", 18, 1 },
	{ "UCrMassPersistentIDSubsystem::GetOrAddIDForHandle", 19, 1 },
	{ "UCrMassPersistentIDSubsystem::SetIDHandlePair", 20, 1 },
	{ "UCrSaveSubsystem::GetSaveData", 21, 1 },
	{ "UCrSaveSubsystem::OnPreLoadMap", 22, 1 },
	{ "UCrSaveSubsystem::SaveNextSaveGame", 23, 1 },
	{ "UMassSignalSubsystem::SignalEntities", 24, 1 },
	{ "UMassSignalSubsystem::SignalEntity", 25, 1 },
	{ "UObject::ProcessEvent", 26, 1 },
	{ "UReplicationGraph::AddNetworkActor", 27, 1 },
	{ "UReplicationGraph::RemoveNetworkActor", 28, 1 },
	{ "UWorld::GetSubsystem<UMassEntitySubsystem>", 29, 1 },
}};

inline constexpr std::array<Index, 110> kServerFields = {{
	{ "ACrGameStateBase::WaveTimerActor", 0, 1 },
	{ "ACrGatherableSpawnersRepActor::RepEnviroWaveStageChange", 1, 1 },
	{ "ACrGatherableSpawnersRepActor::RepEnviroWaveTypeChange", 2, 1 },
	{ "ACrGatherableSpawnersRepActor::RepGlobalGatherablePCGSeed", 3, 1 },
	{ "ACrPCGActorBase::LocalSeed", 4, 1 },
	{ "ACrPCGGatherableSpawnerVolume::LocalSeed", 5, 1 },
	{ "ACrPCGVolume::LocalSeed", 6, 1 },
	{ "ACrWaveTimerActor::NextPhase", 7, 1 },
	{ "ACrWaveTimerActor::NextTime", 8, 1 },
	{ "ACrWaveTimerActor::bPause", 9, 1 },
	{ "AGameStateBase::PlayerArray", 10, 1 },
	{ "APlayerState::CompressedPing", 11, 1 },
	{ "APlayerState::PlayerNamePrivate", 12, 1 },
	{ "AWorldSettings::TimeDilation", 13, 1 },
	{ "AuItemRecipeData_GetOutputItem", 14, 1 },
	{ "AuItemRecipeData_GetOutputItem::ReturnValue", 15, 1 },
	{ "FAuAPMassFragment", 16, 1 },
	{ "FAuSimpleItem", 17, 1 },
	{ "FAuSimpleItem::Count", 18, 1 },
	{ "FAuSimpleItem::ItemDataBase", 19, 1 },
	{ "FBaseCoreReplicationSaveData", 20, 1 },
	{ "FBuildingCustomNameSaveData", 21, 1 },
	{ "FCrAntennaSaveData", 22, 1 },
	{ "FCrBaseCoreSaveData", 23, 1 },
	{ "FCrBuildingGraphData::NodeDatas", 24, 1 },
	{ "FCrBuildingStabilitySubsystemState::BuildingFoundationData", 25, 1 },
	{ "FCrBuildingStabilitySubsystemState::GraphData", 26, 1 },
	{ "FCrCharacterPlayerBaseSaveDataPerPlayer", 27, 1 },
	{ "FCrCharacterPlayersBaseSaveData", 28, 1 },
	{ "FCrCraftingFragment::CraftingMultiplier", 29, 1 },
	{ "FCrCraftingFragment::CurrentRecipe", 30, 1 },
	{ "FCrCraftingFragment::SelectedRecipe", 31, 1 },
	{ "FCrEntitySaveData", 32, 1 },
	{ "FCrEntitySaveData::FragmentValues", 33, 1 },
	{ "FCrEntitySaveData::Tags", 34, 1 },
	{ "FCrEnviroWaveSettings", 35, 1 },
	{ "FCrEnviroWaveSettings::PreWaveDuration", 36, 1 },
	{ "FCrEnviroWaveSettings::WaveEndPosition", 37, 1 },
	{ "FCrEnviroWaveSettings::WaveFadeoutBurningDuration", 38, 1 },
	{ "FCrEnviroWaveSettings::WaveFadeoutFadingDuration", 39, 1 },
	{ "FCrEnviroWaveSettings::WaveFadeoutFireWaveDuration", 40, 1 },
	{ "FCrEnviroWaveSettings::WaveGrowbackMoonPhaseDuration", 41, 1 },
	{ "FCrEnviroWaveSettings::WaveGrowbackRegrowthDuration", 42, 1 },
	{ "FCrEnviroWaveSettings::WaveGrowbackRegrowthStartDuration", 43, 1 },
	{ "FCrEnviroWaveSettings::WavePreWaveExplosionDuration", 44, 1 },
	{ "FCrEnviroWaveSettings::WaveSpeed", 45, 1 },
	{ "FCrEnviroWaveSettings::WaveStartPosition", 46, 1 },
	{ "FCrGameVersion", 47, 1 },
	{ "FCrGridEntityHandle", 48, 1 },
	{ "FCrGridEntityHandle::Data", 49, 1 },
	{ "FCrGridEntityHandle::EntityHandle", 50, 1 },
	{ "FCrMassActorReplicationHelper", 51, 1 },
	{ "FCrMassActorReplicationHelper::Actor", 52, 1 },
	{ "FCrMassEntityReplicationHelper", 53, 1 },
	{ "FCrMassSaveData::StabilitySubsystemState", 54, 1 },
	{ "FCrMassTemperatureFragment::CurrentHeat", 55, 1 },
	{ "FCrMassTemperatureFragment::LastUpdateTime", 56, 1 },
	{ "FCrPlayersMapMenuState", 57, 1 },
	{ "FCrSaveGameData", 58, 1 },
	{ "FCrSaveGameData::GameVersion", 59, 1 },
	{ "FCrSaveGameData::Level", 60, 1 },
	{ "FCrSaveGameData::Timestamp", 61, 1 },
	{ "FCrSaveGameData::WorldAudioTimeSeconds", 62, 1 },
	{ "FCrSaveGameData::WorldRealTimeSeconds", 63, 1 },
	{ "FCrSaveGameData::WorldTimeSeconds", 64, 1 },
	{ "FCrSaveGameData::WorldUnpausedTimeSeconds", 65, 1 },
	{ "FCrZiplineReplicatorSaveData", 66, 1 },
	{ "FCrZiplineSaveData", 67, 1 },
	{ "FGameStateSaveData", 68, 1 },
	{ "FHttpServerRequest::Verb", 69, 1 },
	{ "FMassActorFragment::Actor", 70, 1 },
	{ "FTransform::Translation", 71, 1 },
	{ "FTransformFragment::Transform", 72, 1 },
	{ "FUObjectItem::SerialNumber", 73, 1 },
	{ "UAuItemDataBase::ItemName", 74, 1 },
	{ "UAuItemDataBase::UniqueItemName", 75, 1 },
	{ "UAuItemDataBaseSubsystem::ItemDataBaseArray", 76, 1 },
	{ "UCrDedicatedServerSettingsComp::SaveGameInterval", 77, 1 },
	{ "UCrDedicatedServerSettingsComp::SaveGameName", 78, 1 },
	{ "UCrDedicatedServerSettingsComp::SessionName", 79, 1 },
	{ "UCrDedicatedServerSettingsComp::bLoadSavedGame", 80, 1 },
	{ "UCrDedicatedServerSettingsComp::bStartNewGame", 81, 1 },
	{ "UCrEnviroWaveSubsystem::CurrentFadeoutSubstage", 82, 1 },
	{ "UCrEnviroWaveSubsystem::CurrentGrowbackSubstage", 83, 1 },
	{ "UCrEnviroWaveSubsystem::CurrentPreWaveSubstage", 84, 1 },
	{ "UCrEnviroWaveTimerSubsystem::NextWaveTimer", 85, 1 },
	{ "UCrEnviroWaveTimerSubsystem::TimerActor", 86, 1 },
	{ "UCrEnviroWaveTimerSubsystem::bStopWaves", 87, 1 },
	{ "UCrEnviroWaveTimerSubsystem::bWaitingForNextWave", 88, 1 },
	{ "UCrGameInstance::ServerSessionName", 89, 1 },
	{ "UCrMassPersistentIDSubsystem::HandleIDMap", 90, 1 },
	{ "UCrMassPersistentIDSubsystem::IDHandleMap", 91, 1 },
	{ "UCrMassPersistentIDSubsystem::MaxID", 92, 1 },
	{ "UCrReplicationGraph::ClassRepNodePolicies", 93, 1 },
	{ "UCrSaveSubsystem::OnAfterSave", 94, 1 },
	{ "UCrSaveSubsystem::OnPreSaveStart", 95, 1 },
	{ "UCrSaveSubsystem::SaveData", 96, 1 },
	{ "UMassActorSubsystem::ActorManager", 97, 1 },
	{ "UMassEntitySubsystem::EntityManager", 98, 1 },
	{ "UMassSignalSubsystem::CachedWorld", 99, 1 },
	{ "UNetDriver::ClientConnections", 100, 1 },
	{ "UObject::ClassPrivate", 101, 1 },
	{ "UObject::InternalIndex", 102, 1 },
	{ "UObject::OuterPrivate", 103, 1 },
	{ "UWorld::AudioTimeSeconds", 104, 1 },
	{ "UWorld::GameState", 105, 1 },
	{ "UWorld::NetDriver", 106, 1 },
	{ "UWorld::RealTimeSeconds", 107, 1 },
	{ "UWorld::TimeSeconds", 108, 1 },
	{ "UWorld::UnpausedTimeSeconds", 109, 1 },
}};

inline constexpr std::array<AbsentField, 0> kServerAbsentFields = {};

}
