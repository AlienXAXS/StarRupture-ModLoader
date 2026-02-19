#pragma once

#include "plugin_interface.h"

// Forward declarations to access global plugin interfaces
IPluginLogger* GetLogger();
IPluginConfig* GetConfig();
IPluginScanner* GetScanner();
IPluginHooks* GetHooks();

// Convenience macros for logging
#define LOG_TRACE(format, ...) if (auto logger = GetLogger()) logger->Trace("KeepTicking", format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto logger = GetLogger()) logger->Debug("KeepTicking", format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) if (auto logger = GetLogger()) logger->Info("KeepTicking", format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) if (auto logger = GetLogger()) logger->Warn("KeepTicking", format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto logger = GetLogger()) logger->Error("KeepTicking", format, ##__VA_ARGS__)
