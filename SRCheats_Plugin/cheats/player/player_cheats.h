#pragma once

#include "plugin_interface.h"

// Player-category cheats: god mode, unlimited ammo, flight, item giving, teleport.
// All state is static.  Init/Shutdown are called from PluginInit / PluginShutdown.
class PlayerCheats
{
public:
    static void Init(IPluginHooks* hooks);
    static void Shutdown(IPluginHooks* hooks);

    // Continuous toggle state — read and written by CheatsPanel
    static bool  s_godModeEnabled;
    static bool  s_unlimitedAmmoEnabled;
    static bool  s_flightEnabled;

    // Flight speed written by the panel slider (units/s, same as UE MaxFlySpeed).
    // Default matches a fast but controllable flight speed.
    static float s_flightSpeed;

    // One-shot actions called from CheatsPanel
    static void GiveDefaultWeapons();
    static void GiveItem(const char* name, int amount);
    static void TeleportToCamera();
    static void StopFlight();   // called when flight is toggled off to restore movement

private:
    static void OnTick(float deltaSeconds);
    static void OnWorldBeginPlay(SDK::UWorld* world, const char* worldName);
};
