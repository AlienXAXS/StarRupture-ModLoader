#pragma once

#include <windows.h>
#include "plugin_interface.h"

namespace ModLoader
{
    // Initialize the logger
    void InitializeLogger();

    // Shutdown the logger
    void ShutdownLogger();

    // Get the global plugin logger instance
    IPluginLogger* GetPluginLogger();

    // Log functions for the mod loader itself - with log levels
    void LogTrace(const wchar_t* format, ...);
    void LogDebug(const wchar_t* format, ...);
    void LogInfo(const wchar_t* format, ...);
    void LogWarn(const wchar_t* format, ...);
    void LogError(const wchar_t* format, ...);

    // Legacy compatibility function (maps to Info level)
    void LogMessage(const wchar_t* format, ...);
}
