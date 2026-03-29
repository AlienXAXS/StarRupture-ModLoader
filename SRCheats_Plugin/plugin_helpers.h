#pragma once

#include "plugin_interface.h"

// Forward declarations to access global plugin interfaces
IPluginLogger* GetLogger();
IPluginConfig* GetConfig();
IPluginScanner* GetScanner();
IPluginHooks* GetHooks();

// Convenience macros for logging
#define LOG_TRACE(format, ...) if (auto logger = GetLogger()) logger->Trace("SRCheats", format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto logger = GetLogger()) logger->Debug("SRCheats", format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) if (auto logger = GetLogger()) logger->Info("SRCheats", format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) if (auto logger = GetLogger()) logger->Warn("SRCheats", format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto logger = GetLogger()) logger->Error("SRCheats", format, ##__VA_ARGS__)
