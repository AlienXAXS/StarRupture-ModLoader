#include "pch.h"
#include "game_log_filter.h"

#ifndef MODLOADER_CLIENT_BUILD

#include "core/startup_utils.h"
#include "hooks/game/engine_exec/engine_exec.h"
#include "logging/logger.h"
#include "utils/game_thread_dispatch.h"

#include <windows.h>

#include <string>
#include <vector>

namespace GameLogFilter
{
	// The two categories a stock dedicated server drowns in. Written to the ini
	// on first run rather than applied invisibly, so an operator who wants a
	// different set (or none) can see what is being done and change it.
	static constexpr const wchar_t* kDefaultRules = L"LogTemp Error,LogSkinnedMeshComp Error";

	// GetPrivateProfileString cannot distinguish "key absent" from "key present
	// but empty", and those mean opposite things here: absent is a first run
	// that should get the defaults, empty is an operator turning the feature
	// off. A default that cannot occur in a real ini separates them.
	static constexpr const wchar_t* kAbsentSentinel = L"\x01";

	// GEngine is not reliably assigned when engine-init fires (NotifyEngineReady
	// can come from the UGameEngine::Init detour). The first tick is the
	// earliest point it certainly is, so a few retries cover the gap without
	// needing to reason about which hook won the race.
	static constexpr int kMaxAttempts = 10;

	static bool g_done = false;

	// Returns the raw configured value, or kAbsentSentinel when the key is not
	// in the file at all. GetPrivateProfileString cannot tell "absent" from
	// "present but empty" any other way, and those mean opposite things here:
	// absent is a first run that should get the defaults, empty is an operator
	// deliberately turning the feature off.
	static std::wstring ReadRawRules()
	{
		const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");

		wchar_t buf[1024] = {};
		GetPrivateProfileStringW(L"Logging", L"GameLogCategories", kAbsentSentinel,
		                         buf, static_cast<DWORD>(std::size(buf)), iniPath.c_str());

		return buf;
	}

	void EnsureConfigDefaults()
	{
		if (ReadRawRules() != kAbsentSentinel)
			return;

		const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");

		if (WritePrivateProfileStringW(L"Logging", L"GameLogCategories",
		                               kDefaultRules, iniPath.c_str()))
		{
			ModLoaderLogger::LogInfo(
				L"[GameLogFilter] [Logging] GameLogCategories was not set -- wrote the default "
				L"'%s' to modloader.ini", kDefaultRules);
		}
		else
		{
			// Not fatal: the defaults still apply this session, the operator
			// just has no visible knob until the write succeeds.
			ModLoaderLogger::LogWarn(
				L"[GameLogFilter] Could not write [Logging] GameLogCategories to '%s' (error %lu) "
				L"-- the default is still applied this session",
				iniPath.c_str(), GetLastError());
		}
	}

	static std::wstring ReadRules()
	{
		const std::wstring raw = ReadRawRules();

		// Absent here means EnsureConfigDefaults could not write the key. Use
		// the defaults anyway rather than silently doing nothing.
		return (raw == kAbsentSentinel) ? kDefaultRules : raw;
	}

	static std::wstring Trim(const std::wstring& s)
	{
		size_t first = s.find_first_not_of(L" \t");
		if (first == std::wstring::npos)
			return std::wstring();

		size_t last = s.find_last_not_of(L" \t");
		return s.substr(first, last - first + 1);
	}

	static std::vector<std::wstring> SplitRules(const std::wstring& rules)
	{
		std::vector<std::wstring> out;

		size_t start = 0;
		while (start <= rules.size())
		{
			const size_t comma = rules.find(L',', start);
			const size_t end   = (comma == std::wstring::npos) ? rules.size() : comma;

			const std::wstring entry = Trim(rules.substr(start, end - start));
			if (!entry.empty())
				out.push_back(entry);

			if (comma == std::wstring::npos)
				break;

			start = comma + 1;
		}

		return out;
	}

	// Returns false only when the engine was not ready and the caller should
	// try again on a later tick. A rule the engine rejected is reported and
	// does not earn a retry -- it will be rejected again next frame too.
	static bool TryApply()
	{
		const std::wstring rules = ReadRules();
		if (rules.empty())
		{
			ModLoaderLogger::LogDebug(
				L"[GameLogFilter] [Logging] GameLogCategories is empty -- leaving the game log alone");
			return true;
		}

		const std::vector<std::wstring> entries = SplitRules(rules);
		if (entries.empty())
			return true;

		if (!Hooks::EngineExec::IsAvailable() && !Hooks::EngineExec::Resolve())
		{
			ModLoaderLogger::LogWarn(
				L"[GameLogFilter] UGameEngine::Exec unresolved -- cannot suppress game log categories");
			return true;
		}

		size_t applied = 0;

		for (const std::wstring& entry : entries)
		{
			const std::wstring command = L"log " + entry;

			std::wstring output;
			const Hooks::EngineExec::Result result =
				Hooks::EngineExec::Execute(command.c_str(), output);

			switch (result)
			{
			case Hooks::EngineExec::Result::Handled:
				++applied;
				ModLoaderLogger::LogInfo(L"[GameLogFilter] [OK] %s", entry.c_str());
				break;

			case Hooks::EngineExec::Result::Unavailable:
				// No GEngine yet. Nothing has been applied, so retry wholesale
				// rather than leaving a half-applied list behind.
				return false;

			case Hooks::EngineExec::Result::Unhandled:
				ModLoaderLogger::LogWarn(
					L"[GameLogFilter] [FAIL] the engine did not accept 'log %s' -- check the "
					L"category name and verbosity (off/error/warning/display/log/verbose/all)",
					entry.c_str());
				break;
			}
		}

		ModLoaderLogger::LogInfo(L"[GameLogFilter] %zu of %zu category rule(s) applied",
		                         applied, entries.size());
		return true;
	}

	static void Attempt(int attempt)
	{
		if (g_done)
			return;

		if (TryApply())
		{
			g_done = true;
			return;
		}

		if (attempt >= kMaxAttempts)
		{
			g_done = true;
			ModLoaderLogger::LogWarn(
				L"[GameLogFilter] gave up after %d tick(s) waiting for GEngine -- game log "
				L"categories were not suppressed", kMaxAttempts);
			return;
		}

		// Drain() swaps the queue, so this runs on the next tick, not this one.
		GameThreadDispatch::PostVoid([attempt]() { Attempt(attempt + 1); });
	}

	void Schedule()
	{
		GameThreadDispatch::PostVoid([]() { Attempt(1); });
	}
}

#endif // !MODLOADER_CLIENT_BUILD
