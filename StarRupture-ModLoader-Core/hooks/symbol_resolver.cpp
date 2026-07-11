#include "pch.h"
#include "symbol_resolver.h"
#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")

namespace Hooks::SymbolResolver
{
	static std::mutex g_mutex;
	static bool g_initialized = false;

	std::mutex& GetMutex()
	{
		return g_mutex;
	}

	void EnsureInitialized()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_initialized)
			return;

		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
		// nullptr search path -- DbgHelp's own default covers: current
		// directory, each loaded module's own directory (this is what picks
		// up a PDB dropped next to StarRupture-Win64-Shipping.exe), then
		// _NT_SYMBOL_PATH / _NT_ALT_SYMBOL_PATH if set. fInvadeProcess=TRUE
		// enumerates every currently-loaded module so nothing needs to be
		// registered by hand.
		SymInitialize(GetCurrentProcess(), nullptr, TRUE);
		g_initialized = true;
	}

	std::string Resolve(uintptr_t address, uintptr_t* outDisplacement)
	{
		if (!address)
			return {};

		EnsureInitialized();

		std::lock_guard<std::mutex> lock(g_mutex);

		uint8_t symBuffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
		auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(symBuffer);
		symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
		symbol->MaxNameLen = MAX_SYM_NAME;

		DWORD64 displacement = 0;
		if (!SymFromAddrW(GetCurrentProcess(), static_cast<DWORD64>(address), &displacement, symbol))
			return {};

		if (outDisplacement)
			*outDisplacement = static_cast<uintptr_t>(displacement);

		char narrow[MAX_SYM_NAME]{};
		WideCharToMultiByte(CP_UTF8, 0, symbol->Name, -1, narrow, MAX_SYM_NAME, nullptr, nullptr);
		return narrow;
	}
}
