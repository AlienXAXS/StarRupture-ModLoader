#pragma once

#include "plugin_interface.h"

// Forward declarations to access global plugin interfaces
IPluginLogger* GetLogger();
IPluginConfig* GetConfig();
IPluginScanner* GetScanner();
IPluginHooks* GetHooks();

// Convenience macros for logging
#define LOG_TRACE(format, ...) if (auto logger = GetLogger()) logger->Trace("RuptureTimer", format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto logger = GetLogger()) logger->Debug("RuptureTimer", format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  if (auto logger = GetLogger()) logger->Info("RuptureTimer", format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  if (auto logger = GetLogger()) logger->Warn("RuptureTimer", format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto logger = GetLogger()) logger->Error("RuptureTimer", format, ##__VA_ARGS__)

// Log once per process lifetime (uses a local static bool)
#define LOG_INFO_ONCE(format, ...) do { static bool _logged = false; if (!_logged) { _logged = true; LOG_INFO(format, ##__VA_ARGS__); } } while(0)
#define LOG_WARN_ONCE(format, ...) do { static bool _logged = false; if (!_logged) { _logged = true; LOG_WARN(format, ##__VA_ARGS__); } } while(0)
