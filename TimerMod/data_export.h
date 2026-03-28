#pragma once

#include "timer_tracker.h"

namespace DataExport
{
	// Write timer state to the configured JSON file.
	// Safe to call every tick — throttled internally by UpdateIntervalSeconds config.
	void Update(float deltaSeconds, const RuptureTimer::TimerState& state);

	// Ensure output directory exists. Call once on world begin play.
	void EnsureOutputDir();
}
