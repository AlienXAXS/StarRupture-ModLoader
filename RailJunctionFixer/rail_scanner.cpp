#include "rail_scanner.h"
#include "plugin_helpers.h"
#include "LogisticsFragmentFixer.h"   // ReadAt<T> template

#include "Engine_classes.hpp"  // UWorld, ULevel, AActor, USplineComponent
#include "CoreUObject_classes.hpp"    // UObject::IsA
#include "Chimera_classes.hpp"        // ACrDronePathPointConnection, ACrDronePathPoint,
// ACrBuildingSplineActorBase, ACrBuildingActorBase

#include <vector>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <cmath>

namespace RailJunctionFixer::RailScanner
{
	// ---------------------------------------------------------------------------
	// Offsets derived from Chimera_classes.hpp / Chimera_structs.hpp
	// ---------------------------------------------------------------------------

	// ACrBuildingSplineActorBase (0x0658 total, inherits ACrBuildingActorBase at 0x0638)
	static constexpr ptrdiff_t OFF_Spline = 0x0638; // USplineComponent*
	static constexpr ptrdiff_t OFF_ConnectedStartActor = 0x0640; // AActor*
	static constexpr ptrdiff_t OFF_ConnectedEndActor = 0x0648; // AActor*

	// ACrBuildingActorBase (0x0638 total, inherits AActor)
	static constexpr ptrdiff_t OFF_MassActorComponent = 0x02F0; // UCrMassActorComponent*

	// ACrDronePathPointConnectionMassSpawner
	//   inherits ACrSimpleBuildingMassSpawner -> ACrSimpleMassSpawnerBase -> AAuAPSimpleMassSpawner -> AActor
	//   NOT ACrBuildingSplineActorBase -- different member layout
	static constexpr ptrdiff_t OFF_MS_SplineComponent = 0x0310; // USplineComponent*
	static constexpr ptrdiff_t OFF_MS_StartTransform = 0x0330; // FTransform (0x60 bytes)
	static constexpr ptrdiff_t OFF_MS_EndTransform = 0x0390; // FTransform (0x60 bytes)
	static constexpr ptrdiff_t OFF_MS_ConnectingSpawners = 0x02F0; // TArray<ACrSimpleBuildingMassSpawner*>
	// FTransform::Translation is at +0x20 within the FTransform
	// -> StartTransform.Translation = actor + 0x0350
	// -> EndTransform.Translation   = actor + 0x03B0
	static constexpr ptrdiff_t FTRANSFORM_TRANSLATION_OFFSET = 0x20;

	// ---------------------------------------------------------------------------
	// How close two rail endpoints must be (in Unreal Units, 1 UU ~ 1 cm) for us
	// to flag them as "suspiciously sharing the same physical position".
	// In a correct save each rail attaches at a distinct socket; this threshold
	// catches degenerate cases rather than the save-corruption bug (see header).
	// ---------------------------------------------------------------------------
	static constexpr double SAME_POS_THRESHOLD_UU = 20.0; // 20 cm

	// ---------------------------------------------------------------------------
	// Helpers
	// ---------------------------------------------------------------------------

	static double VecDistSq(const SDK::FVector& a, const SDK::FVector& b)
	{
		double dx = a.X - b.X;
		double dy = a.Y - b.Y;
		double dz = a.Z - b.Z;
		return dx * dx + dy * dy + dz * dz;
	}

	// Dot product of (endpoint - origin) with an axis vector.
	// Returns the signed "distance along axis": negative = left, positive = right.
	static double ProjectOnAxis(const SDK::FVector& endpoint,
	                            const SDK::FVector& origin,
	                            const SDK::FVector& axis)
	{
		double dx = endpoint.X - origin.X;
		double dy = endpoint.Y - origin.Y;
		double dz = endpoint.Z - origin.Z;
		return dx * axis.X + dy * axis.Y + dz * axis.Z;
	}

	// ---------------------------------------------------------------------------
	// SEH-safe trampoline helpers
	// ---------------------------------------------------------------------------
	// MSVC forbids __try/__except in functions with C++ objects that need
	// unwinding (std::vector, std::unordered_map, lambdas, etc.).
	// These thin wrappers isolate each SEH block into its own function.
	// ---------------------------------------------------------------------------

