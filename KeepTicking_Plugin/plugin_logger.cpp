#include "plugin_logger.h"
#include <cstdarg>
#include <stdio.h>

namespace PluginLogger
{
	static IPluginLogger* g_logger = nullptr;
	static const char* PLUGIN_NAME = "KeepTicking";

	void Initialize(IPluginLogger* logger)
	{
		g_logger = logger;
	}

	void Trace(const char* format, ...)
	{
		if (!g_logger || !g_logger->Trace) return;

		va_list args;
		va_start(args, format);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		g_logger->Trace(PLUGIN_NAME, "%s", buffer);
	}

	void Debug(const char* format, ...)
	{
		if (!g_logger || !g_logger->Debug) return;

		va_list args;
		va_start(args, format);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		g_logger->Debug(PLUGIN_NAME, "%s", buffer);
	}

	void Info(const char* format, ...)
	{
		if (!g_logger || !g_logger->Info) return;

		va_list args;
		va_start(args, format);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		g_logger->Info(PLUGIN_NAME, "%s", buffer);
	}

	void Warn(const char* format, ...)
	{
		if (!g_logger || !g_logger->Warn) return;

		va_list args;
		va_start(args, format);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		g_logger->Warn(PLUGIN_NAME, "%s", buffer);
	}

	void Error(const char* format, ...)
	{
		if (!g_logger || !g_logger->Error) return;

		va_list args;
		va_start(args, format);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		g_logger->Error(PLUGIN_NAME, "%s", buffer);
	}
}
