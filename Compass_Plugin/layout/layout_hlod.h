#pragma once

#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include "Engine_classes.hpp"
#include "layout_markers.h"
#include "layout_basecores.h"
#include "../plugin_helpers.h"

namespace Layout
{

// ---------------------------------------------------------------------------
// ScanHLOD — fallback scanner for distant POIs and base cores that are
// outside the World Partition streaming radius and therefore missing from
// the actor arrays scanned by ScanMarkers / ScanBaseCores.
//
// AWorldPartitionHLOD / ALODActor actors carry UStaticMeshComponent children
// whose mesh asset names reference the original asset.  We pattern-match those
// names to identify the POI/BaseCore type, then use the HLOD actor's world
// location as an approximate position.
//
// Any HLOD-derived entry within HLOD_DEDUP_DIST of an already-found real-actor
// entry of the same category is discarded — the real entry is more accurate.
// ---------------------------------------------------------------------------

static constexpr float HLOD_DEDUP_DIST_SQ = 5000.0f * 5000.0f; // 50 m

static inline float DistSq2D(const SDK::FVector& a, const SDK::FVector& b)
{
	const float dx = a.X - b.X;
	const float dy = a.Y - b.Y;
	return dx * dx + dy * dy;
}

// Case-insensitive substring search (ASCII).
static inline bool ContainsCI(const std::string& haystack, const char* needle)
{
	const size_t nlen = strlen(needle);
	if (nlen > haystack.size()) return false;
	for (size_t i = 0; i + nlen <= haystack.size(); ++i)
	{
		bool match = true;
		for (size_t j = 0; j < nlen && match; ++j)
			match = (tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j]));
		if (match) return true;
	}
	return false;
}

inline void ScanHLOD(SDK::UWorld* world,
	std::vector<MarkerEntry>&   inOutMarkers,
	std::vector<BaseCoreEntry>& inOutCores)
{
	if (!world) return;

	struct MarkerKw { const char* keyword; PoiType type; };
	static const MarkerKw kwMarkers[] = {
		{ "Cave",          PoiType::Cave          },
		{ "Antena",        PoiType::Antena        },
		{ "Antenna",       PoiType::Antena        },
		{ "AbandonedBase", PoiType::AbandonedBase },
		{ "Obelisk",       PoiType::Obelisk       },
	};

	int hlodActors = 0, hlodAdded = 0;

	// GetAllActorsOfClass covers actors in any container (World Partition managed
	// actors, always-loaded levels, etc.) — more reliable than manual level walk.
	SDK::TArray<SDK::AActor*> wpHlodList, lodList;
	if (SDK::AWorldPartitionHLOD::StaticClass())
		SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::AWorldPartitionHLOD::StaticClass(), &wpHlodList);
	if (SDK::ALODActor::StaticClass())
		SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ALODActor::StaticClass(), &lodList);

	LOG_DEBUG("[ScanHLOD] GetAllActorsOfClass: WorldPartitionHLOD=%d  LODActor=%d",
		wpHlodList.Num(), lodList.Num());

	auto ProcessActor = [&](SDK::AActor* actor)
	{
		if (!actor) return;
		hlodActors++;
		const SDK::FVector hlodLoc = actor->K2_GetActorLocation();

		SDK::TArray<SDK::UActorComponent*> comps =
			actor->K2_GetComponentsByClass(SDK::UStaticMeshComponent::StaticClass());

		LOG_TRACE("[ScanHLOD] actor='%s' at (%.0f,%.0f,%.0f) — %d mesh comps",
			actor->Class ? actor->Class->GetName().c_str() : "?",
			hlodLoc.X, hlodLoc.Y, hlodLoc.Z, comps.Num());

		for (int j = 0; j < comps.Num(); j++)
		{
			auto* smc = static_cast<SDK::UStaticMeshComponent*>(comps[j]);
			if (!smc || !smc->StaticMesh) continue;

			const std::string meshName = smc->StaticMesh->GetName();
			LOG_TRACE("[ScanHLOD]   comp[%d] mesh='%s'", j, meshName.c_str());

			// --- BaseCore check ---
			if (ContainsCI(meshName, "BaseCore") || ContainsCI(meshName, "Base_Core") ||
				ContainsCI(meshName, "BaseCamp"))
			{
				bool dup = false;
				for (const auto& existing : inOutCores)
					if (DistSq2D(existing.location, hlodLoc) < HLOD_DEDUP_DIST_SQ) { dup = true; break; }
				if (dup) { LOG_TRACE("[ScanHLOD]   -> BaseCore dup, skipped"); continue; }

				BaseCoreEntry e;
				e.location     = hlodLoc;
				e.upgradeLevel = 0;
				e.bIsAttacked  = false;
				e.bIsInfected  = false;
				e.name         = L"Base Core";
				inOutCores.push_back(e);
				hlodAdded++;
				LOG_TRACE("[ScanHLOD] Added distant BaseCore (mesh='%s') at (%.0f,%.0f,%.0f)",
					meshName.c_str(), hlodLoc.X, hlodLoc.Y, hlodLoc.Z);
				return; // one entry per HLOD actor
			}

			// --- POI marker checks ---
			for (const auto& kw : kwMarkers)
			{
				if (!ContainsCI(meshName, kw.keyword)) continue;

				bool dup = false;
				for (const auto& existing : inOutMarkers)
					if (existing.type == kw.type &&
						DistSq2D(existing.location, hlodLoc) < HLOD_DEDUP_DIST_SQ)
					{ dup = true; break; }
				if (dup) { LOG_TRACE("[ScanHLOD]   -> %s dup, skipped", kw.keyword); return; }

				MarkerEntry e;
				e.location = hlodLoc;
				e.type     = kw.type;
				e.state    = PoiState::VisibleUnknown;
				inOutMarkers.push_back(e);
				hlodAdded++;
				LOG_DEBUG("[ScanHLOD] Added distant %s (mesh='%s') at (%.0f,%.0f,%.0f)",
					kw.keyword, meshName.c_str(), hlodLoc.X, hlodLoc.Y, hlodLoc.Z);
				return; // one entry per HLOD actor
			}
		}
	};

	for (int i = 0; i < wpHlodList.Num(); i++) ProcessActor(wpHlodList[i]);
	for (int i = 0; i < lodList.Num();    i++) ProcessActor(lodList[i]);

	LOG_DEBUG("[ScanHLOD] Done — actors checked=%d, entries added=%d", hlodActors, hlodAdded);
}

} // namespace Layout
