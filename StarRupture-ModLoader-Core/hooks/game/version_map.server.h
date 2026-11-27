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
// name (a key may leave out an outer class, as the client's key does:
// TSharedFragmentsContainer<FConstSharedStruct> is nested in
// FMassEntityManager). Two names have overloads. FName::AppendString has three:
// its row is the FString& one the SDK calls, the start the loader's own pattern
// for it matches. UE::StructUtils::GetStructInstanceCrc32 has two: its row is
// the one taking a UScriptStruct, the struct's bytes and a CRC, which the
// client's pattern for the key matches on this exe (the FConstStructView one is
// a short wrapper that jumps to it). A global row (kind 4, or the loader's scoped
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
// A key naming a class and one of its base classes
// (ACrGatherableBaseActor::ICrGatherableInterface) is where that base sits
// inside the class, with the base's size.
#pragma once

#include "hooks/game/version_map_built.h"

namespace VersionMap
{

// The server's build here is the one the loader's start check was written for.
static_assert(kBuiltCL == 127004, "the server table is measured on CL-127004; move it with kBuiltCL");

inline constexpr std::array<Build, 1> kServerBuilds = {{
	{ 127004, 0x2E3595FF, 0x0E2A8000, "server steam_build 25642172" },
}};

inline constexpr std::array<FunctionRow, 38> kServerFunctionRows = {{
	// ACrGameModeBase::GetProfessionForNewPlayer
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06E7DBB0, nullptr },
	// ACrGameModeBase::PreLogin
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06E92390, nullptr },
	// ACrGameStateBase::UpdatePause
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06EA2AF0, nullptr },
	// ACrGatherableBaseActor::DepleteResource
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06F3D960, nullptr },
	// FCrCraftingFragment::StaticStruct
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06B89D90, nullptr },
	// FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05E015D0, nullptr },
	// FJsonObjectConverter::UStructToJsonObjectString
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x02945D00, nullptr },
	// FMassEntityConfig::DestroyEntityTemplate
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05D869E0, nullptr },
	// FMassEntityConfig::GetOrCreateEntityTemplate
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05D89CA0, nullptr },
	// FMassEntityManager::InternalGetFragmentDataPtr
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03C24BC0, nullptr },
	// FMassEntityManager::IsEntityValid
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03C250C0, nullptr },
	// FName::AppendString
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x0142A250, nullptr },
	// FPaths::ProjectSavedDir
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x013F7D90, nullptr },
	// FWeakObjectPtr::operator=(FObjectPtr)
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x0173D680, nullptr },
	// GNames (NamePoolData)
	{ 127004, 0x2E3595FF, 0x0E2A8000, 4, 0x00000000, false, nullptr, 0, -1, -1, 0x0D2BD600, nullptr },
	// GWorld
	{ 127004, 0x2E3595FF, 0x0E2A8000, 4, 0x00000000, false, nullptr, 0, -1, -1, 0x0D50EE38, nullptr },
	// ModLoader/GObjects (GUObjectArray.ObjObjects)
	{ 127004, 0x2E3595FF, 0x0E2A8000, 6, 0xD7B4F6BF, false, nullptr, 0, -1, -1, 0x0D3A1A50, nullptr },
	// TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x03C16DB0, nullptr },
	// UAuItemsComponent::AddNewItem
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x05FD95A0, nullptr },
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
	// UE::StructUtils::GetStructInstanceCrc32
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x015600D0, nullptr },
	// UMassActorSubsystem::GetEntityHandleFromActor
	{ 127004, 0x2E3595FF, 0x0E2A8000, 1, 0x00000000, false, nullptr, 0, -1, -1, 0x06C92C50, nullptr },
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

