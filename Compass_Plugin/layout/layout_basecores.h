#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "../plugin_helpers.h"

namespace Layout
{

struct BaseCoreEntry
{
	SDK::FVector location;
	SDK::uint8   upgradeLevel;
	bool         bIsAttacked;
	bool         bIsInfected;
	std::wstring name;
};

// Returns all ACrBaseCore actors in the level, with custom names resolved.
inline std::vector<BaseCoreEntry> ScanBaseCores(SDK::UWorld* world)
{
	std::vector<BaseCoreEntry> result;
	if (!world)
		return result;

	// Cache name subsystem once per scan
	LOG_TRACE("[Compass] ScanBaseCores: resolving UCrBuildingCustomNameSubsystem...");
	SDK::UCrBuildingCustomNameSubsystem* nameSys =
		static_cast<SDK::UCrBuildingCustomNameSubsystem*>(
			SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
				world,
				SDK::UCrBuildingCustomNameSubsystem::StaticClass()));
	LOG_TRACE("[Compass] ScanBaseCores: nameSys=%p", (void*)nameSys);

	SDK::TArray<SDK::AActor*> actors;
	SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ACrBaseCore::StaticClass(), &actors);
	LOG_TRACE("[Compass] ScanBaseCores: GetAllActorsOfClass returned %d actors", actors.Num());

	for (int i = 0; i < actors.Num(); i++)
	{
		SDK::AActor* actor = actors[i];
		if (!actor)
			continue;

		auto* core = static_cast<SDK::ACrBaseCore*>(actor);

		BaseCoreEntry e;
		e.location     = core->K2_GetActorLocation();
		e.upgradeLevel = core->UpgradeLevel;
		e.bIsAttacked  = core->bIsAttacked;
		e.bIsInfected  = core->IsInfected;

		// 1. Player-set custom name
		if (nameSys)
		{
			SDK::FString fs = nameSys->GetBuildingCustomName(core);
			const wchar_t* p = fs.CStr();
			e.name = (p && fs.Num() > 0) ? p : L"";
			LOG_TRACE("[Compass] ScanBaseCores[%d]: custom name = '%ls'", i, e.name.c_str());
		}

		// 2. Interaction component display name
		if (e.name.empty())
		{
			SDK::TArray<SDK::UActorComponent*> comps =
				core->K2_GetComponentsByClass(SDK::UCrInteractionComponent::StaticClass());
			for (int j = 0; j < comps.Num(); ++j)
			{
				auto* ic = static_cast<SDK::UCrInteractionComponent*>(comps[j]);
				if (!ic) continue;
				{
					SDK::FText ft = ic->GetInteractionDisplayName();
					const wchar_t* p = ft.GetStringRef().CStr();
					e.name = (p && ft.GetStringRef().Num() > 0) ? p : L"";
				}
				if (!e.name.empty()) break;
			}
		}

		// 3. Hard fallback
		if (e.name.empty())
			e.name = L"Base Core";

		LOG_TRACE("[Compass] ScanBaseCores[%d]: '%ls' at (%.0f, %.0f, %.0f)",
			i, e.name.c_str(), e.location.X, e.location.Y, e.location.Z);
		result.push_back(e);
	}

	// ---------------------------------------------------------------------------
	// Fallback: BuildingsMarkerDataContainer on ACrMapMenuDataReplicationHelper.
	//
	// UCrMapManuSubsystem::UpdateMapMenuData ticks every frame and calls
	// GatherBuildingsData (which iterates Mass building entities) only when:
	//   1. GameState->MapMenuDataReplicationHelper is non-null (replicated actor).
	//   2. At least one local PC has byte+3816 != 0 (the "map is open" flag).
	//
	// The helper pointer is typically present as soon as GameState exists.
	// The flag is normally only set when the player opens the map UI.
	// We force byte+3816=1 on the local PC whenever bmd is empty so the
	// subsystem populates BuildingsMarkerDataContainer without any player action.
	// The fake-helper spawn path below is a backstop for the rare case where
	// the helper pointer itself is absent.
	//
	// All statics are cleared on world change.
	// Location is FVector2f (X,Y only); Z is set to 0 for unloaded actors.
	// ---------------------------------------------------------------------------

	// Persistent cache of distant core locations (survives empty bmd scans).
	// All statics are auto-reset when the world pointer changes.
	static SDK::UWorld*                           s_lastWorld            = nullptr;
	static std::vector<SDK::FVector>              s_distantCoreCache;
	static bool                                   s_helperSpawnAttempted = false;
	static SDK::ACrMapMenuDataReplicationHelper*  s_fakeHelper           = nullptr;

	if (world != s_lastWorld)
	{
		LOG_TRACE("[ScanBaseCores] World changed (%p -> %p) — resetting distant-core statics", (void*)s_lastWorld, (void*)world);
		s_lastWorld            = world;
		s_distantCoreCache.clear();
		s_helperSpawnAttempted = false;
		s_fakeHelper           = nullptr;
	}
	else
	{
		LOG_TRACE("[ScanBaseCores] World unchanged (%p), spawnAttempted=%d fakeHelper=%p",
			(void*)world, (int)s_helperSpawnAttempted, (void*)s_fakeHelper);
	}

	auto* gameState = world->GameState
		? static_cast<SDK::ACrGameStateBase*>(world->GameState)
		: nullptr;
	LOG_TRACE("[ScanBaseCores] gameState=%p", (void*)gameState);

	auto* mapHelper = gameState ? gameState->MapMenuDataReplicationHelper : nullptr;
	LOG_TRACE("[ScanBaseCores] mapHelper=%p (from GameState)", (void*)mapHelper);

	// Spawn a local helper if the real one hasn't arrived yet.
	if (!mapHelper && !s_helperSpawnAttempted && gameState)
	{
		s_helperSpawnAttempted = true;
		LOG_TRACE("[ScanBaseCores] MapMenuDataReplicationHelper absent — spawning local stand-in");

		SDK::FTransform transform{};
		transform.Rotation.W = 1.0f;            // identity quaternion
		transform.Scale3D    = { 1.0f, 1.0f, 1.0f };

		SDK::AActor* spawned = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
			world,
			SDK::ACrMapMenuDataReplicationHelper::StaticClass(),
			transform,
			SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
			nullptr,
			SDK::ESpawnActorScaleMethod::OverrideRootScale);

		LOG_TRACE("[ScanBaseCores] BeginDeferredActorSpawnFromClass returned %p", (void*)spawned);

		if (spawned)
		{
			SDK::UGameplayStatics::FinishSpawningActor(spawned, transform,
				SDK::ESpawnActorScaleMethod::OverrideRootScale);
			s_fakeHelper = static_cast<SDK::ACrMapMenuDataReplicationHelper*>(spawned);
			gameState->MapMenuDataReplicationHelper = s_fakeHelper;
			mapHelper = s_fakeHelper;
			LOG_TRACE("[ScanBaseCores] Spawned local helper %p — UCrMapManuSubsystem will populate it next tick", (void*)s_fakeHelper);
		}
		else
		{
			LOG_WARN("[ScanBaseCores] BeginDeferredActorSpawnFromClass returned null — distant cores unavailable");
		}
	}
	else if (!mapHelper && s_helperSpawnAttempted)
	{
		LOG_TRACE("[ScanBaseCores] Spawn already attempted but mapHelper still null (waiting for first tick)");
	}
	else if (!mapHelper && !gameState)
	{
		LOG_TRACE("[ScanBaseCores] No gameState — cannot spawn helper yet");
	}

	if (mapHelper)
	{
		auto& bmd = mapHelper->BuildingsMarkerDataContainer.BuildingsMarkerData;
		LOG_TRACE("[ScanBaseCores] mapHelper=%p bmd.Num()=%d", (void*)mapHelper, bmd.Num());

		if (bmd.Num() > 0)
		{
			// Fresh data available -- rebuild the distant-core cache
			s_distantCoreCache.clear();
			for (int i = 0; i < bmd.Num(); i++)
			{
				const auto& item = bmd[i];
				const std::string uniqueName = item.BuildingUniqueName.ToString();
				LOG_TRACE("[ScanBaseCores] bmd[%d] UniqueName='%s' loc=(%.0f,%.0f)",
					i, uniqueName.c_str(), item.Location.X, item.Location.Y);
				if (uniqueName != "BaseCore")
					continue;
				s_distantCoreCache.push_back({ item.Location.X, item.Location.Y, 0.0f });
			}
			LOG_TRACE("[ScanBaseCores] bmd cache rebuilt: %d distant cores from %d total entries",
				(int)s_distantCoreCache.size(), bmd.Num());
		}
		else
		{
			LOG_TRACE("[ScanBaseCores] bmd is empty -- waiting for subsystem tick, cache has %d entries",
				(int)s_distantCoreCache.size());
		}
	}
	else
	{
		LOG_TRACE("[ScanBaseCores] mapHelper is null — skipping bmd read, cache has %d entries",
			(int)s_distantCoreCache.size());
	}

	// Merge cache into result, deduplicating against loaded actors (XY, 2000 UU)
	int added = 0;
	for (const auto& loc : s_distantCoreCache)
	{
		bool duplicate = false;
		for (const auto& existing : result)
		{
			const float dx = existing.location.X - loc.X;
			const float dy = existing.location.Y - loc.Y;
			if (dx * dx + dy * dy < 2000.0f * 2000.0f) { duplicate = true; break; }
		}
		if (duplicate) continue;

		BaseCoreEntry e;
		e.location     = loc;
		e.upgradeLevel = 0;
		e.bIsAttacked  = false;
		e.bIsInfected  = false;
		e.name         = L"Base Core";
		result.push_back(e);
		++added;
	}

	LOG_TRACE("[ScanBaseCores] Merge done: %d from cache added, %d skipped as duplicates, total=%d",
		added, (int)s_distantCoreCache.size() - added, (int)result.size());

	return result;
}

} // namespace Layout
