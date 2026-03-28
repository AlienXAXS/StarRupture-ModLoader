#pragma once

#include <cstdint>

namespace RuptureTimer
{
	// Maps EEnviroWaveStage to user-friendly names
	// None=Stable, PreWave=Warning, Moving=Burning, Fadeout=Cooling, Growback=Stabilizing
	enum class RupturePhase : uint8_t
	{
		Unknown      = 0,
		Stable       = 1,   // Waiting for next rupture (EEnviroWaveStage::None)
		Warning      = 2,   // 15-second pre-wave warning (EEnviroWaveStage::PreWave)
		Burning      = 3,   // Wave moving across map - DANGEROUS (EEnviroWaveStage::Moving)
		Cooling      = 4,   // Aftermath / fadeout (EEnviroWaveStage::Fadeout)
		Stabilizing  = 5,   // Safe gathering window (EEnviroWaveStage::Growback)
	};

	struct TimerState
	{
		bool         valid;                  // false if world/game state not ready
		RupturePhase phase;
		const char*  phaseName;             // "Stable", "Warning", "Burning", "Cooling", "Stabilizing"
		float        phaseRemainingSeconds; // time left in the current phase (-1.0f = unknown)
		float        nextRuptureInSeconds;  // seconds until next Burning phase starts (the primary display value)
		                                    // 0 = currently in rupture; -1 = unknown
		int32_t      waveNumber;            // how many ruptures have completed (NextPhase)
		bool         paused;                // timer paused on server
		uint8_t      waveType;             // 0=None, 1=Heat, 2=Cold
		const char*  waveTypeName;         // "None", "Heat", "Cold"

		// Per-phase timing breakdown.
		// -1.0f = not available (dedicated server client) or not yet scheduled (future phases).
		// 0.0f  = this phase has already passed in the current cycle.
		// Populated in full mode (local / listen-server) only.
		float        warningRemaining;      // time left in Warning phase
		float        burningRemaining;      // time left in Burning phase
		float        coolingRemaining;      // time left in Cooling phase
		float        stabilizingRemaining;  // time left in Stabilizing phase
		float        stableRemaining;       // duration of the next Stable period
	};

	// Read current rupture timer state from the game. Call only from game thread.
	TimerState ReadCurrentState();
}
