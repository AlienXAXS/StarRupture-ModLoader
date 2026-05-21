#pragma once
#include <windows.h>

DWORD WINAPI AutoUpdateThreadProc(LPVOID param);
void InitSubsystems();
void InstallHooksPhase();
void WaitForEnginePhase();
void LoadPluginsPhase();
void InitPluginsPhase();
