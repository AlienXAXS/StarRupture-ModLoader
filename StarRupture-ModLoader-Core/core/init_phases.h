#pragma once
#include <windows.h>

DWORD WINAPI AutoUpdateThreadProc(LPVOID param);
void InitSubsystems();
void InstallHooksPhase();

// Stage 1, after the loader's own hooks are in and before the game main thread
// is released. Loads and runs the DLLs in ModLoader\Preload -- see
// preload/preload_manager.h. Cannot fail the boot: a preload plugin that breaks
// is unloaded and the game starts without it.
void PreloadPhase();
// True when the wait for the engine's init signal ran out (WaitForEngineReady).
bool WaitForEnginePhase();
void LoadPluginsPhase();
void InitPluginsPhase();