inline constexpr std::array<FieldRow, 116> kServerFieldRows = {{
	// AActor::Owner
	{ 127004, 0x158, 0x8, 0x00, nullptr },
	// AActor::bAlwaysRelevant
	{ 127004, 0x58, 0x1, 0x08, nullptr },
	// ACrGameStateBase::WaveTimerActor
	{ 127004, 0x318, 0x8, 0x00, nullptr },
	// ACrGatherableBaseActor::ICrGatherableInterface
	{ 127004, 0x2B0, 0x8, 0x00, nullptr },
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
	// APlayerController::PlayerState
	{ 127004, 0x2B0, 0x8, 0x00, nullptr },
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
	// FMassEnemyHealthFragment::HP
	{ 127004, 0x0, 0x4, 0x00, nullptr },
	// FMassEntityManager::ConstSharedFragmentsContainer
	{ 127004, 0x120, 0xB0, 0x00, nullptr },
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

inline constexpr std::array<Index, 38> kServerFunctions = {{
	{ "ACrGameModeBase::GetProfessionForNewPlayer", 0, 1 },
	{ "ACrGameModeBase::PreLogin", 1, 1 },
	{ "ACrGameStateBase::UpdatePause", 2, 1 },
	{ "ACrGatherableBaseActor::DepleteResource", 3, 1 },
	{ "FCrCraftingFragment::StaticStruct", 4, 1 },
	{ "FCrMassEntityReplicationHelper::FCrMassEntityReplicationHelper", 5, 1 },
	{ "FJsonObjectConverter::UStructToJsonObjectString", 6, 1 },
	{ "FMassEntityConfig::DestroyEntityTemplate", 7, 1 },
	{ "FMassEntityConfig::GetOrCreateEntityTemplate", 8, 1 },
	{ "FMassEntityManager::InternalGetFragmentDataPtr", 9, 1 },
	{ "FMassEntityManager::IsEntityValid", 10, 1 },
	{ "FName::AppendString", 11, 1 },
	{ "FPaths::ProjectSavedDir", 12, 1 },
	{ "FWeakObjectPtr::operator=(FObjectPtr)", 13, 1 },
	{ "GNames (NamePoolData)", 14, 1 },
	{ "GWorld", 15, 1 },
	{ "ModLoader/GObjects (GUObjectArray.ObjObjects)", 16, 1 },
	{ "TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd", 17, 1 },
	{ "UAuItemsComponent::AddNewItem", 18, 1 },
	{ "UCrBaseCoreSubsystem::GetBaseCoresRangedBuilding", 19, 1 },
	{ "UCrBuildingCustomNameSubsystem::GetBuildingCustomNameByReplicationHelper", 20, 1 },
	{ "UCrBuildingFunctionLibrary::AddTemperatureToEntity", 21, 1 },
	{ "UCrDedicatedServerSettingsComp::ParseSettings", 22, 1 },
	{ "UCrMassEnviroWaveSubsystem::AddEntitiesToWave", 23, 1 },
	{ "UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave", 24, 1 },
	{ "UCrMassPersistentIDSubsystem::GetOrAddIDForHandle", 25, 1 },
	{ "UCrMassPersistentIDSubsystem::SetIDHandlePair", 26, 1 },
	{ "UCrSaveSubsystem::GetSaveData", 27, 1 },
	{ "UCrSaveSubsystem::OnPreLoadMap", 28, 1 },
	{ "UCrSaveSubsystem::SaveNextSaveGame", 29, 1 },
	{ "UE::StructUtils::GetStructInstanceCrc32", 30, 1 },
	{ "UMassActorSubsystem::GetEntityHandleFromActor", 31, 1 },
	{ "UMassSignalSubsystem::SignalEntities", 32, 1 },
	{ "UMassSignalSubsystem::SignalEntity", 33, 1 },
	{ "UObject::ProcessEvent", 34, 1 },
	{ "UReplicationGraph::AddNetworkActor", 35, 1 },
	{ "UReplicationGraph::RemoveNetworkActor", 36, 1 },
	{ "UWorld::GetSubsystem<UMassEntitySubsystem>", 37, 1 },
}};

inline constexpr std::array<Index, 116> kServerFields = {{
	{ "AActor::Owner", 0, 1 },
	{ "AActor::bAlwaysRelevant", 1, 1 },
	{ "ACrGameStateBase::WaveTimerActor", 2, 1 },
	{ "ACrGatherableBaseActor::ICrGatherableInterface", 3, 1 },
	{ "ACrGatherableSpawnersRepActor::RepEnviroWaveStageChange", 4, 1 },
	{ "ACrGatherableSpawnersRepActor::RepEnviroWaveTypeChange", 5, 1 },
	{ "ACrGatherableSpawnersRepActor::RepGlobalGatherablePCGSeed", 6, 1 },
	{ "ACrPCGActorBase::LocalSeed", 7, 1 },
	{ "ACrPCGGatherableSpawnerVolume::LocalSeed", 8, 1 },
	{ "ACrPCGVolume::LocalSeed", 9, 1 },
	{ "ACrWaveTimerActor::NextPhase", 10, 1 },
	{ "ACrWaveTimerActor::NextTime", 11, 1 },
	{ "ACrWaveTimerActor::bPause", 12, 1 },
	{ "AGameStateBase::PlayerArray", 13, 1 },
	{ "APlayerController::PlayerState", 14, 1 },
	{ "APlayerState::CompressedPing", 15, 1 },
	{ "APlayerState::PlayerNamePrivate", 16, 1 },
	{ "AWorldSettings::TimeDilation", 17, 1 },
	{ "AuItemRecipeData_GetOutputItem", 18, 1 },
	{ "AuItemRecipeData_GetOutputItem::ReturnValue", 19, 1 },
	{ "FAuAPMassFragment", 20, 1 },
	{ "FAuSimpleItem", 21, 1 },
	{ "FAuSimpleItem::Count", 22, 1 },
	{ "FAuSimpleItem::ItemDataBase", 23, 1 },
	{ "FBaseCoreReplicationSaveData", 24, 1 },
	{ "FBuildingCustomNameSaveData", 25, 1 },
	{ "FCrAntennaSaveData", 26, 1 },
	{ "FCrBaseCoreSaveData", 27, 1 },
	{ "FCrBuildingGraphData::NodeDatas", 28, 1 },
	{ "FCrBuildingStabilitySubsystemState::BuildingFoundationData", 29, 1 },
	{ "FCrBuildingStabilitySubsystemState::GraphData", 30, 1 },
	{ "FCrCharacterPlayerBaseSaveDataPerPlayer", 31, 1 },
	{ "FCrCharacterPlayersBaseSaveData", 32, 1 },
	{ "FCrCraftingFragment::CraftingMultiplier", 33, 1 },
	{ "FCrCraftingFragment::CurrentRecipe", 34, 1 },
	{ "FCrCraftingFragment::SelectedRecipe", 35, 1 },
	{ "FCrEntitySaveData", 36, 1 },
	{ "FCrEntitySaveData::FragmentValues", 37, 1 },
	{ "FCrEntitySaveData::Tags", 38, 1 },
	{ "FCrEnviroWaveSettings", 39, 1 },
	{ "FCrEnviroWaveSettings::PreWaveDuration", 40, 1 },
	{ "FCrEnviroWaveSettings::WaveEndPosition", 41, 1 },
	{ "FCrEnviroWaveSettings::WaveFadeoutBurningDuration", 42, 1 },
	{ "FCrEnviroWaveSettings::WaveFadeoutFadingDuration", 43, 1 },
	{ "FCrEnviroWaveSettings::WaveFadeoutFireWaveDuration", 44, 1 },
	{ "FCrEnviroWaveSettings::WaveGrowbackMoonPhaseDuration", 45, 1 },
	{ "FCrEnviroWaveSettings::WaveGrowbackRegrowthDuration", 46, 1 },
	{ "FCrEnviroWaveSettings::WaveGrowbackRegrowthStartDuration", 47, 1 },
	{ "FCrEnviroWaveSettings::WavePreWaveExplosionDuration", 48, 1 },
	{ "FCrEnviroWaveSettings::WaveSpeed", 49, 1 },
	{ "FCrEnviroWaveSettings::WaveStartPosition", 50, 1 },
	{ "FCrGameVersion", 51, 1 },
	{ "FCrGridEntityHandle", 52, 1 },
	{ "FCrGridEntityHandle::Data", 53, 1 },
	{ "FCrGridEntityHandle::EntityHandle", 54, 1 },
	{ "FCrMassActorReplicationHelper", 55, 1 },
	{ "FCrMassActorReplicationHelper::Actor", 56, 1 },
	{ "FCrMassEntityReplicationHelper", 57, 1 },
	{ "FCrMassSaveData::StabilitySubsystemState", 58, 1 },
	{ "FCrMassTemperatureFragment::CurrentHeat", 59, 1 },
	{ "FCrMassTemperatureFragment::LastUpdateTime", 60, 1 },
	{ "FCrPlayersMapMenuState", 61, 1 },
	{ "FCrSaveGameData", 62, 1 },
	{ "FCrSaveGameData::GameVersion", 63, 1 },
	{ "FCrSaveGameData::Level", 64, 1 },
	{ "FCrSaveGameData::Timestamp", 65, 1 },
	{ "FCrSaveGameData::WorldAudioTimeSeconds", 66, 1 },
	{ "FCrSaveGameData::WorldRealTimeSeconds", 67, 1 },
	{ "FCrSaveGameData::WorldTimeSeconds", 68, 1 },
	{ "FCrSaveGameData::WorldUnpausedTimeSeconds", 69, 1 },
	{ "FCrZiplineReplicatorSaveData", 70, 1 },
	{ "FCrZiplineSaveData", 71, 1 },
	{ "FGameStateSaveData", 72, 1 },
	{ "FHttpServerRequest::Verb", 73, 1 },
	{ "FMassActorFragment::Actor", 74, 1 },
	{ "FMassEnemyHealthFragment::HP", 75, 1 },
	{ "FMassEntityManager::ConstSharedFragmentsContainer", 76, 1 },
	{ "FTransform::Translation", 77, 1 },
	{ "FTransformFragment::Transform", 78, 1 },
	{ "FUObjectItem::SerialNumber", 79, 1 },
	{ "UAuItemDataBase::ItemName", 80, 1 },
	{ "UAuItemDataBase::UniqueItemName", 81, 1 },
	{ "UAuItemDataBaseSubsystem::ItemDataBaseArray", 82, 1 },
	{ "UCrDedicatedServerSettingsComp::SaveGameInterval", 83, 1 },
	{ "UCrDedicatedServerSettingsComp::SaveGameName", 84, 1 },
	{ "UCrDedicatedServerSettingsComp::SessionName", 85, 1 },
	{ "UCrDedicatedServerSettingsComp::bLoadSavedGame", 86, 1 },
	{ "UCrDedicatedServerSettingsComp::bStartNewGame", 87, 1 },
	{ "UCrEnviroWaveSubsystem::CurrentFadeoutSubstage", 88, 1 },
	{ "UCrEnviroWaveSubsystem::CurrentGrowbackSubstage", 89, 1 },
	{ "UCrEnviroWaveSubsystem::CurrentPreWaveSubstage", 90, 1 },
	{ "UCrEnviroWaveTimerSubsystem::NextWaveTimer", 91, 1 },
	{ "UCrEnviroWaveTimerSubsystem::TimerActor", 92, 1 },
	{ "UCrEnviroWaveTimerSubsystem::bStopWaves", 93, 1 },
	{ "UCrEnviroWaveTimerSubsystem::bWaitingForNextWave", 94, 1 },
	{ "UCrGameInstance::ServerSessionName", 95, 1 },
	{ "UCrMassPersistentIDSubsystem::HandleIDMap", 96, 1 },
	{ "UCrMassPersistentIDSubsystem::IDHandleMap", 97, 1 },
	{ "UCrMassPersistentIDSubsystem::MaxID", 98, 1 },
	{ "UCrReplicationGraph::ClassRepNodePolicies", 99, 1 },
	{ "UCrSaveSubsystem::OnAfterSave", 100, 1 },
	{ "UCrSaveSubsystem::OnPreSaveStart", 101, 1 },
	{ "UCrSaveSubsystem::SaveData", 102, 1 },
	{ "UMassActorSubsystem::ActorManager", 103, 1 },
	{ "UMassEntitySubsystem::EntityManager", 104, 1 },
	{ "UMassSignalSubsystem::CachedWorld", 105, 1 },
	{ "UNetDriver::ClientConnections", 106, 1 },
	{ "UObject::ClassPrivate", 107, 1 },
	{ "UObject::InternalIndex", 108, 1 },
	{ "UObject::OuterPrivate", 109, 1 },
	{ "UWorld::AudioTimeSeconds", 110, 1 },
	{ "UWorld::GameState", 111, 1 },
	{ "UWorld::NetDriver", 112, 1 },
	{ "UWorld::RealTimeSeconds", 113, 1 },
	{ "UWorld::TimeSeconds", 114, 1 },
	{ "UWorld::UnpausedTimeSeconds", 115, 1 },
}};

inline constexpr std::array<AbsentField, 0> kServerAbsentFields = {};

}
