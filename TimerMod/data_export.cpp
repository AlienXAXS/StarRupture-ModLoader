#include "data_export.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdarg>
#include <fstream>

namespace DataExport
{

static float s_accumulator = 0.0f;

// ---------------------------------------------------------------------------
// Build a simple JSON string from the timer state.
// No external JSON library needed — the structure is fixed and simple.
// Negative float values are written as JSON null (unknown / not applicable).
// ---------------------------------------------------------------------------

// Append a formatted string to buf at offset *pos, updating *pos on success.
static void Append(char* buf, int bufSize, int* pos, const char* fmt, ...)
{
	if (*pos < 0 || *pos >= bufSize) return;
	va_list args;
	va_start(args, fmt);
	int written = _vsnprintf_s(buf + *pos, bufSize - *pos, _TRUNCATE, fmt, args);
	va_end(args);
	if (written > 0) *pos += written;
}

// Write a JSON key: value line where negative value → null.
static void AppendNullableFloat(char* buf, int bufSize, int* pos, const char* key, float val, bool comma)
{
	if (val < 0.0f)
		Append(buf, bufSize, pos, "  \"%s\": null%s\n", key, comma ? "," : "");
	else
		Append(buf, bufSize, pos, "  \"%s\": %.1f%s\n", key, val, comma ? "," : "");
}

static void WriteJson(const char* filePath, const RuptureTimer::TimerState& state)
{
	char buf[1024];
	int pos = 0;

	if (!state.valid)
	{
		Append(buf, sizeof(buf), &pos, "{\n  \"valid\": false\n}\n");
	}
	else
	{
		const bool extended = RuptureTimerConfig::Config::ShouldWriteExtendedPhaseTimers();

		Append(buf, sizeof(buf), &pos, "{\n");
		Append(buf, sizeof(buf), &pos, "  \"valid\": true,\n");
		Append(buf, sizeof(buf), &pos, "  \"phase\": \"%s\",\n",    state.phaseName);
		AppendNullableFloat(buf, sizeof(buf), &pos, "phase_remaining_sec", state.phaseRemainingSeconds, true);
		AppendNullableFloat(buf, sizeof(buf), &pos, "next_rupture_in_sec", state.nextRuptureInSeconds,  true);
		Append(buf, sizeof(buf), &pos, "  \"wave_number\": %d,\n",  state.waveNumber);
		Append(buf, sizeof(buf), &pos, "  \"wave_type\": \"%s\",\n", state.waveTypeName);

		if (extended)
		{
			Append(buf, sizeof(buf), &pos, "  \"paused\": %s,\n", state.paused ? "true" : "false");
			AppendNullableFloat(buf, sizeof(buf), &pos, "warning_remaining_sec",     state.warningRemaining,     true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "burning_remaining_sec",     state.burningRemaining,     true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "cooling_remaining_sec",     state.coolingRemaining,     true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "stabilizing_remaining_sec", state.stabilizingRemaining, true);
			AppendNullableFloat(buf, sizeof(buf), &pos, "stable_remaining_sec",      state.stableRemaining,      false);
		}
		else
		{
			Append(buf, sizeof(buf), &pos, "  \"paused\": %s\n", state.paused ? "true" : "false");
		}

		Append(buf, sizeof(buf), &pos, "}\n");
	}

	std::ofstream f(filePath, std::ios::out | std::ios::trunc);
	if (!f.is_open())
	{
		LOG_WARN("DataExport: failed to open %s for writing", filePath);
		return;
	}

	f << buf;
	f.close();
}

void EnsureOutputDir()
{
	const char* path = RuptureTimerConfig::Config::GetJsonFilePath();

	// Find last separator to extract directory part
	const char* lastSlash = nullptr;
	for (const char* p = path; *p; p++)
		if (*p == '/' || *p == '\\') lastSlash = p;

	if (!lastSlash) return;

	char dirBuf[512];
	size_t dirLen = static_cast<size_t>(lastSlash - path);
	if (dirLen >= sizeof(dirBuf)) return;

	memcpy(dirBuf, path, dirLen);
	dirBuf[dirLen] = '\0';

	CreateDirectoryA(dirBuf, nullptr);
	// Ignore ERROR_ALREADY_EXISTS — that's fine
}

void Update(float deltaSeconds, const RuptureTimer::TimerState& state)
{
	if (!RuptureTimerConfig::Config::ShouldWriteJsonFile())
	{
		static bool s_warned = false;
		if (!s_warned) { s_warned = true; LOG_WARN("DataExport: WriteJsonFile is disabled in config — JSON will not update"); }
		return;
	}

	s_accumulator += deltaSeconds;
	float interval = RuptureTimerConfig::Config::GetUpdateIntervalSeconds();
	if (interval < 0.1f) interval = 0.1f;

	if (s_accumulator < interval) return;
	s_accumulator = 0.0f;

	LOG_DEBUG("DataExport: writing %s (phase=%s remaining=%.1f)",
		RuptureTimerConfig::Config::GetJsonFilePath(),
		state.phaseName ? state.phaseName : "?",
		state.phaseRemainingSeconds);
	WriteJson(RuptureTimerConfig::Config::GetJsonFilePath(), state);
}

} // namespace DataExport
