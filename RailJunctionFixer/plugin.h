#pragma once

#include <windows.h>
#include "plugin_interface.h"

// Plugin exports
extern "C" {
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks);
  __declspec(dllexport) void PluginShutdown();
}
