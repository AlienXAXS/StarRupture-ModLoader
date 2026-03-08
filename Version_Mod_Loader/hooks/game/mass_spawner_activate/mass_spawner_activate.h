#pragma once

#include "../../hooks_common.h"

namespace Hooks::MassSpawnerActivate
{
    // Before: return true to cancel the activation entirely.
    //         After callbacks will NOT fire if cancelled.
    // Spawner is AAbstractMassEnemySpawner* passed as void*.
    typedef bool (*BeforeCallback)(void* spawner, bool bDisableAggroLock);

    // After: fires after ActivateSpawner completes (only if not cancelled).
    // Spawner is AAbstractMassEnemySpawner* passed as void*.
    typedef void (*AfterCallback)(void* spawner, bool bDisableAggroLock);

    bool Install();
    void Remove();
    bool IsInstalled();

    void RegisterBeforeCallback(BeforeCallback callback);
    void UnregisterBeforeCallback(BeforeCallback callback);
    void RegisterAfterCallback(AfterCallback callback);
    void UnregisterAfterCallback(AfterCallback callback);
}
