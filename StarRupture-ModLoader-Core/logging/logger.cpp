#include "logger.h"
#include "log.h"  // Use the existing comprehensive log system
#include "plugin_log_levels.h"
#include <cstdarg>
#include <stdio.h>

namespace ModLoaderLogger
{
	static CRITICAL_SECTION g_logLock;
	static bool g_logInitialized = false;

	// Map the plugin-facing level onto the file logger's.
	static LogToFile::Level ToFileLevel(PluginLogLevel level)
	{
		switch (level)
		{
		case PluginLogLevel::Trace: return LogToFile::Level::Trace;
		case PluginLogLevel::Debug: return LogToFile::Level::Debug;
		case PluginLogLevel::Warn:  return LogToFile::Level::Warn;
		case PluginLogLevel::Error: return LogToFile::Level::Error;
		default:                    return LogToFile::Level::Info;
		}
	}

	static const char* ToLevelString(PluginLogLevel level)
	{
		switch (level)
		{
		case PluginLogLevel::Trace: return "TRACE";
		case PluginLogLevel::Debug: return "DEBUG";
		case PluginLogLevel::Warn:  return "WARN";
		case PluginLogLevel::Error: return "ERROR";
		default:                    return "INFO";
		}
	}

	// Universal logger implementation for plugins.
	//
	// This is the one place every plugin log line passes through, so it is where
	// the per-plugin level (plugin_log_levels.h) is applied. The plugin's own
	// gate replaces the global one rather than stacking with it -- hence
	// WriteBypass rather than the ordinary LogToFile::Trace/Debug/... entry
	// points, which would re-test g_minLevel and cap a plugin turned up above it.
	static void PluginLog(PluginLogLevel level, const IPluginSelf* self, const char* message)
	{
		if (!g_logInitialized) return;
		const char* name = (self && self->name) ? self->name : "?";

		const LogToFile::Level fileLevel = ToFileLevel(level);
		if (!PluginLogLevels::ShouldLog(name, fileLevel))
			return;

		LogToFile::WriteBypass(fileLevel, ToLevelString(level), "[Plugin:%s] %s", name, message);
	}

	// Shared body of the five level-specific plugin entry points. Tests the
	// plugin's level BEFORE formatting: a plugin's Trace() calls are typically
	// the ones sitting in a per-tick loop, and turning that plugin down is
	// worth nothing if every suppressed line still costs a vsnprintf.
	static void PluginLogFormatted(PluginLogLevel level, const IPluginSelf* self,
	                                const char* format, va_list args)
	{
		if (!g_logInitialized) return;

		const char* name = (self && self->name) ? self->name : "?";
		if (!PluginLogLevels::ShouldLog(name, ToFileLevel(level)))
			return;

		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		PluginLog(level, self, buffer);
	}

	static void PluginLogTrace(const IPluginSelf* self, const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		PluginLogFormatted(PluginLogLevel::Trace, self, format, args);
		va_end(args);
	}

	static void PluginLogDebug(const IPluginSelf* self, const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		PluginLogFormatted(PluginLogLevel::Debug, self, format, args);
		va_end(args);
	}

	static void PluginLogInfo(const IPluginSelf* self, const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		PluginLogFormatted(PluginLogLevel::Info, self, format, args);
		va_end(args);
	}

	static void PluginLogWarn(const IPluginSelf* self, const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		PluginLogFormatted(PluginLogLevel::Warn, self, format, args);
		va_end(args);
	}

	static void PluginLogError(const IPluginSelf* self, const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		PluginLogFormatted(PluginLogLevel::Error, self, format, args);
		va_end(args);
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
		LogToFile::Info("[ModLoader] Logger initialized (using Log:: backend)");
	}

	void ShutdownLogger()
	{
		LogToFile::Info("[ModLoader] Logger shutting down");
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
		_vsnwprintf_s(buffer, 1024, _TRUNCATE, format, args);
		va_end(args);

		// Convert wide string to narrow for Log:: system
		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		LogToFile::Trace("[ModLoader] %s", narrowBuffer);
	}

	void LogDebug(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(buffer, 1024, _TRUNCATE, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		LogToFile::Debug("[ModLoader] %s", narrowBuffer);
	}

	void LogInfo(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(buffer, 1024, _TRUNCATE, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		LogToFile::Info("[ModLoader] %s", narrowBuffer);
	}

	void LogWarn(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(buffer, 1024, _TRUNCATE, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		LogToFile::Warn("[ModLoader] %s", narrowBuffer);
	}

	void LogError(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(buffer, 1024, _TRUNCATE, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		LogToFile::Error("[ModLoader] %s", narrowBuffer);
	}

	// Keep legacy LogMessage for compatibility - maps to Info level
	void LogMessage(const wchar_t* format, ...)
	{
		if (!g_logInitialized) return;

		wchar_t buffer[1024];
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(buffer, 1024, _TRUNCATE, format, args);
		va_end(args);

		char narrowBuffer[1024];
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeof(narrowBuffer), nullptr, nullptr);

		LogToFile::Info("[ModLoader] %s", narrowBuffer);
	}
}
