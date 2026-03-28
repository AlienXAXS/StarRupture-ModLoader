#pragma once

#include "timer_tracker.h"

struct IPluginScanner;
struct IPluginHooks;

namespace HudOverlay
{
	// Hook AHUD::PostRender and begin rendering the timer overlay.
	// Returns false if the pattern was not found or the hook failed.
	bool Install(IPluginScanner* scanner, IPluginHooks* hooks);

	// Remove the hook. Safe to call if Install was never called or failed.
	void Remove(IPluginHooks* hooks);

	// Push a fresh timer state for the overlay to display.
	// Call from the engine tick callback after ReadCurrentState().
	void SetState(const RuptureTimer::TimerState& state);
}
