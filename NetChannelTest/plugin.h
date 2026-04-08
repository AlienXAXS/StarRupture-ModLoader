#pragma once
#include "plugin_interface.h"

extern "C" {
	__declspec(dllexport) PluginInfo* GetPluginInfo();
	__declspec(dllexport) bool PluginInit(IPluginSelf* self);
	__declspec(dllexport) void PluginShutdown();
}
