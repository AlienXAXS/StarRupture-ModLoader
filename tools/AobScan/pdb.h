#pragma once
// Opens the game exe's own PDB through DbgHelp, for --suggest, --layout and
// the symbolRva of --json.
#include <cstdint>
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

	// Where the PDB symbol that contains an address of the mapped game image
	// starts, as an RVA. The PDB is opened on the first lookup, so a run that
	// never asks costs nothing. The symbol's name is never read out.
	class SymbolStarts
	{
	public:
		SymbolStarts(const wchar_t* exePath, uintptr_t base, size_t size)
			: path_(exePath), base_(base), size_(size) {}
		~SymbolStarts()
		{
			if (tried_)
				SymCleanup(session());
		}
		SymbolStarts(const SymbolStarts&) = delete;
		SymbolStarts& operator=(const SymbolStarts&) = delete;

		// False with no matching PDB, or when no symbol holds the address.
		bool Of(uint64_t rva, uint64_t& start)
		{
			if (!tried_)
			{
				tried_ = true;
				loaded_ = LoadMatchedPdb(session(), path_, base_, size_);
			}
			if (!loaded_)
				return false;
			SYMBOL_INFOW info{};
			info.SizeOfStruct = sizeof(info);
			info.MaxNameLen   = 1;
			DWORD64 displacement = 0;
			if (!SymFromAddrW(session(), base_ + rva, &displacement, &info) || info.Address < base_)
				return false;
			if (displacement != 0 && displacement >= info.Size)
				return false;
			start = info.Address - base_;
			return true;
		}

	private:
		static HANDLE session() { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5354)); }

		std::wstring path_;
		uintptr_t    base_;
		size_t       size_;
		bool         tried_  = false;
		bool         loaded_ = false;
	};
}
