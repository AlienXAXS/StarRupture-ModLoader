#include "logger.h"
#include "log.h"  // Use the existing comprehensive log system
#include <cstdarg>
#include <stdio.h>

namespace ModLoader
{
	static CRITICAL_SECTION g_logLock;
	static bool g_logInitialized = false;

	// Universal logger implementation for plugins
	static void PluginLog(PluginLogLevel level, const char* pluginName, const char* message)
	{
		if (!g_logInitialized) return;

		// Map plugin log level to Log:: system
		switch (level)
		{
		case PluginLogLevel::Trace:
			Log::Trace("[Plugin:%s] %s", pluginName, message);
			break;
		case PluginLogLevel::Debug:
			Log::Debug("[Plugin:%s] %s", pluginName, message);
			break;
		case PluginLogLevel::Info:
			Log::Info("[Plugin:%s] %s", pluginName, message);
			break;
		case PluginLogLevel::Warn:
			Log::Warn("[Plugin:%s] %s", pluginName, message);
			break;
		case PluginLogLevel::Error:
			Log::Error("[Plugin:%s] %s", pluginName, message);
			break;
		}
	}

	static void PluginLogTrace(const char* pluginName, const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		PluginLog(PluginLogLevel::Trace, pluginName, buffer);
	}

	static void PluginLogDebug(const char* pluginName, const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		PluginLog(PluginLogLevel::Debug, pluginName, buffer);
	}

	static void PluginLogInfo(const char* pluginName, const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		PluginLog(PluginLogLevel::Info, pluginName, buffer);
	}

	static void PluginLogWarn(const char* pluginName, const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		PluginLog(PluginLogLevel::Warn, pluginName, buffer);
	}

	static void PluginLogError(const char* pluginName, const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		PluginLog(PluginLogLevel::Error, pluginName, buffer);
	}

	// Global logger instance
	static IPluginLogger g_pluginLogger = {
		PluginLog,
		PluginLogTrace,
		PluginLogDebug,
		PluginLogInfo,
		PluginLogWarn,
		PluginLogError
	};

	void InitializeLogger()
	{
		InitializeCriticalSection(&g_logLock);
		g_logInitialized = true;
		Log::Info("[ModLoader] Logger initialized (using Log:: backend)");
	}

	void ShutdownLogger()
	{
		Log::Info("[ModLoader] Logger shutting down");
		g_logInitialized = false;
		DeleteCriticalSection(&g_logLock);
	}

	IPluginLogger* GetPluginLogger()
	{
		return &g_pluginLogger;
	}

	// ModLoader's own logging functions - now with log levels!
	void LogTrace(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 1024, format, args);
		va_end(args);

		// Convert wide string to narrow for Log:: system
		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		Log::Trace("[ModLoader] %s", narrowBuffer);
	}

	void LogDebug(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 1024, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		Log::Debug("[ModLoader] %s", narrowBuffer);
	}

	void LogInfo(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 1024, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		Log::Info("[ModLoader] %s", narrowBuffer);
	}

	void LogWarn(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 1024, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		Log::Warn("[ModLoader] %s", narrowBuffer);
	}

	void LogError(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 1024, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		Log::Error("[ModLoader] %s", narrowBuffer);
	}

	// Keep legacy LogMessage for compatibility - maps to Info level
	void LogMessage(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 1024, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		Log::Info("[ModLoader] %s", narrowBuffer);
	}
}
