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
void WaitForEnginePhase();
void LoadPluginsPhase();
void InitPluginsPhase();
