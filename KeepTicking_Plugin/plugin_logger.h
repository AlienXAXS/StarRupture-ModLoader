#pragma once

#include "../Version_Mod_Loader/plugin_interface.h"

namespace PluginLogger
{
    // Initialize the plugin logger with the interface provided by mod loader
    void Initialize(IPluginLogger* logger);

    // Logger wrapper functions
  void Trace(const char* format, ...);
void Debug(const char* format, ...);
    void Info(const char* format, ...);
    void Warn(const char* format, ...);
    void Error(const char* format, ...);
}
