#pragma once
#include <windows.h>

// Shared handles and events created in DllMain, used across core modules.
extern HANDLE g_mainInitThread;
extern HANDLE g_autoUpdateThread;
extern HANDLE g_pluginsLoadedEvent;
extern HANDLE g_engineReadyEvent;
extern HANDLE g_ue4ssReadyEvent;
extern HANDLE g_pluginsReadyEvent;
