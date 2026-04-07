#pragma once

#include "plugin_interface.h"

IPluginLogger* GetLogger();
IPluginConfig* GetConfig();
IPluginScanner* GetScanner();
IPluginHooks* GetHooks();

#define LOG_TRACE(format, ...) if (auto logger = GetLogger()) logger->Trace("NetChannelTest", format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto logger = GetLogger()) logger->Debug("NetChannelTest", format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  if (auto logger = GetLogger()) logger->Info("NetChannelTest", format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  if (auto logger = GetLogger()) logger->Warn("NetChannelTest", format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto logger = GetLogger()) logger->Error("NetChannelTest", format, ##__VA_ARGS__)
