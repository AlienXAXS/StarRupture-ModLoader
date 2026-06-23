#pragma once

#include "../../hooks_common.h"

namespace Hooks::MassSpawnerDeactivate
{
	// Before: return true to cancel the deactivation entirely.
	//         After callbacks will NOT fire if cancelled.
	// Spawner is AAbstractMassEnemySpawner* passed as void*.
	using BeforeCallback = bool(*)(void* spawner, bool bPermanently);

	// After: fires after DeactivateSpawner completes (only if not cancelled).
	// Spawner is AAbstractMassEnemySpawner* passed as void*.
	using AfterCallback = void(*)(void* spawner, bool bPermanently);

	bool Install();
	void Remove();
	bool IsInstalled();

	void RegisterBeforeCallback(BeforeCallback callback);
	void UnregisterBeforeCallback(BeforeCallback callback);
	void RegisterAfterCallback(AfterCallback callback);
	void UnregisterAfterCallback(AfterCallback callback);

	// Returns the trampoline address of the original AAbstractMassEnemySpawner::DeactivateSpawner,
	// or 0 if not yet installed.
	// Cast to: void(__fastcall*)(void* thisPtr, bool bPermanently)
	uintptr_t GetOriginalPtr();
}