	// Try to get a UClass* via StaticClass(). Returns nullptr on any exception.
	// Note: StaticClass() internally constructs std::string, so we must use
	// C++ try/catch here (not __try/__except which conflicts with object unwinding).
	static SDK::UClass* TryGetStaticClass_RailConnection()
	{
		try { return SDK::ACrDronePathPointConnection::StaticClass(); }
		catch (...) { return nullptr; }
	}

	static SDK::UClass* TryGetStaticClass_DronePathPoint()
	{
		try { return SDK::ACrDronePathPoint::StaticClass(); }
		catch (...) { return nullptr; }
	}

	static SDK::UClass* TryGetStaticClass_ConnectionMassSpawner()
	{
		try { return SDK::ACrDronePathPointConnectionMassSpawner::StaticClass(); }
		catch (...) { return nullptr; }
	}

	// Try obj->IsA(cls) where obj may be any UObject. Returns false on SEH exception.
	// Safe to call on non-actor objects -- IsA is a UObject method.
	static bool TryIsA(SDK::AActor* actor, SDK::UClass* cls)
	{
		__try { return actor->IsA(cls); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// World filter: for a standard or Mass Entity-spawned actor the outer chain is
	//   actor -> ULevel -> UWorld
	// Returns false on SEH exception (garbage outer pointer).
	static bool TryIsInWorld(SDK::UObject* obj, SDK::UWorld* world)
	{
		__try
		{
			SDK::UObject* lvl = obj->Outer;
			return lvl && (lvl->Outer == static_cast<SDK::UObject*>(world));
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Try to read spline data. Returns true on success and fills out params.
	// On SEH exception, sets *outNumPts to 0 and returns false.
	// On success with < 2 points, sets *outNumPts to the actual count and returns false.
	static bool TryReadSplineData(SDK::USplineComponent* spline,
	                              int32_t* outNumPts,
	                              SDK::FVector* outStartPos,
	                              SDK::FVector* outEndPos)
	{
		__try
		{
			int32_t numPts = spline->GetNumberOfSplinePoints();
			*outNumPts = numPts;
			if (numPts >= 2)
			{
				*outStartPos = spline->GetWorldLocationAtSplinePoint(0);
				*outEndPos = spline->GetWorldLocationAtSplinePoint(numPts - 1);
				return true;
			}
			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			*outNumPts = 0;
			return false;
		}
	}

	// Try to read junction location and right vector. Returns true on success.
	static bool TryReadJunctionTransform(SDK::AActor* junction,
	                                     SDK::FVector* outLoc,
	                                     SDK::FVector* outRight)
	{
		__try
		{
			*outLoc = junction->K2_GetActorLocation();
			*outRight = junction->GetActorRightVector();
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// ---------------------------------------------------------------------------
	// Per-rail data recorded at each junction end
	// ---------------------------------------------------------------------------
	struct RailEnd
	{
		SDK::AActor* railActor;
		SDK::FVector endpointPos; // world position of the spline point at this junction
		bool isStartEnd; // true  = spline point 0  (start)
		// false = spline point N-1 (end)
		int proposedSocketIdx; // filled in during junction analysis
	};

	// ---------------------------------------------------------------------------
	// ScanRailSocketState
	// ---------------------------------------------------------------------------
	void ScanRailSocketState(SDK::UWorld* world)
	{
		LOG_DEBUG("RailScanner: ---- Begin rail socket scan ----");

		if (!world)
		{
			LOG_ERROR("RailScanner: world is null, aborting scan");
			return;
		}

		// ------------------------------------------------------------------
		// 1. Get the UClass objects we need for IsA checks
		// ------------------------------------------------------------------
		SDK::UClass* railClass = TryGetStaticClass_RailConnection();
		SDK::UClass* junctionClass = TryGetStaticClass_DronePathPoint();

		if (!railClass)
		{
			LOG_ERROR("RailScanner: ACrDronePathPointConnection::StaticClass() failed - "
				"SDK mismatch or class not loaded yet");
			return;
		}

		LOG_DEBUG("RailScanner: railClass=%p  junctionClass=%p",
		          static_cast<void*>(railClass), static_cast<void*>(junctionClass));

		// ------------------------------------------------------------------
		// 2. Iterate GObjects to collect rail actors.
		//    Mass Entity BP actors spawned at runtime are NOT added to
		//    ULevel::Actors; they only appear in GObjects.
		//    World membership is tested via the outer chain:
		//      actor -> ULevel -> UWorld
		// ------------------------------------------------------------------
		//
		// junction ptr  ->  list of RailEnd records (one per rail-end at this junction)
		//
		std::unordered_map<SDK::AActor*, std::vector<RailEnd>> junctionMap;

		int nRailsTotal = 0;
		int nNullSpline = 0;
		int nMissingStart = 0;
		int nMissingEnd = 0;
		int nMissingBoth = 0;
		int nSplineError = 0;

		SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
		if (!objArray)
		{
			LOG_ERROR("RailScanner: GObjects is null - cannot iterate actors");
			return;
		}

		LOG_DEBUG("RailScanner: Iterating GObjects (%d elements) for ACrDronePathPointConnection...",
		          objArray->NumElements);

		for (int32_t i = 0; i < objArray->NumElements; ++i)
		{
			SDK::UObject* obj = objArray->GetByIndex(i);
			if (!obj || !obj->Class) continue;

			if (!TryIsInWorld(obj, world)) continue;
			if (!TryIsA(static_cast<SDK::AActor*>(obj), railClass)) continue;

			auto actor = static_cast<SDK::AActor*>(obj);
			++nRailsTotal;

			uintptr_t base = reinterpret_cast<uintptr_t>(actor);

			// Read ACrBuildingSplineActorBase members via offsets.
			auto startJunction = ReadAt<SDK::AActor*>(base, OFF_ConnectedStartActor);
			auto endJunction = ReadAt<SDK::AActor*>(base, OFF_ConnectedEndActor);
			auto spline = ReadAt<SDK::USplineComponent*>(base, OFF_Spline);

			const bool noStart = (startJunction == nullptr);
			const bool noEnd = (endJunction == nullptr);
			if (noStart) ++nMissingStart;
			if (noEnd) ++nMissingEnd;
			if (noStart && noEnd) ++nMissingBoth;

			if (!spline)
			{
				++nNullSpline;
				LOG_WARN("RailScanner: Rail %p has null USplineComponent", actor);
				continue;
			}

			// Get spline endpoint world positions via SDK UFunction wrappers.
			// GetWorldLocationAtSplinePoint and GetNumberOfSplinePoints are
			// BlueprintCallable wrappers -- they dispatch via ProcessEvent on the
			// server just as on the client.
			int32_t numPts = 0;
			SDK::FVector startPos = {}, endPos = {};
			bool splineOk = TryReadSplineData(spline, &numPts, &startPos, &endPos);

			if (!splineOk && numPts == 0)
			{
				// SEH exception path (numPts was set to 0 by the trampoline)
				++nSplineError;
				LOG_WARN("RailScanner: SEH exception reading spline for rail %p", actor);
				continue;
			}

			if (!splineOk)
			{
				LOG_WARN("RailScanner: Rail %p spline has < 2 points (%d)", actor, numPts);
				continue;
			}

			if (startJunction)
				junctionMap[startJunction].push_back({actor, startPos, true, -1});
			if (endJunction)
				junctionMap[endJunction].push_back({actor, endPos, false, -1});
		}

		LOG_DEBUG("RailScanner: ACrDronePathPointConnection pass: found %d actor(s)", nRailsTotal);

		// ------------------------------------------------------------------
		// 2b. MassSpawner fallback: if no ACrDronePathPointConnection actors
		//     were found, try ACrDronePathPointConnectionMassSpawner.
		//     These are the placed spawner actors whose endpoints come from
		//     StartTransform / EndTransform rather than spline points, and
		//     whose junction connections come from ConnectingSpawners TArray.
		// ------------------------------------------------------------------
		if (nRailsTotal == 0)
		{
			SDK::UClass* spawnerClass = TryGetStaticClass_ConnectionMassSpawner();
			if (!spawnerClass)
			{
				LOG_WARN("RailScanner: ACrDronePathPointConnectionMassSpawner::StaticClass() "
					"failed - no MassSpawner fallback available");
			}
			else
			{
				LOG_DEBUG("RailScanner: Falling back to ACrDronePathPointConnectionMassSpawner...");
				int nSpawners = 0;

				for (int32_t i = 0; i < objArray->NumElements; ++i)
				{
					SDK::UObject* obj = objArray->GetByIndex(i);
					if (!obj || !obj->Class) continue;

					if (!TryIsInWorld(obj, world)) continue;
					if (!TryIsA(static_cast<SDK::AActor*>(obj), spawnerClass)) continue;

					auto actor = static_cast<SDK::AActor*>(obj);
					++nSpawners;
					++nRailsTotal;

					uintptr_t base = reinterpret_cast<uintptr_t>(actor);

					// Endpoint positions from StartTransform.Translation and
					// EndTransform.Translation.  FTransform::Translation is at
					// +0x20 within the FTransform struct (UE5 double-precision).
					auto startPos = ReadAt<SDK::FVector>(
						base, OFF_MS_StartTransform + FTRANSFORM_TRANSLATION_OFFSET);
					auto endPos = ReadAt<SDK::FVector>(
						base, OFF_MS_EndTransform + FTRANSFORM_TRANSLATION_OFFSET);

					// ConnectingSpawners TArray layout: Data* at offset, Num at offset+8.
					// entries[0] = start junction spawner, entries[1] = end junction spawner.
					auto spawnerData = ReadAt<SDK::AActor**>(base, OFF_MS_ConnectingSpawners);
					int32_t spawnerCount = ReadAt<int32_t>(base, OFF_MS_ConnectingSpawners + 8);

					SDK::AActor* startJunction = (spawnerData && spawnerCount > 0) ? spawnerData[0] : nullptr;
					SDK::AActor* endJunction = (spawnerData && spawnerCount > 1) ? spawnerData[1] : nullptr;

					const bool noStart = (startJunction == nullptr);
					const bool noEnd = (endJunction == nullptr);
					if (noStart) ++nMissingStart;
					if (noEnd) ++nMissingEnd;
					if (noStart && noEnd) ++nMissingBoth;

					if (startJunction)
						junctionMap[startJunction].push_back({actor, startPos, true, -1});
					if (endJunction)
						junctionMap[endJunction].push_back({actor, endPos, false, -1});
				}

				LOG_DEBUG("RailScanner: MassSpawner fallback: found %d spawner actor(s)", nSpawners);
			}
		}

		LOG_DEBUG("RailScanner: Collection complete:");
		LOG_DEBUG("  Total rail actors found  : %d", nRailsTotal);
		LOG_DEBUG("  Null spline component    : %d", nNullSpline);
		LOG_DEBUG("  Missing start junction   : %d", nMissingStart);
		LOG_DEBUG("  Missing end junction     : %d", nMissingEnd);
		LOG_DEBUG("  Missing both junctions   : %d", nMissingBoth);
		LOG_DEBUG("  Spline read errors (SEH) : %d", nSplineError);
		LOG_DEBUG("  Distinct junctions seen  : %d", static_cast<int>(junctionMap.size()));

		// ------------------------------------------------------------------
		// 3. Analyse each junction
		// ------------------------------------------------------------------
		//
		// For each junction we:
		//   a) Check for endpoint positions that are suspiciously close
		//      (geometry-level sanity, not the save-corruption detection -- see header).
		//   b) Sort rail ends by their projection onto the junction's right vector.
		//    This left-to-right ordering is the proposed socket assignment.
		//
		// The "right vector" gives us the lateral axis of the junction.  On a
		// 3-way junction the sockets are typically laid out along this axis:
		//   socket 0 = leftmost,  socket 1 = centre,  socket 2 = rightmost.
		// (Verify in-game; the game may use a different convention.)
		//
		// If GetActorRightVector() fails we fall back to sorting by world-X then
		// world-Y, which gives a consistent (if not orientation-aware) ordering.
		//
		// ------------------------------------------------------------------

		int nJunctionsWithGeomIssue = 0;
		int nGeomConflictPairs = 0;

		for (auto& [junction, railEnds] : junctionMap)
		{
			if (railEnds.empty()) continue;

			const int nRails = static_cast<int>(railEnds.size());

			// ---- 3a. Geometry sanity: look for near-duplicate endpoint positions ----
			std::vector<std::pair<int, int>> conflictPairs;
			for (int i = 0; i < nRails; ++i)
			{
				for (int j = i + 1; j < nRails; ++j)
				{
					double dSq = VecDistSq(railEnds[i].endpointPos, railEnds[j].endpointPos);
					if (dSq < SAME_POS_THRESHOLD_UU * SAME_POS_THRESHOLD_UU)
						conflictPairs.push_back({i, j});
				}
			}

			if (!conflictPairs.empty())
			{
				++nJunctionsWithGeomIssue;
				nGeomConflictPairs += static_cast<int>(conflictPairs.size());
				LOG_WARN("RailScanner: Junction %p -- %d rails, %d GEOMETRY conflicts "
				         "(endpoints < %.0f UU apart):",
				         junction, nRails, static_cast<int>(conflictPairs.size()),
				         SAME_POS_THRESHOLD_UU);
				for (auto& [ia, ib] : conflictPairs)
				{
					double dist = std::sqrt(VecDistSq(railEnds[ia].endpointPos,
					                                  railEnds[ib].endpointPos));
					LOG_WARN("  rail %p (%s-end) and rail %p (%s-end) dist=%.2f UU",
					         railEnds[ia].railActor, railEnds[ia].isStartEnd ? "START" : "END",
					         railEnds[ib].railActor, railEnds[ib].isStartEnd ? "START" : "END",
					         dist);
				}
			}

			// ---- 3b. Get junction transform for lateral-axis projection ----
			SDK::FVector jLoc = {};
			SDK::FVector jRight = {};
			bool haveTransform = TryReadJunctionTransform(junction, &jLoc, &jRight);

			if (!haveTransform)
				LOG_WARN("RailScanner: SEH reading junction %p transform", junction);

			// ---- 3c. Compute projections and sort ----
			//
			// Each entry in sortedIdx is an index into railEnds[].
			// After sorting, sortedIdx[0] = the rail whose endpoint is furthest LEFT
			// (most negative projection onto jRight), sortedIdx[N-1] = furthest RIGHT.
			// Assign proposedSocketIdx = rank in this sorted order.
			//
			std::vector<int> sortedIdx(nRails);
			std::vector<double> projections(nRails, 0.0);
			std::iota(sortedIdx.begin(), sortedIdx.end(), 0);

			if (haveTransform)
			{
				for (int i = 0; i < nRails; ++i)
					projections[i] = ProjectOnAxis(railEnds[i].endpointPos, jLoc, jRight);

				std::sort(sortedIdx.begin(), sortedIdx.end(),
				          [&](int a, int b) { return projections[a] < projections[b]; });
			}
			else
			{
				// Fallback: sort by world X (then Y as tiebreak)
				std::sort(sortedIdx.begin(), sortedIdx.end(),
				          [&](int a, int b)
				          {
					          if (railEnds[a].endpointPos.X != railEnds[b].endpointPos.X)
						          return railEnds[a].endpointPos.X < railEnds[b].endpointPos.X;
					          return railEnds[a].endpointPos.Y < railEnds[b].endpointPos.Y;
				          });
			}

			// Write proposed socket index back into each RailEnd
			for (int rank = 0; rank < nRails; ++rank)
				railEnds[sortedIdx[rank]].proposedSocketIdx = rank;

			// ---- 3d. Log junction summary ----
			bool isJunctionClass = false;
			if (junctionClass)
				isJunctionClass = TryIsA(junction, junctionClass);

			LOG_DEBUG("RailScanner: Junction %p [%s] -- %d rail(s), "
			          "transform=%s, geomConflicts=%d",
			          junction,
			          isJunctionClass ? "CrDronePathPoint" : "unknown-class",
			          nRails,
			          haveTransform ? "OK" : "FAILED",
			          static_cast<int>(conflictPairs.size()));

			// Log each rail end in proposed socket order
			for (int rank = 0; rank < nRails; ++rank)
			{
				int idx = sortedIdx[rank];
				const RailEnd& re = railEnds[idx];
				LOG_DEBUG("  propSocket %d: rail=%p  %s-end"
				          "  pos=(%.1f, %.1f, %.1f)"
				          "  proj=%.1f",
				          rank,
				          re.railActor,
				          re.isStartEnd ? "START" : "END",
				          re.endpointPos.X,
				          re.endpointPos.Y,
				          re.endpointPos.Z,
				          haveTransform ? projections[idx] : 0.0);
			}
		}

		// ------------------------------------------------------------------
		// 4. Final summary
		// ------------------------------------------------------------------
		LOG_DEBUG("RailScanner: ---- Scan summary ----");
		LOG_DEBUG("  Rail actors          : %d", nRailsTotal);
		LOG_DEBUG("  Junctions referenced : %d", static_cast<int>(junctionMap.size()));
		LOG_DEBUG("  Geometry conflicts   : %d junctions, %d pairs",
		          nJunctionsWithGeomIssue, nGeomConflictPairs);
		LOG_DEBUG("RailScanner: ---- End rail socket scan ----");

		// ------------------------------------------------------------------
		// TODO (next step): Read Mass entity socket fragment data to detect
		// save-corruption directly.
		//
		// After SignalSocketEntities() runs, FCrLogisticsSocketRuntimeData.WorldPosition
		// on each rail entity reflects the junction socket it was assigned to.
		// In a bad save, multiple rail entities will all have the same WorldPosition
		// (all pointing to socket 0 of the junction).
		//
		// To read that data we need:
		//   1. actor->MassActorComponent (at ACrBuildingActorBase+0x02F0) -> EntityHandle
		// 2. FMassEntityManager::GetArchetypeForEntity(entityHandle) -> FMassArchetypeData*
		//   3. Scan/index into FMassArchetypeData to find FCrLogisticsSocketsFragment
		//      at the right offset for this entity's chunk slot
		//   4. Read Sockets[i].WorldPosition for each socket
		//
		// Once we can read those positions we can compare them across rails at the
		// same junction. Rails that have WorldPosition identical to another rail's
		// socket at that junction are confirmed as sharing a socket (bad).
		// The proposedSocketIdx values computed above give the repair target.
		// ------------------------------------------------------------------
	}

	// ---------------------------------------------------------------------------
	// OnActorBeginPlay — AActor::BeginPlay diagnostic callback
	// ---------------------------------------------------------------------------
	// Logs every actor that begins play at TRACE level.  Useful for building
	// a picture of what the engine is spawning and in what order.
	//
	// K2_GetActorLocation is a ProcessEvent call that can fault on garbage
	// pointers, so it is isolated in a SEH trampoline.  GetName() constructs
	// std::string (C++ unwinding), so the main body uses C++ try/catch.
	// MSVC forbids mixing both forms in one function — hence the split.
	// ---------------------------------------------------------------------------

	static bool TryGetActorLocation(SDK::AActor* actor, SDK::FVector* outLoc)
	{
		__try
		{
			*outLoc = actor->K2_GetActorLocation();
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void OnActorBeginPlay(void* rawActor)
	{
		if (!rawActor) return;

		auto actor = static_cast<SDK::AActor*>(rawActor);

		try
		{
			// Actor name (short) — e.g. "BP_DroneRailT1_C_42"
			std::string actorName = actor->GetName();

			// Class name — e.g. "BP_DroneRailT1_C"
			std::string className;
			if (actor->Class)
				className = actor->Class->GetName();
			else
				className = "(null class)";

			// Actor location (via SEH trampoline)
			SDK::FVector loc = {};
			bool haveLoc = TryGetActorLocation(actor, &loc);

			/*
				if (haveLoc)
				{
					LOG_TRACE("BeginPlay: %p [%s] '%s'  pos=(%.0f, %.0f, %.0f)",
						rawActor,
						className.c_str(),
						actorName.c_str(),
						loc.X, loc.Y, loc.Z);
				}
				else
				{
					LOG_TRACE("BeginPlay: %p [%s] '%s'",
						rawActor,
						className.c_str(),
						actorName.c_str());
				}
				*/
		}
		catch (...)
		{
			// If GetName explodes, just log the pointer
			LOG_TRACE("BeginPlay: %p (exception reading actor info)", rawActor);
		}
	}
}
