#pragma once
#include <windows.h>

// Shared handles and events created in DllMain, used across core modules.
extern HANDLE g_mainInitThread;
extern HANDLE g_autoUpdateThread;
extern HANDLE g_pluginsLoadedEvent;
extern HANDLE g_engineReadyEvent;
extern HANDLE g_ue4ssReadyEvent;
extern HANDLE g_pluginsReadyEvent;

// Set when Core_Attach was entered on the game main thread and init was
// deferred via QueueUserAPC. In that case the main thread holds itself inside
// the APC on g_stage1DoneEvent for the duration of Stage 1 instead of being
// SuspendThread()ed by the init thread -- see MainInitApcProc.
extern bool   g_mainThreadParked;
extern HANDLE g_stage1DoneEvent;
