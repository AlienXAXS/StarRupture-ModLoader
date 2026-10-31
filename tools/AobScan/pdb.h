#pragma once
// Opens the game exe's own PDB through DbgHelp, for --suggest and --layout.
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

#ifndef SYMOPT_DISABLE_SYMSRV_AUTODETECTION
#define SYMOPT_DISABLE_SYMSRV_AUTODETECTION 0x02000000
#endif

namespace AobScan
{
	// session is any unique value: with fInvadeProcess FALSE, DbgHelp only uses
	// it as a session key. The PDB has to sit next to the exe and match it
	// exactly; no symbol server is consulted. The caller owns the session and
	// calls SymCleanup on it, whether or not this succeeds.
	inline bool LoadMatchedPdb(HANDLE session, const std::wstring& exePath, DWORD64 base, size_t size)
	{
		SymSetOptions(SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_EXACT_SYMBOLS
			| SYMOPT_DISABLE_SYMSRV_AUTODETECTION);
		const size_t slash = exePath.find_last_of(L"\\/");
		const std::wstring dir = slash == std::wstring::npos ? L"." : exePath.substr(0, slash);
		if (!SymInitializeW(session, dir.c_str(), FALSE))
			return false;
		if (!SymLoadModuleExW(session, nullptr, exePath.c_str(), nullptr, base, static_cast<DWORD>(size), nullptr, 0))
			return false;
		IMAGEHLP_MODULEW64 info{};
		info.SizeOfStruct = sizeof(info);
		return SymGetModuleInfoW64(session, base, &info) && info.SymType == SymPdb && !info.PdbUnmatched;
	}
}
