#include "globals.h"

HANDLE g_mainInitThread   = NULL;
HANDLE g_autoUpdateThread = NULL;
HANDLE g_pluginsLoadedEvent = NULL;
HANDLE g_engineReadyEvent = NULL;
HANDLE g_ue4ssReadyEvent  = NULL;
HANDLE g_pluginsReadyEvent = NULL;
bool   g_mainThreadParked  = false;
HANDLE g_stage1DoneEvent   = NULL;
