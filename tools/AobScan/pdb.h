#pragma once
// Opens the game exe's own PDB through DbgHelp, for --suggest, --layout and
// the symbolRva and nameMatch of --json.
#include <cstdint>
#include <cwctype>
#include <optional>
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

	// A symbol name as nameMatch compares it: the class and struct keywords go,
	// and so does every space that does not separate two words. Nothing else
	// changes, so the comparison is exact.
	inline std::wstring ComparableName(const std::wstring& name)
	{
		const auto inWord = [](wchar_t c) { return iswalnum(c) || c == L'_' || c == L'$'; };
		std::wstring out;
		bool space = false;
		for (size_t i = 0; i < name.size();)
		{
			if (iswspace(name[i]))
			{
				space = true;
				++i;
				continue;
			}
			size_t end = i + 1;
			if (inWord(name[i]))
			{
				while (end < name.size() && inWord(name[end]))
					++end;
				const std::wstring word = name.substr(i, end - i);
				if ((word == L"class" || word == L"struct") && end < name.size() && iswspace(name[end]))
				{
					i = end;
					continue;
				}
				if (space && !out.empty() && inWord(out.back()))
					out += L' ';
			}
			out.append(name, i, end - i);
			space = false;
			i = end;
		}
		return out;
	}

	// The comparable qualified name of a public symbol, which the PDB keeps
	// decorated: no return type, no parameters. Empty for a name that does not
	// undecorate, and for a thunk (an adjustor, for one), which is not the
	// function although its short name reads the same.
	inline std::wstring ComparablePublicName(const wchar_t* name)
	{
		if (name[0] != L'?')
			return ComparableName(name);
		wchar_t text[4096];
		if (!UnDecorateSymbolNameW(name, text, ARRAYSIZE(text), UNDNAME_COMPLETE) || wcsstr(text, L"[thunk]"))
			return {};
		if (!UnDecorateSymbolNameW(name, text, ARRAYSIZE(text), UNDNAME_NAME_ONLY))
			return {};
		return ComparableName(text);
	}

	// What the PDB says about an address of the mapped game image. It is opened
	// on the first lookup, so a run that never asks costs nothing. Symbol names
	// are compared here and never handed out.
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
			if (!Loaded())
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

		// Whether a function or public code symbol that starts exactly at `rva`
		// is named `key`. Folded functions share an address, so any one of its
		// names counts. A null `rva` has no symbol. Nothing with no matching
		// PDB, or when the PDB cannot be searched.
		std::optional<bool> Named(const uint64_t* rva, const std::string& key)
		{
			if (!Loaded())
				return std::nullopt;
			if (!rva)
				return false;

			std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, key.data(), static_cast<int>(key.size()), nullptr, 0), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, key.data(), static_cast<int>(key.size()), wide.data(),
				static_cast<int>(wide.size()));
			struct Search
			{
				DWORD64      address;
				std::wstring wanted;
				bool         found;
			} search{ base_ + *rva, ComparableName(wide), false };
			if (search.wanted.empty())
				return false;

			// DbgHelp's own undecoration would hide which publics are thunks.
			SymSetOptions(SymGetOptions() & ~SYMOPT_UNDNAME);
			// SYMSEARCH_ALLITEMS lists every name at the address, the public
			// ones included; SymFromAddr gives only one of them.
			const BOOL searched = SymSearchW(session(), base_, 0, 0, nullptr, search.address,
				[](PSYMBOL_INFOW symbol, ULONG, PVOID context) -> BOOL
				{
					constexpr ULONG kFunction = 5, kPublicSymbol = 10;   // SymTagEnum
					Search& s = *static_cast<Search*>(context);
					if (symbol->Address != s.address)
						return TRUE;
					if (symbol->Tag == kFunction)
						s.found = ComparableName(symbol->Name) == s.wanted;
					else if (symbol->Tag == kPublicSymbol && (symbol->Flags & SYMFLAG_PUBLIC_CODE))
						s.found = ComparablePublicName(symbol->Name) == s.wanted;
					return !s.found;
				}, &search, SYMSEARCH_ALLITEMS);
			if (!searched && !search.found)
				return std::nullopt;
			return search.found;
		}

	private:
		bool Loaded()
		{
			if (!tried_)
			{
				tried_ = true;
				loaded_ = LoadMatchedPdb(session(), path_, base_, size_);
			}
			return loaded_;
		}

		static HANDLE session() { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5354)); }

		std::wstring path_;
		uintptr_t    base_;
		size_t       size_;
		bool         tried_  = false;
		bool         loaded_ = false;
	};
}
