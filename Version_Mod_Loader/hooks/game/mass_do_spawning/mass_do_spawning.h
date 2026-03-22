#pragma once

#include "../../hooks_common.h"

namespace Hooks::MassDoSpawning
{
	// Before: return true to cancel the batch spawn entirely.
	//         After callbacks will NOT fire if cancelled.
	// Spawner is AMassSpawner* passed as void*.
	using BeforeCallback = bool(*)(void* spawner);

	// After: fires after DoSpawning completes (only if not cancelled).
	// Spawner is AMassSpawner* passed as void*.
	using AfterCallback = void(*)(void* spawner);

	bool Install();
	void Remove();
	bool IsInstalled();

	void RegisterBeforeCallback(BeforeCallback callback);
	void UnregisterBeforeCallback(BeforeCallback callback);
	void RegisterAfterCallback(AfterCallback callback);
	void UnregisterAfterCallback(AfterCallback callback);
}
