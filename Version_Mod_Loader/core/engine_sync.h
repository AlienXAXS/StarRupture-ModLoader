#pragma once
#include <windows.h>

VOID CALLBACK MainInitApcProc(ULONG_PTR param);
void OnEngineInitForUELog();
void WaitForEngineReady();
