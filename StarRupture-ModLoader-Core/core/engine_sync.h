#pragma once
#include <windows.h>

VOID CALLBACK MainInitApcProc(ULONG_PTR param);
void OnEngineInitForUELog();
void OnEngineInitForGObjectExport();
// True when the wait ran out before the engine's init signal came.
bool WaitForEngineReady();
