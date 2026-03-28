#include "timer_tracker.h"
#include "plugin_helpers.h"

#include <cmath>
#include <cstdlib>
#include "Basic.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"

namespace RuptureTimer
{

// ---------------------------------------------------------------------------
// Find UCrEnviroWaveSubsystem by iterating GObjects for the given world.
// World subsystems are UObjects whose Outer is the UWorld instance.
// ---------------------------------------------------------------------------
static SDK::UCrEnviroWaveSubsystem* FindEnviroWaveSubsystem(SDK::UWorld* world)
{
	SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (!arr) return nullptr;

	for (int i = 0; i < arr->Num(); i++)
	{
		SDK::UObject* obj = arr->GetByIndex(i);
		if (!obj || !obj->Class || !obj->Outer) continue;
		if (obj->Outer != static_cast<SDK::UObject*>(world)) continue;
		if (obj->Class->GetName() == "CrEnviroWaveSubsystem")
			return static_cast<SDK::UCrEnviroWaveSubsystem*>(obj);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Find ACrGatherableSpawnersRepActor — a replicated actor present on all
// clients (including dedicated server clients). It carries two Net/RepNotify
// fields that store the current wave type and stage, making it usable as a
// fallback phase source when UCrEnviroWaveSubsystem is absent.
// ---------------------------------------------------------------------------
static SDK::ACrGatherableSpawnersRepActor* FindGatherableSpawnersRepActor(SDK::UWorld* world)
{
	SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (!arr) return nullptr;

	// ACrGatherableSpawnersRepActor is an AActor — Outer chain is:
	//   actor → ULevel → UWorld
	// So we must check two levels up, not one.
	for (int i = 0; i < arr->Num(); i++)
	{
		SDK::UObject* obj = arr->GetByIndex(i);
		if (!obj || !obj->Class) continue;
		if (!obj->Outer || obj->Outer->Outer != static_cast<SDK::UObject*>(world)) continue;
		if (obj->Class->GetName() == "CrGatherableSpawnersRepActor")
			return static_cast<SDK::ACrGatherableSpawnersRepActor*>(obj);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Client-side phase state machine — dedicated server fallback.
//
// On a dedicated server client, UCrEnviroWaveSubsystem and
// ACrGatherableSpawnersRepActor are both absent. The only observable signals
// are NextTime (absolute server timestamp) and NextPhase (wave counter).
//
// Known fixed phase durations (empirically validated, full cycle = 54 min):
//   Burning:     30 s
//   Cooling:     60 s
//   Stabilizing: 600 s (10 min)
//   Stable:      2550 s (42 min 30 s)  ← derived: 3240 - 30 - 60 - 600
//
// Detectable transitions:
//   Stable → Burning:       nextTimeRemaining jumps from ~0 to ~BURNING_DURATION
//   Burning → Cooling:      nextTimeRemaining jumps from ~0 to a large value (full cycle ahead)
//   Cooling → Stabilizing:  elapsed time since Cooling start >= COOLING_DURATION
//   Stabilizing → Stable:   NextPhase increments (server broadcasts new wave number)
// ---------------------------------------------------------------------------
static constexpr float BURNING_DURATION     = 30.0f;
static constexpr float COOLING_DURATION     = 60.0f;
static constexpr float STABILIZING_DURATION = 600.0f;

// Thresholds for jump detection
static constexpr float NEAR_ZERO_THRESHOLD  = 10.0f;   // "near zero" = prev was <= this
static constexpr float BURN_JUMP_MAX        = 60.0f;   // small jump: new value <= this  → Burning
static constexpr float COOLING_JUMP_MIN     = 120.0f;  // large jump: new value >= this → Cooling

struct ClientPhaseTracker
{
	RupturePhase phase           = RupturePhase::Unknown;
	float phaseStartServerTime   = -1.0f;  // server time when current phase began
	float prevNextTimeRemaining  = -1.0f;
	int32_t prevNextPhase        = -1;
	SDK::UWorld* lastWorld       = nullptr; // reset tracker on world change
	bool initialized             = false;
};
static ClientPhaseTracker s_tracker;

// Update the state machine; call once per tick in the fallback path.
// serverTime    — result of gameState->GetServerWorldTimeSeconds()
// nextTimeRemaining — NextTime - serverTime, clamped to >= 0
// nextPhase     — timerActor->NextPhase
// world         — current UWorld (used for world-change detection)
static void UpdateClientPhaseStateMachine(float serverTime, float nextTimeRemaining,
                                          int32_t nextPhase, SDK::UWorld* world)
{
	// Reset on world change
	if (world != s_tracker.lastWorld)
	{
		s_tracker = ClientPhaseTracker{};
		s_tracker.lastWorld = world;
	}

	if (!s_tracker.initialized)
	{
		s_tracker.initialized           = true;
		s_tracker.prevNextTimeRemaining = nextTimeRemaining;
		s_tracker.prevNextPhase         = nextPhase;

		// Initial-connection heuristic: if nextTimeRemaining is in the
		// Burning range, we are probably mid-Burning; otherwise assume Stable.
		// Phase label may be wrong for one cycle, but next_rupture_in_sec is
		// always accurate because NextTime already encodes the correct time.
		if (nextTimeRemaining >= 0.0f && nextTimeRemaining <= BURN_JUMP_MAX)
		{
			s_tracker.phase              = RupturePhase::Burning;
			// Back-estimate phase start from remaining burn time
			s_tracker.phaseStartServerTime = serverTime - (BURNING_DURATION - nextTimeRemaining);
		}
		else
		{
			s_tracker.phase              = RupturePhase::Stable;
			s_tracker.phaseStartServerTime = serverTime;
		}
		return;
	}

	float prev = s_tracker.prevNextTimeRemaining;

	// Transition detection — evaluated in priority order.

	// 1. NextPhase incremented → new wave number means we just entered Stable.
	if (nextPhase > s_tracker.prevNextPhase && s_tracker.prevNextPhase >= 0)
	{
		s_tracker.phase              = RupturePhase::Stable;
		s_tracker.phaseStartServerTime = serverTime;
		LOG_DEBUG("ClientPhase→Stable (NextPhase %d→%d)", s_tracker.prevNextPhase, nextPhase);
	}
	// 2. nextTimeRemaining jumped from near-zero to small-positive → Burning started.
	else if (prev >= 0.0f && prev <= NEAR_ZERO_THRESHOLD
	         && nextTimeRemaining > prev + 5.0f
	         && nextTimeRemaining <= BURN_JUMP_MAX)
	{
		s_tracker.phase              = RupturePhase::Burning;
		s_tracker.phaseStartServerTime = serverTime;
		LOG_DEBUG("ClientPhase→Burning (jump %.1f→%.1f)", prev, nextTimeRemaining);
	}
	// 3. nextTimeRemaining jumped from near-zero to large → Cooling started.
	else if (prev >= 0.0f && prev <= NEAR_ZERO_THRESHOLD
	         && nextTimeRemaining >= COOLING_JUMP_MIN)
	{
		s_tracker.phase              = RupturePhase::Cooling;
		s_tracker.phaseStartServerTime = serverTime;
		LOG_DEBUG("ClientPhase→Cooling (jump %.1f→%.1f)", prev, nextTimeRemaining);
	}
	// 4. Cooling has elapsed its fixed duration → transition to Stabilizing.
	else if (s_tracker.phase == RupturePhase::Cooling
	         && (serverTime - s_tracker.phaseStartServerTime) >= COOLING_DURATION)
	{
		// Set phase start to the exact moment cooling ended (not current tick)
		s_tracker.phaseStartServerTime += COOLING_DURATION;
		s_tracker.phase = RupturePhase::Stabilizing;
		LOG_DEBUG("ClientPhase→Stabilizing (cooling elapsed)");
	}

	s_tracker.prevNextTimeRemaining = nextTimeRemaining;
	s_tracker.prevNextPhase         = nextPhase;
}

// ---------------------------------------------------------------------------
// Compute the total duration of each stage from the game's own settings.
// These are the values the server/client agreed on — not hardcoded constants.
//
// FCrEnviroWaveSettings fields used per stage:
//   PreWave:     PreWaveDuration + WavePreWaveExplosionDuration
//   Moving:      |WaveEndPosition - WaveStartPosition| / WaveSpeed
//   Fadeout:     WaveFadeoutFireWaveDuration + WaveFadeoutBurningDuration + WaveFadeoutFadingDuration
//   Growback:    WaveGrowbackMoonPhaseDuration + WaveGrowbackRegrowthStartDuration + WaveGrowbackRegrowthDuration
// ---------------------------------------------------------------------------
static float StageDurationFromSettings(SDK::EEnviroWaveStage stage, const SDK::FCrEnviroWaveSettings& s)
{
	switch (stage)
	{
		case SDK::EEnviroWaveStage::PreWave:
			return s.PreWaveDuration + s.WavePreWaveExplosionDuration;

		case SDK::EEnviroWaveStage::Moving:
			if (s.WaveSpeed > 0.0f)
				return fabsf(s.WaveEndPosition - s.WaveStartPosition) / s.WaveSpeed;
			return 0.0f;

		case SDK::EEnviroWaveStage::Fadeout:
			return s.WaveFadeoutFireWaveDuration + s.WaveFadeoutBurningDuration + s.WaveFadeoutFadingDuration;

		case SDK::EEnviroWaveStage::Growback:
			return s.WaveGrowbackMoonPhaseDuration + s.WaveGrowbackRegrowthStartDuration + s.WaveGrowbackRegrowthDuration;

		default:
			return 0.0f;
	}
}

// ---------------------------------------------------------------------------
// Read current rupture timer state from game objects.
// Must be called from the game thread (engine tick callback).
// ---------------------------------------------------------------------------
TimerState ReadCurrentState()
{
	TimerState state{};
	state.valid    = false;
	state.phase    = RupturePhase::Unknown;
	state.phaseName = "Unknown";
	state.phaseRemainingSeconds = -1.0f;
	state.nextRuptureInSeconds  = -1.0f;
	state.waveNumber = 0;
	state.paused     = false;
	state.waveType   = 0;
	state.waveTypeName = "None";
	state.warningRemaining     = -1.0f;
	state.burningRemaining     = -1.0f;
	state.coolingRemaining     = -1.0f;
	state.stabilizingRemaining = -1.0f;
	state.stableRemaining      = -1.0f;

	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world) { LOG_WARN_ONCE("ReadCurrentState: UWorld is null"); return state; }

	// Get game state for the replicated WaveTimerActor
	auto* gameState = static_cast<SDK::ACrGameStateBase*>(world->GameState);
	if (!gameState)         { LOG_WARN_ONCE("ReadCurrentState: GameState is null"); return state; }
	if (!gameState->WaveTimerActor) { LOG_WARN_ONCE("ReadCurrentState: WaveTimerActor is null (not yet replicated?)"); return state; }

	SDK::ACrWaveTimerActor* timerActor = gameState->WaveTimerActor;

	state.valid      = true;
	state.waveNumber = timerActor->NextPhase;
	state.paused     = timerActor->bPause;

	// NextTime is an absolute server world timestamp (seconds since level start),
	// not a countdown. Subtract current server time to get remaining seconds.
	double serverTime = gameState->GetServerWorldTimeSeconds();
	float nextTimeRemaining = timerActor->NextTime - (float)serverTime;
	if (nextTimeRemaining < 0.0f) nextTimeRemaining = 0.0f;

	// Try to find the wave subsystem. It is server-authoritative and will be absent
	// on clients connected to a dedicated server (ShouldCreateSubsystem returns false
	// for client worlds). Fall back to timer-actor-only mode when unavailable.
	SDK::UCrEnviroWaveSubsystem* waveSub = FindEnviroWaveSubsystem(world);
	if (!waveSub)
	{
		LOG_DEBUG("ReadCurrentState: UCrEnviroWaveSubsystem absent — using replication-actor mode");

		// Try ACrGatherableSpawnersRepActor — a replicated actor present on all clients.
		// It holds RepEnviroWaveTypeChange and RepEnviroWaveStageChange as persistent
		// Net/RepNotify fields, giving us accurate phase and wave-type labels.
		// Per-phase breakdown timers (-1) are not available without the subsystem.
		SDK::ACrGatherableSpawnersRepActor* repActor = FindGatherableSpawnersRepActor(world);
		if (repActor)
		{
			SDK::EEnviroWaveStage repStage = repActor->RepEnviroWaveStageChange;
			SDK::EEnviroWave      repWave  = repActor->RepEnviroWaveTypeChange;

			state.waveType = static_cast<uint8_t>(repWave);
			switch (repWave)
			{
				case SDK::EEnviroWave::Heat: state.waveTypeName = "Heat"; break;
				case SDK::EEnviroWave::Cold: state.waveTypeName = "Cold"; break;
				default:                     state.waveTypeName = "None"; break;
			}

			switch (repStage)
			{
				case SDK::EEnviroWaveStage::None:
					// Stable — NextTime is the absolute time the next wave fires.
					state.phase                 = RupturePhase::Stable;
					state.phaseName             = "Stable";
					state.phaseRemainingSeconds = nextTimeRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				case SDK::EEnviroWaveStage::PreWave:
					// Warning — NextTime appears to point to when burning starts.
					state.phase                 = RupturePhase::Warning;
					state.phaseName             = "Warning";
					state.phaseRemainingSeconds = nextTimeRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					break;

				case SDK::EEnviroWaveStage::Moving:
					// Burning — wave is active now.
					state.phase                 = RupturePhase::Burning;
					state.phaseName             = "Burning";
					state.phaseRemainingSeconds = -1.0f; // no progress without subsystem
					state.nextRuptureInSeconds  = 0.0f;
					break;

				case SDK::EEnviroWaveStage::Fadeout:
					// Cooling — NextTime is the stable-period countdown (undercount of
					// total next_rupture since we can't add remaining cooling time).
					state.phase                 = RupturePhase::Cooling;
					state.phaseName             = "Cooling";
					state.phaseRemainingSeconds = -1.0f;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				case SDK::EEnviroWaveStage::Growback:
					// Stabilizing — same situation as Cooling.
					state.phase                 = RupturePhase::Stabilizing;
					state.phaseName             = "Stabilizing";
					state.phaseRemainingSeconds = -1.0f;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				default:
					state.phase     = RupturePhase::Unknown;
					state.phaseName = "Unknown";
					break;
			}
		}
		else
		{
			// Last-resort fallback: no subsystem and no rep actor found.
			// Use the client-side state machine to infer phase from observable
			// jumps in nextTimeRemaining and changes in NextPhase.
			LOG_DEBUG("ReadCurrentState: using client-side phase state machine");

			UpdateClientPhaseStateMachine(
				(float)serverTime, nextTimeRemaining,
				timerActor->NextPhase, world);

			float elapsed = serverTime - s_tracker.phaseStartServerTime;

			switch (s_tracker.phase)
			{
				case RupturePhase::Stable:
					state.phase                 = RupturePhase::Stable;
					state.phaseName             = "Stable";
					state.phaseRemainingSeconds = nextTimeRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stableRemaining       = nextTimeRemaining;
					break;

				case RupturePhase::Warning:
					state.phase                 = RupturePhase::Warning;
					state.phaseName             = "Warning";
					state.phaseRemainingSeconds = nextTimeRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					break;

				case RupturePhase::Burning:
				{
					// During Burning, NextTime = end of Burning, so nextTimeRemaining = burning remaining.
					float burningRemaining = nextTimeRemaining;
					if (burningRemaining < 0.0f) burningRemaining = 0.0f;
					state.phase                 = RupturePhase::Burning;
					state.phaseName             = "Burning";
					state.phaseRemainingSeconds = burningRemaining;
					state.nextRuptureInSeconds  = 0.0f;
					state.burningRemaining      = burningRemaining;
					break;
				}

				case RupturePhase::Cooling:
				{
					// nextTimeRemaining = full remaining time until next rupture (from NextTime).
					// phase_remaining = time left in Cooling = COOLING_DURATION - elapsed.
					float coolingRemaining = COOLING_DURATION - elapsed;
					if (coolingRemaining < 0.0f) coolingRemaining = 0.0f;
					// stableRemaining can be estimated: total - cooling_remaining - stabilizing
					float stableEst = nextTimeRemaining - coolingRemaining - STABILIZING_DURATION;
					state.phase                 = RupturePhase::Cooling;
					state.phaseName             = "Cooling";
					state.phaseRemainingSeconds = coolingRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.coolingRemaining      = coolingRemaining;
					state.stabilizingRemaining  = STABILIZING_DURATION;
					state.stableRemaining       = (stableEst >= 0.0f) ? stableEst : -1.0f;
					break;
				}

				case RupturePhase::Stabilizing:
				{
					// phaseStartServerTime was set to cooling_start + COOLING_DURATION.
					float stabilizingRemaining = STABILIZING_DURATION - elapsed;
					if (stabilizingRemaining < 0.0f) stabilizingRemaining = 0.0f;
					float stableEst = nextTimeRemaining - stabilizingRemaining;
					state.phase                 = RupturePhase::Stabilizing;
					state.phaseName             = "Stabilizing";
					state.phaseRemainingSeconds = stabilizingRemaining;
					state.nextRuptureInSeconds  = nextTimeRemaining;
					state.stabilizingRemaining  = stabilizingRemaining;
					state.stableRemaining       = (stableEst >= 0.0f) ? stableEst : -1.0f;
					break;
				}

				default: // Unknown — not enough data yet
					state.phase     = RupturePhase::Unknown;
					state.phaseName = "Unknown";
					break;
			}
		}
		return state;
	}

	// --- Full mode: subsystem is available (local / listen server) ---

	// Wave type (Heat / Cold)
	SDK::EEnviroWave waveType = waveSub->GetCurrentType();
	state.waveType = static_cast<uint8_t>(waveType);
	switch (waveType)
	{
		case SDK::EEnviroWave::Heat: state.waveTypeName = "Heat"; break;
		case SDK::EEnviroWave::Cold: state.waveTypeName = "Cold"; break;
		default:                     state.waveTypeName = "None"; break;
	}

	SDK::EEnviroWaveStage stage = waveSub->GetCurrentStage();

	if (stage == SDK::EEnviroWaveStage::None)
	{
		// Stable period — NextTime is the absolute server time when the next wave fires
		state.phase     = RupturePhase::Stable;
		state.phaseName = "Stable";
		state.phaseRemainingSeconds = nextTimeRemaining;
		state.nextRuptureInSeconds  = nextTimeRemaining;
		state.stableRemaining       = nextTimeRemaining;
		// Wave-type and per-wave-phase durations are not meaningful until the next wave starts.
		return state;
	}

	// For all active wave stages, fetch the actual durations from the game client
	SDK::FCrEnviroWaveSettings settings = waveSub->GetCurrentStageSettings();

	float progress = waveSub->GetCurrentStageProgress();
	if (progress < 0.0f) progress = 0.0f;
	if (progress > 1.0f) progress = 1.0f;

	float stageDuration = StageDurationFromSettings(stage, settings);
	float phaseRemaining = stageDuration * (1.0f - progress);
	if (phaseRemaining < 0.0f) phaseRemaining = 0.0f;

	// During Cooling/Stabilizing, the server has already set the next wave's absolute time.
	// nextTimeRemaining gives the Stable period before the NEXT rupture.
	float stableCountdown = nextTimeRemaining;

	// Precompute full durations of adjacent phases for the breakdown timers.
	float fullBurning     = StageDurationFromSettings(SDK::EEnviroWaveStage::Moving,   settings);
	float fullCooling     = StageDurationFromSettings(SDK::EEnviroWaveStage::Fadeout,  settings);
	float fullStabilizing = StageDurationFromSettings(SDK::EEnviroWaveStage::Growback, settings);

	switch (stage)
	{
		case SDK::EEnviroWaveStage::PreWave:
			state.phase     = RupturePhase::Warning;
			state.phaseName = "Warning";
			state.phaseRemainingSeconds = phaseRemaining;
			// Warning → Burning is immediate after this phase
			state.nextRuptureInSeconds  = phaseRemaining;
			// Breakdown: warning is live; downstream phases have not started yet
			state.warningRemaining      = phaseRemaining;
			state.burningRemaining      = fullBurning;
			state.coolingRemaining      = fullCooling;
			state.stabilizingRemaining  = fullStabilizing;
			// stableRemaining unknown — next stable period not yet scheduled
			break;

		case SDK::EEnviroWaveStage::Moving:
			state.phase     = RupturePhase::Burning;
			state.phaseName = "Burning";
			state.phaseRemainingSeconds = phaseRemaining;
			// Currently in rupture — next rupture starts after the full next cycle
			state.nextRuptureInSeconds  = 0.0f;
			// Breakdown
			state.warningRemaining      = 0.0f;
			state.burningRemaining      = phaseRemaining;
			state.coolingRemaining      = fullCooling;
			state.stabilizingRemaining  = fullStabilizing;
			// stableRemaining unknown — not yet scheduled during Burning
			break;

		case SDK::EEnviroWaveStage::Fadeout:
		{
			state.phase     = RupturePhase::Cooling;
			state.phaseName = "Cooling";
			state.phaseRemainingSeconds = phaseRemaining;
			// Next rupture = rest of Cooling + full Stabilizing + Stable countdown
			state.nextRuptureInSeconds  = phaseRemaining + fullStabilizing + stableCountdown;
			// Breakdown
			state.warningRemaining      = 0.0f;
			state.burningRemaining      = 0.0f;
			state.coolingRemaining      = phaseRemaining;
			state.stabilizingRemaining  = fullStabilizing;
			state.stableRemaining       = stableCountdown;
			break;
		}

		case SDK::EEnviroWaveStage::Growback:
			state.phase     = RupturePhase::Stabilizing;
			state.phaseName = "Stabilizing";
			state.phaseRemainingSeconds = phaseRemaining;
			// Next rupture = rest of Stabilizing + Stable countdown
			state.nextRuptureInSeconds  = phaseRemaining + stableCountdown;
			// Breakdown
			state.warningRemaining      = 0.0f;
			state.burningRemaining      = 0.0f;
			state.coolingRemaining      = 0.0f;
			state.stabilizingRemaining  = phaseRemaining;
			state.stableRemaining       = stableCountdown;
			break;

		default:
			state.phase     = RupturePhase::Unknown;
			state.phaseName = "Unknown";
			break;
	}

	return state;
}

} // namespace RuptureTimer
