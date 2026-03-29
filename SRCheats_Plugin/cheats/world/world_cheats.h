#pragma once

#include "plugin_interface.h"

// World-category cheats: enemy control, item destruction, feature unlocks, survival attributes.
// All state is static.  Init/Shutdown are called from PluginInit / PluginShutdown.
class WorldCheats
{
public:
    static void Init(IPluginHooks* hooks);
    static void Shutdown(IPluginHooks* hooks);

    // Toggle state — read and written by CheatsPanel
    static bool s_noEnemySpawns;
    static bool s_enemiesIgnoreMe;

    // One-shot actions called from CheatsPanel
    static void KillAllEnemies();
    static void DestroyAllItems();
    static void UnlockAllFeatures();
    static void SetSurvivalAttr(const char* name, int value);

    // Called from CheatsPanel when the Enemies Ignore Me toggle changes
    static void SetEnemiesIgnoreMe(bool enable);

private:
    static bool OnBeforeActivateSpawner(void* spawner, bool bDisableAggroLock);
    static void OnWorldBeginPlay(SDK::UWorld* world, const char* worldName);
    static void OnTick(float deltaSeconds);
};
