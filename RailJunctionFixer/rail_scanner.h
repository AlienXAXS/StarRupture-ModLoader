#pragma once

namespace SDK { class UWorld; class AActor; }

namespace RailJunctionFixer {
	namespace RailScanner {

		// Scans all rail spline actors (ACrDronePathPointConnection) in the world and
		// logs diagnostic information about their socket state:
		//
		//   - Which rails are connected / disconnected (ConnectedStartActor / EndActor)
		//   - For each junction, how many rails attach to it
		//   - Whether any two rails at the same junction share the same endpoint position
		//     (a geometry-level hint that something is wrong with socket assignment)
		//   - A proposed socket index order for each junction, derived by projecting
		//     each rail's endpoint onto the junction's lateral (right) axis and sorting
		//     left-to-right. This can be used as the ground-truth assignment when
		//     repairing corrupted Mass entity socket data.
		//
		// NOTE on what this detects:
		//   The core "socket sharing" bug lives in Mass entity data
		//   (FCrLogisticsSocketsFragment / FCrCustomConnectionData), not in the actor
		//   positions themselves. After a bad save load, the rail spline actors still sit
		//   at geometrically correct positions - their endpoints are distinct - but the
		//   Mass entity fragments all point to the same socket slot on the junction.
		//   Reading that fragment data requires navigating FMassArchetypeData chunks
		//   which is a planned follow-up. What this scan DOES provide today:
		//     1. A full inventory of rail <-> junction relationships derived from actors.
		//     2. A position-based proposed socket order (the repair target).
		//     3. A geometry sanity check that flags any two rails whose endpoints are
		//        suspiciously close (< SAME_SOCKET_DISTANCE_THRESHOLD), which would
		//        indicate a more severe actor-level placement problem.
		//
		// Call after SignalSocketEntities() from OnSaveLoaded() (or OnExperienceLoadComplete).

		void ScanRailSocketState(SDK::UWorld* world);

		// ---------------------------------------------------------------------------
		// AActor::BeginPlay diagnostic callback
		//
		// Called by the mod loader's AActor::BeginPlay hook for every actor that
		// begins play.  Logs the actor's name, class, and address at TRACE level
		// so the output is available for diagnostics without flooding the log at
		// normal verbosity.
		//
		// Register via IPluginHooks::RegisterActorBeginPlayCallback after engine init.
		// ---------------------------------------------------------------------------
		void OnActorBeginPlay(void* actor);

	} // namespace RailScanner
} // namespace RailJunctionFixer
