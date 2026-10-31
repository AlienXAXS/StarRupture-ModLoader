// sr-aobscan --suggest [--reference <current.exe>]: for each pattern that is
// MISSING or AMBIGUOUS on the scanned build, find the function it was meant to
// land on and propose a pattern that resolves to it again.
//
// Finding the function, in this order:
//   reference  The current pattern still resolves on the reference build (the
//              one players run today). The function it lands on is named by
//              that build's PDB, and the same name is looked up in the scanned
//              build's PDB. Without a PDB on the scanned build, the reference
//              function's whole body (relocations, [rip+disp32] and branch
//              operands wildcarded) is searched for instead ("bytes").
//   name       The hook name is a function in the scanned build's PDB.
//   renamed    The hook's class has exactly one method whose name carries every
//              word of the hook's method name, in order, with the fewest extra
//              words (GetResourceConditionResult -> GetPlacementResource...).
//
// Building the pattern: the function's instructions are decoded, and every
// byte that changes with the build's layout rather than its code -- base
// relocations, [rip+disp32] displacements, rel8/rel32 branch operands -- is a
// wildcard. The proposal is the shortest instruction-aligned prefix, at least
// kMinPatternBytes long, that matches exactly once. When the reference
// function is known, both builds' instructions are merged first (a byte that
// differs becomes a wildcard) so one pattern serves both. When the function's
// own bytes are not unique, the pattern is a window around a call to it, with
// followRel32At on the call, as the loader's FOLLOW_REL32 requests do.
//
// Every proposal is checked with ScanValidation::Resolve, the loader's own
// resolve, on each build it claims to serve.
//
// Output is pattern strings, RVAs and verdicts only. Symbol names are read to
// find functions and are never printed: the scanned image and its PDB are not
// ours to publish.
#include "suggest.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <DbgHelp.h>

#include "memory_scanner/image_info.h"
#include "memory_scanner/scanner.h"
#include "plugin_scan.h"

#ifndef SYMOPT_DISABLE_SYMSRV_AUTODETECTION
#define SYMOPT_DISABLE_SYMSRV_AUTODETECTION 0x02000000
#endif

namespace
{
	constexpr size_t kMinPatternBytes = 16;   // a 14-byte detour's worth, and then some
	constexpr size_t kMaxPatternBytes = 64;
	constexpr size_t kMaxBodyBytes    = 512;  // "bytes": how much of the reference body to search for
	constexpr size_t kMinBodyBytes    = 24;
	constexpr size_t kMaxBefore       = 6;    // call windows: instructions before the call
	constexpr size_t kMaxAfter        = 8;    // and after it
	constexpr size_t kMaxCallSites    = 8;
	constexpr ULONG  kSymTagFunction  = 5;    // cvconst.h SymTagFunction
	constexpr ULONG  kMaxSymbolName   = 2000;

	std::string Hex(uint64_t value)
	{
		char buf[24];
		sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(value));
		return buf;
	}

	std::wstring Widen(const std::string& s)
	{
		if (s.empty())
			return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0)
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
		return out;
	}

	// ---- x64 instruction decoding ---------------------------------------------
	//
	// Hooks::GetInstructionLength measures the prologue forms a detour has to
	// relocate and reports nothing about operands. A pattern needs to know where
	// in every instruction the displacement and the immediate sit, and whether
	// either is relative, so this decodes the whole general-purpose, x87, SSE and
	// VEX/EVEX encoding space. An opcode it does not know ends the decode there
	// rather than guessing a length.

	struct Insn
	{
		uint8_t length   = 0;
		uint8_t dispAt   = 0;
		uint8_t dispSize = 0;
		uint8_t immAt    = 0;
		uint8_t immSize  = 0;
		bool    rip      = false;  // the displacement is [rip+disp32]
		bool    relative = false;  // the immediate is a branch displacement
		bool    near32   = false;  // E8 / E9 rel32: the loader's followRel32At form
	};

	bool Decode(const uint8_t* p, size_t avail, Insn& out)
	{
		if (avail > 15)
			avail = 15;

		size_t i = 0;
		bool opsize16 = false, adsize32 = false;
		for (;; ++i)
		{
			if (i >= avail)
				return false;
			const uint8_t b = p[i];
			if (b == 0x66)      opsize16 = true;
			else if (b == 0x67) adsize32 = true;
			else if (b != 0xF0 && b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x36 && b != 0x3E
			         && b != 0x26 && b != 0x64 && b != 0x65)
				break;
		}

		bool rexW = false;
		if ((p[i] & 0xF0) == 0x40)
		{
			rexW = (p[i] & 0x08) != 0;
			if (++i >= avail)
				return false;
		}

		int  map = 0;  // 0 one-byte, 1 0F, 2 0F38, 3 0F3A
		bool vex = false;
		uint8_t op = p[i];
		if (op == 0xC4 || op == 0xC5 || op == 0x62)
		{
			// VEX (C4 / C5) and EVEX (62): in 64-bit mode these bytes are always
			// the prefix, never LES / LDS / BOUND.
			const size_t prefix = op == 0xC5 ? 2 : op == 0xC4 ? 3 : 4;
			if (i + prefix >= avail)
				return false;
			map = op == 0xC5 ? 1 : op == 0xC4 ? (p[i + 1] & 0x1F) : (p[i + 1] & 0x07);
			if (map < 1 || map > 3)
				return false;
			vex = true;
			i  += prefix;
			op  = p[i++];
		}
		else if (op == 0x0F)
		{
			if (++i >= avail)
				return false;
			if (p[i] == 0x38 || p[i] == 0x3A)
			{
				map = p[i] == 0x38 ? 2 : 3;
				if (++i >= avail)
					return false;
			}
			else
				map = 1;
			op = p[i++];
		}
		else
			++i;

		const size_t immz = opsize16 && !rexW ? 2 : 4;  // the "z" operand size; REX.W wins over 66
		bool   modrm = false;
		size_t imm   = 0;
		bool   rel   = false;

		if (map == 0)
		{
			if (op < 0x40)
			{
				switch (op & 7)
				{
				case 0: case 1: case 2: case 3: modrm = true; break;
				case 4:                         imm = 1;      break;
				case 5:                         imm = immz;   break;
				default:                        return false;  // invalid in 64-bit mode
				}
			}
			else if (op <= 0x4F)                    return false;  // a second REX
			else if (op <= 0x5F)                    {}
			else if (op == 0x63)                    modrm = true;
			else if (op == 0x68)                    imm = immz;
			else if (op == 0x69)                    { modrm = true; imm = immz; }
			else if (op == 0x6A)                    imm = 1;
			else if (op == 0x6B)                    { modrm = true; imm = 1; }
			else if (op >= 0x6C && op <= 0x6F)      {}
			else if (op >= 0x70 && op <= 0x7F)      { imm = 1; rel = true; }
			else if (op == 0x80 || op == 0x83)      { modrm = true; imm = 1; }
			else if (op == 0x81)                    { modrm = true; imm = immz; }
			else if (op >= 0x84 && op <= 0x8F)      modrm = true;
			else if (op >= 0x90 && op <= 0x99)      {}
			else if (op >= 0x9B && op <= 0x9F)      {}
			else if (op >= 0xA0 && op <= 0xA3)      imm = adsize32 ? 4 : 8;  // moffs
			else if (op >= 0xA4 && op <= 0xA7)      {}
			else if (op == 0xA8)                    imm = 1;
			else if (op == 0xA9)                    imm = immz;
			else if (op >= 0xAA && op <= 0xAF)      {}
			else if (op >= 0xB0 && op <= 0xB7)      imm = 1;
			else if (op >= 0xB8 && op <= 0xBF)      imm = rexW ? 8 : immz;
			else if (op == 0xC0 || op == 0xC1)      { modrm = true; imm = 1; }
			else if (op == 0xC2 || op == 0xCA)      imm = 2;
			else if (op == 0xC3 || op == 0xC9 || op == 0xCB || op == 0xCC || op == 0xCF) {}
			else if (op == 0xC6)                    { modrm = true; imm = 1; }
			else if (op == 0xC7)                    { modrm = true; imm = immz; }
			else if (op == 0xC8)                    imm = 3;
			else if (op == 0xCD)                    imm = 1;
			else if (op >= 0xD0 && op <= 0xD3)      modrm = true;
			else if (op == 0xD7)                    {}
			else if (op >= 0xD8 && op <= 0xDF)      modrm = true;
			else if (op >= 0xE0 && op <= 0xE3)      { imm = 1; rel = true; }
			else if (op >= 0xE4 && op <= 0xE7)      imm = 1;
			else if (op == 0xE8 || op == 0xE9)      { imm = 4; rel = true; }
			else if (op == 0xEB)                    { imm = 1; rel = true; }
			else if (op >= 0xEC && op <= 0xEF)      {}
			else if (op == 0xF1 || op == 0xF4 || op == 0xF5 || (op >= 0xF8 && op <= 0xFD)) {}
			else if (op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) modrm = true;
			else                                    return false;
		}
		else if (map == 1)
		{
			const bool imm8 = (op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
			if (vex)
			{
				modrm = op != 0x77;  // VZEROUPPER / VZEROALL
				imm   = imm8 ? 1 : 0;
			}
			else if (op == 0x04 || op == 0x0A || op == 0x0C || (op >= 0x24 && op <= 0x27) || op == 0x36
			         || op == 0x39 || (op >= 0x3B && op <= 0x3F) || op == 0x7A || op == 0x7B)
				return false;
			else if (op == 0x05 || op == 0x06 || op == 0x07 || op == 0x08 || op == 0x09 || op == 0x0B
			         || op == 0x0E || (op >= 0x30 && op <= 0x35) || op == 0x37 || op == 0x77
			         || op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA8 || op == 0xA9 || op == 0xAA
			         || (op >= 0xC8 && op <= 0xCF))
				{}
			else if (op >= 0x80 && op <= 0x8F)      { imm = 4; rel = true; }  // Jcc rel32
			else if (imm8 || op == 0xA4 || op == 0xAC || op == 0xBA || op == 0x0F) { modrm = true; imm = 1; }
			else                                    modrm = true;
		}
		else
		{
			modrm = true;
			imm   = map == 3 ? 1 : 0;
		}

		size_t dispAt = 0, disp = 0;
		bool   rip    = false;
		if (modrm)
		{
			if (i >= avail)
				return false;
			const uint8_t m   = p[i++];
			const uint8_t mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
			uint8_t base = 0;
			if (mod != 3 && rm == 4)
			{
				if (i >= avail)
					return false;
				base = p[i++] & 7;
			}
			if (mod == 1)                           disp = 1;
			else if (mod == 2)                      disp = 4;
			else if (mod == 0 && rm == 5)           { disp = 4; rip = true; }
			else if (mod == 0 && rm == 4 && base == 5) disp = 4;

			if (map == 0 && (op == 0xF6 || op == 0xF7) && reg < 2)
				imm = op == 0xF6 ? 1 : immz;        // TEST r/m, imm
			if (map == 0 && op == 0xC7 && m == 0xF8)
				rel = true;                         // XBEGIN rel32
			dispAt = i;
		}

		const size_t length = i + disp + imm;
		if (length > avail)
			return false;

		out = Insn{};
		out.length   = static_cast<uint8_t>(length);
		out.dispAt   = static_cast<uint8_t>(dispAt);
		out.dispSize = static_cast<uint8_t>(disp);
		out.immAt    = static_cast<uint8_t>(i + disp);
		out.immSize  = static_cast<uint8_t>(imm);
		out.rip      = rip;
		out.relative = rel;
		out.near32   = map == 0 && (op == 0xE8 || op == 0xE9) && length == i + 4 && !opsize16;
		return true;
	}

	// ---- Symbols --------------------------------------------------------------

	struct Function
	{
		uintptr_t    address = 0;
		uint32_t     size    = 0;
		std::wstring name;
	};

	class Symbols
	{
	public:
		~Symbols()
		{
			if (process_)
				SymCleanup(process_);
		}

		// process is any unique value: with fInvadeProcess FALSE, DbgHelp only
		// uses it as a session key. The PDB has to sit next to the exe and match
		// it exactly; no symbol server is consulted.
		bool Load(HANDLE process, const std::wstring& exePath, uintptr_t base, size_t size)
		{
			const size_t slash = exePath.find_last_of(L"\\/");
			const std::wstring dir = slash == std::wstring::npos ? L"." : exePath.substr(0, slash);
			if (!SymInitializeW(process, dir.c_str(), FALSE))
				return false;
			process_ = process;
			if (!SymLoadModuleExW(process, nullptr, exePath.c_str(), nullptr, base, static_cast<DWORD>(size), nullptr, 0))
				return false;
			IMAGEHLP_MODULEW64 info{};
			info.SizeOfStruct = sizeof(info);
			if (!SymGetModuleInfoW64(process, base, &info) || info.SymType != SymPdb || info.PdbUnmatched)
				return false;
			base_   = base;
			loaded_ = true;
			return true;
		}

		bool Loaded() const { return loaded_; }

		// The function symbol that starts exactly at address.
		bool At(uintptr_t address, Function& out) const
		{
			if (!loaded_)
				return false;
			alignas(SYMBOL_INFOW) uint8_t buf[sizeof(SYMBOL_INFOW) + kMaxSymbolName * sizeof(wchar_t)]{};
			auto* info = reinterpret_cast<SYMBOL_INFOW*>(buf);
			info->SizeOfStruct = sizeof(SYMBOL_INFOW);
			info->MaxNameLen   = kMaxSymbolName;
			DWORD64 displacement = 0;
			if (!SymFromAddrW(process_, address, &displacement, info) || displacement != 0 || info->Tag != kSymTagFunction)
				return false;
			out.address = static_cast<uintptr_t>(info->Address);
			out.size    = info->Size;
			out.name.assign(info->Name, wcsnlen(info->Name, kMaxSymbolName));
			return true;
		}

		// Every function named exactly name (overloads included).
		std::vector<Function> Named(const std::wstring& name) const
		{
			return Enumerate(name, [&name](const std::wstring& n) { return n == name; });
		}

		// Every function declared directly in scope ("Class::Method", not
		// "Class::Nested::Method").
		std::vector<Function> Methods(const std::wstring& scope) const
		{
			const std::wstring prefix = scope + L"::";
			return Enumerate(prefix + L"*", [&prefix](const std::wstring& n) {
				return n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0
				    && n.find(L"::", prefix.size()) == std::wstring::npos;
			});
		}

	private:
		template <typename Keep>
		std::vector<Function> Enumerate(const std::wstring& mask, Keep&& keep) const
		{
			struct Context
			{
				Keep*                  keep;
				std::vector<Function>* out;
			};
			std::vector<Function> out;
			if (!loaded_)
				return out;
			Context ctx{ &keep, &out };
			SymEnumSymbolsW(process_, base_, mask.c_str(),
				[](PSYMBOL_INFOW s, ULONG, PVOID user) -> BOOL {
					auto* c = static_cast<Context*>(user);
					if (s->Tag != kSymTagFunction)
						return TRUE;
					std::wstring name(s->Name, wcsnlen(s->Name, s->MaxNameLen ? s->MaxNameLen : kMaxSymbolName));
					if ((*c->keep)(name))
						c->out->push_back({ static_cast<uintptr_t>(s->Address), s->Size, std::move(name) });
					return TRUE;
				}, &ctx);
			return out;
		}

		HANDLE  process_ = nullptr;
		DWORD64 base_    = 0;
		bool    loaded_  = false;
	};

	// ---- Images ---------------------------------------------------------------

	struct Image
	{
		HMODULE               module = nullptr;
		uintptr_t             base   = 0;
		size_t                size   = 0;
		std::vector<uint32_t> relocs;  // DIR64 sites, ascending
		Symbols               symbols;

		void Index()
		{
			base = reinterpret_cast<uintptr_t>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			size = nt->OptionalHeader.SizeOfImage;

			const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
			size_t offset = 0;
			while (dir.VirtualAddress && offset + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size)
			{
				const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base + dir.VirtualAddress + offset);
				if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
					break;
				const size_t entries = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
				const auto*  list    = reinterpret_cast<const uint16_t*>(block + 1);
				for (size_t i = 0; i < entries; ++i)
					if ((list[i] >> 12) == IMAGE_REL_BASED_DIR64)
						relocs.push_back(block->VirtualAddress + (list[i] & 0x0FFF));
				offset += block->SizeOfBlock;
			}
			std::sort(relocs.begin(), relocs.end());
		}

		bool Relocated(uintptr_t address) const
		{
			const auto rva = static_cast<uint32_t>(address - base);
			const auto it  = std::upper_bound(relocs.begin(), relocs.end(), rva);
			return it != relocs.begin() && *(it - 1) + 8 > rva;
		}

		uintptr_t End() const { return base + size; }
	};

	// ---- Instruction streams --------------------------------------------------

	struct Stream
	{
		std::vector<uint8_t>   bytes;
		std::vector<uint8_t>   wild;
		std::vector<uint32_t>  ends;     // end offset of instruction i
		std::vector<uintptr_t> targets;  // E8/E9 rel32 target of instruction i, else 0

		size_t   Count() const { return ends.size(); }
		uint32_t Begin(size_t i) const { return i ? ends[i - 1] : 0; }
		uint32_t Length(size_t i) const { return ends[i] - Begin(i); }
	};

	// Decodes [from, to) as far as it decodes, with every layout-dependent byte
	// marked wild.
	Stream DecodeRange(const Image& image, uintptr_t from, uintptr_t to)
	{
		Stream s;
		if (to > image.End())
			to = image.End();
		uintptr_t at = from;
		while (at < to)
		{
			Insn insn;
			const auto* p = reinterpret_cast<const uint8_t*>(at);
			if (!Decode(p, image.End() - at, insn) || at + insn.length > to)
				break;
			const size_t first = s.bytes.size();
			for (size_t k = 0; k < insn.length; ++k)
			{
				s.bytes.push_back(p[k]);
				s.wild.push_back(image.Relocated(at + k) ? 1 : 0);
			}
			if (insn.rip)
				std::fill_n(s.wild.begin() + first + insn.dispAt, insn.dispSize, 1);
			if (insn.relative)
				std::fill_n(s.wild.begin() + first + insn.immAt, insn.immSize, 1);
			uintptr_t target = 0;
			if (insn.near32)
			{
				int32_t rel32;
				memcpy(&rel32, p + insn.length - 4, sizeof(rel32));
				target = at + insn.length + static_cast<intptr_t>(rel32);
			}
			at += insn.length;
			s.ends.push_back(static_cast<uint32_t>(at - from));
			s.targets.push_back(target);
		}
		return s;
	}

	// Appends instruction i of a, merged with instruction j of b (same length):
	// a byte both builds agree on stays, anything else is a wildcard.
	void AppendMerged(Stream& out, const Stream& a, size_t i, const Stream& b, size_t j)
	{
		const uint32_t ai = a.Begin(i), bj = b.Begin(j), length = a.Length(i);
		for (uint32_t k = 0; k < length; ++k)
		{
			const bool same = !a.wild[ai + k] && !b.wild[bj + k] && a.bytes[ai + k] == b.bytes[bj + k];
			out.bytes.push_back(a.bytes[ai + k]);
			out.wild.push_back(same ? 0 : 1);
		}
		out.ends.push_back(static_cast<uint32_t>(out.bytes.size()));
		out.targets.push_back(a.targets[i]);
	}

	// Both builds' instructions from the start, for as long as they line up.
	Stream MergePrefix(const Stream& a, const Stream& b)
	{
		Stream out;
		for (size_t i = 0; i < a.Count() && i < b.Count() && a.Length(i) == b.Length(i); ++i)
			AppendMerged(out, a, i, b, i);
		return out;
	}

	// Both builds' instructions around a[ka] / b[kb], for as far either way as
	// they line up. outAnchor is where a[ka] lands in the result.
	Stream MergeAround(const Stream& a, size_t ka, const Stream& b, size_t kb, size_t& outAnchor)
	{
		size_t before = 0, after = 0;
		while (before < kMaxBefore && before < ka && before < kb
		       && a.Length(ka - before - 1) == b.Length(kb - before - 1))
			++before;
		while (after < kMaxAfter && ka + after + 1 < a.Count() && kb + after + 1 < b.Count()
		       && a.Length(ka + after + 1) == b.Length(kb + after + 1))
			++after;

		Stream out;
		for (size_t d = before; d > 0; --d)
			AppendMerged(out, a, ka - d, b, kb - d);
		for (size_t d = 0; d <= after; ++d)
			AppendMerged(out, a, ka + d, b, kb + d);
		outAnchor = before;
		return out;
	}

	// The pattern for instructions [first, last], trailing wildcards dropped.
	std::string PatternOf(const Stream& s, size_t first, size_t last, size_t& outLength)
	{
		const size_t from = s.Begin(first);
		size_t to = s.ends[last];
		while (to > from && s.wild[to - 1])
			--to;
		outLength = to - from;
		if (to == from || s.wild[from])
			return {};
		std::string out;
		out.reserve((to - from) * 3);
		char buf[4];
		for (size_t i = from; i < to; ++i)
		{
			if (i > from)
				out += ' ';
			if (s.wild[i])
				out += "??";
			else
			{
				sprintf_s(buf, "%02X", s.bytes[i]);
				out += buf;
			}
		}
		return out;
	}

	// Exactly one match in each image. The stream was read out of every image
	// in the list at a known place, so that one match is that place.
	bool UniqueIn(const std::vector<const Image*>& images, const std::string& pattern)
	{
		for (const Image* image : images)
			if (Scanner::CountMatchesInModule(image->module, pattern, 2, nullptr) != 1)
				return false;
		return true;
	}

	struct Window
	{
		bool        found  = false;
		size_t      first  = 0;
		size_t      last   = 0;
		size_t      length = 0;
		std::string pattern;
	};

	// The shortest window of whole instructions that contains `anchor`, is at
	// least kMinPatternBytes and at most kMaxPatternBytes long, and is unique
	// in every image. startAtAnchor pins the window's start to the anchor (a
	// function-start pattern); otherwise up to kMaxBefore instructions before
	// it are tried. Uniqueness only gets easier as a window grows, so the end
	// is found by bisection.
	Window SmallestUnique(const Stream& s, size_t anchor, bool startAtAnchor, const std::vector<const Image*>& images)
	{
		Window best;
		if (anchor >= s.Count())
			return best;
		const size_t maxBefore = startAtAnchor ? 0 : (std::min)(anchor, kMaxBefore);
		for (size_t before = 0; before <= maxBefore; ++before)
		{
			const size_t first = anchor - before;
			const size_t from  = s.Begin(first);
			if (best.found && s.ends[anchor] - from >= best.length)
				break;
			if (s.wild[from] || s.ends[anchor] - from > kMaxPatternBytes)
				continue;

			size_t hi = anchor;
			while (hi + 1 < s.Count() && s.ends[hi + 1] - from <= kMaxPatternBytes
			       && (startAtAnchor || hi + 1 <= anchor + kMaxAfter))
				++hi;

			std::string pattern;
			size_t      length = 0;
			auto unique = [&](size_t last) {
				pattern = PatternOf(s, first, last, length);
				return !pattern.empty() && length >= kMinPatternBytes && UniqueIn(images, pattern);
			};
			if (!unique(hi))
				continue;
			Window here{ true, first, hi, length, pattern };
			size_t lo = anchor;
			while (lo < here.last)
			{
				const size_t mid = lo + (here.last - lo) / 2;
				if (unique(mid))
					here = { true, first, mid, length, pattern };
				else
					lo = mid + 1;
			}
			if (!best.found || here.length < best.length)
				best = here;
		}
		return best;
	}

	// ---- Call sites -----------------------------------------------------------

	struct CallSite
	{
		uintptr_t    site    = 0;  // the E8 / E9
		uintptr_t    from    = 0;  // decode range of the code around it
		uintptr_t    to      = 0;
		std::wstring caller;       // the calling function's symbol, when there is one
		size_t       ordinal = 0;  // which call to the target within that caller
	};

	// Every E8 / E9 rel32 to `function`, where it decodes as an instruction of
	// the function containing it.
	std::vector<CallSite> CallSitesOf(const Image& image, uintptr_t function)
	{
		std::vector<CallSite> sites;
		for (const Scanner::XRef& xref : Scanner::FindXrefsToAddressInModule(function, image.module))
		{
			if (!xref.isRelative)
				continue;
			const Scanner::FunctionInfo fn = Scanner::DescribeFunction(xref.address);
			if (!fn.valid)
				continue;
			CallSite site;
			site.site = xref.address;
			site.from = fn.start;
			site.to   = fn.end;
			if (fn.chainedChunk)
			{
				DWORD64 imageBase = 0;
				const PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(xref.address, &imageBase, nullptr);
				if (!rf)
					continue;
				site.from = static_cast<uintptr_t>(imageBase + rf->BeginAddress);
				site.to   = static_cast<uintptr_t>(imageBase + rf->EndAddress);
			}
			if (site.to > xref.address + 5 + kMaxPatternBytes)
				site.to = xref.address + 5 + kMaxPatternBytes;

			const Stream s = DecodeRange(image, site.from, site.to);
			bool boundary = false;
			for (size_t i = 0; i < s.Count() && !boundary; ++i)
				boundary = site.from + s.Begin(i) == xref.address && s.targets[i] == function;
			if (!boundary)
				continue;

			Function caller;
			if (image.symbols.At(fn.start, caller))
				site.caller = caller.name;
			sites.push_back(std::move(site));
		}
		std::sort(sites.begin(), sites.end(), [](const CallSite& a, const CallSite& b) { return a.site < b.site; });
		for (size_t i = 1; i < sites.size(); ++i)
			if (sites[i].from == sites[i - 1].from)
				sites[i].ordinal = sites[i - 1].ordinal + 1;
		return sites;
	}

	size_t InstructionAt(const Stream& s, uintptr_t streamStart, uintptr_t address)
	{
		for (size_t i = 0; i < s.Count(); ++i)
			if (streamStart + s.Begin(i) == address)
				return i;
		return SIZE_MAX;
	}

	// ---- The renamed-method heuristic -----------------------------------------

	std::vector<std::wstring> Words(const std::wstring& name)
	{
		std::vector<std::wstring> words;
		std::wstring word;
		for (size_t i = 0; i < name.size(); ++i)
		{
			const wchar_t c = name[i];
			if (!iswalnum(c))
			{
				if (!word.empty()) words.push_back(std::move(word));
				word.clear();
				continue;
			}
			if (!word.empty())
			{
				const wchar_t prev = word.back();
				const bool boundary =
					(iswupper(c) && (iswlower(prev) || iswdigit(prev)))
					|| (iswupper(c) && iswupper(prev) && i + 1 < name.size() && iswlower(name[i + 1]))
					|| (iswdigit(c) != 0) != (iswdigit(prev) != 0);
				if (boundary)
				{
					words.push_back(std::move(word));
					word.clear();
				}
			}
			word += c;
		}
		if (!word.empty())
			words.push_back(std::move(word));
		return words;
	}

	// How many words `candidate` adds to `old`, or -1 when it does not carry all
	// of old's words in order.
	int ExtraWords(const std::vector<std::wstring>& old, const std::vector<std::wstring>& candidate)
	{
		size_t matched = 0;
		for (const std::wstring& w : candidate)
			if (matched < old.size() && w == old[matched])
				++matched;
		return matched == old.size() ? static_cast<int>(candidate.size() - old.size()) : -1;
	}
}

namespace AobScan::Suggest
{
	struct Session::State
	{
		Image        target;
		std::wstring targetPath;
		std::wstring referencePath;
		Image        reference;
		bool         referenceMapped = false;
		std::string  referenceError;
		bool         loaded = false;

		void Load()
		{
			if (loaded)
				return;
			loaded = true;
			SymSetOptions(SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_EXACT_SYMBOLS
				| SYMOPT_DISABLE_SYMSRV_AUTODETECTION);
			target.symbols.Load(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5352)), targetPath, target.base, target.size);

			if (referencePath.empty())
				return;
			MappedImage mapped;
			if (!MapImage(referencePath.c_str(), mapped, referenceError))
				return;
			reference.module = reinterpret_cast<HMODULE>(mapped.base);
			reference.Index();
			referenceMapped = true;
			reference.symbols.Load(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5353)), referencePath, reference.base, reference.size);
		}
	};

	Session::Session(HMODULE target, const wchar_t* targetPath, const wchar_t* referencePath)
		: state_(std::make_unique<State>())
	{
		state_->target.module = target;
		state_->target.Index();
		state_->targetPath    = targetPath ? targetPath : L"";
		state_->referencePath = referencePath ? referencePath : L"";
	}

	Session::~Session() = default;

	std::string Session::EnvJson() const
	{
		const State& s = *state_;
		std::string out = "{ \"reference\": ";
		if (s.referencePath.empty())    out += "\"none\"";
		else if (!s.loaded)             out += "\"unused\"";
		else if (s.referenceMapped)     out += "\"mapped\"";
		else                            out += "\"unreadable\"";
		if (s.loaded)
		{
			out += std::string(", \"targetSymbols\": ") + (s.target.symbols.Loaded() ? "true" : "false");
			if (s.referenceMapped)
				out += std::string(", \"referenceSymbols\": ") + (s.reference.symbols.Loaded() ? "true" : "false");
		}
		return out + " }";
	}

	namespace
	{
		struct Located
		{
			uintptr_t    target        = 0;
			uintptr_t    reference     = 0;
			uintptr_t    referenceSite = 0;  // follow requests: the call the current pattern follows
			bool         referenceResolves = false;
			const char*  method     = "none";
			const char*  confidence = "";
			int          hookNameMatches = -1;
			std::string  note;
		};

		ScanValidation::Request AsRequest(const Request& r, const std::string& pattern, HMODULE module)
		{
			ScanValidation::Request v;
			v.name            = r.name;
			v.pattern         = pattern;
			v.kind            = r.kind;
			v.module          = module;
			v.resultOffset    = r.resultOffset;
			v.followRel32At   = r.followRel32At;
			v.followRipDispAt = r.followRipDispAt;
			return v;
		}

		const Function* Only(const std::vector<Function>& fns)
		{
			return fns.size() == 1 ? &fns[0] : nullptr;
		}

		uintptr_t Unique(const Symbols& symbols, const std::wstring& name)
		{
			const Function* fn = Only(symbols.Named(name));
			return fn ? fn->address : 0;
		}

		Located Locate(Session::State& s, const Request& r)
		{
			Located L;
			const bool targetSymbols = s.target.symbols.Loaded();
			const bool refSymbols    = s.referenceMapped && s.reference.symbols.Loaded();
			const std::wstring hook  = Widen(r.name);
			std::wstring symbol;  // the function's name, when a PDB gave one

			if (s.referenceMapped)
			{
				for (const std::string& pattern : r.patterns)
				{
					const ScanValidation::Result res = ScanValidation::Resolve(AsRequest(r, pattern, s.reference.module));
					if (!res.Succeeded())
						continue;
					L.referenceResolves = true;
					L.reference = res.address;
					if (r.followRel32At >= 0)
						L.referenceSite = res.rawMatch + static_cast<uintptr_t>(r.followRel32At);
					break;
				}
			}

			if (L.referenceResolves)
			{
				if (!Scanner::IsFunctionStart(L.reference))
				{
					L.reference = 0;
					L.note = "the current pattern lands inside a function on the reference build; proposals cover function starts";
					return L;
				}

				Function at;
				if (refSymbols && targetSymbols && s.reference.symbols.At(L.reference, at))
				{
					symbol = at.name;
					const std::vector<Function> same = s.target.symbols.Named(symbol);
					if (same.size() == 1)
					{
						L.target = same[0].address;
						L.method = "reference";
						L.confidence = "high";
					}
					else if (same.size() > 1)
					{
						auto gap = [&at](const Function& fn) {
							return (std::max)(fn.size, at.size) - (std::min)(fn.size, at.size);
						};
						const Function* closest = nullptr;
						bool tie = false;
						for (const Function& fn : same)
						{
							if (!closest || gap(fn) < gap(*closest))
							{
								closest = &fn;
								tie = false;
							}
							else if (gap(fn) == gap(*closest))
								tie = true;
						}
						if (tie)
							L.note = "the function is overloaded and several overloads are as close in size";
						else
						{
							L.target = closest->address;
							L.method = "reference";
							L.confidence = "medium";
							L.note = "the function is overloaded; took the overload closest in size";
						}
					}
					else
						L.note = "the reference function's name is not in the scanned build";
				}
				else if (!targetSymbols)
				{
					// No PDB to name it: look for the reference function's whole body.
					const Scanner::FunctionInfo fn = Scanner::DescribeFunction(L.reference);
					const uintptr_t end = (std::min)(fn.end, L.reference + kMaxBodyBytes);
					const Stream body = DecodeRange(s.reference, L.reference, end);
					size_t length = 0;
					const std::string pattern = body.Count() ? PatternOf(body, 0, body.Count() - 1, length) : std::string();
					uintptr_t first = 0;
					if (length >= kMinBodyBytes
					    && Scanner::CountMatchesInModule(s.target.module, pattern, 2, &first) == 1
					    && Scanner::IsFunctionStart(first))
					{
						L.target = first;
						L.method = "bytes";
						L.confidence = "medium";
						L.note = "no PDB for the scanned build; the reference function's body is there unchanged";
					}
					else
						L.note = "no PDB for the scanned build, and the reference function's body is not there unchanged";
				}
			}

			if (!L.target && targetSymbols)
			{
				const std::vector<Function> named = s.target.symbols.Named(hook);
				if (named.size() == 1)
				{
					L.target = named[0].address;
					L.method = "name";
					L.confidence = "medium";
					symbol = hook;
					L.note = (L.note.empty() ? "" : L.note + "; ")
						+ "located by the hook's label only, which may name a function other than the one hooked";
				}
				else if (named.size() > 1)
					L.note = "the hook name is overloaded on the scanned build";
				else
				{
					const size_t sep = hook.rfind(L"::");
					if (sep != std::wstring::npos && sep > 0)
					{
						const std::vector<std::wstring> oldWords = Words(hook.substr(sep + 2));
						const Function* best = nullptr;
						int bestExtra = -1;
						bool tie = false;
						const std::vector<Function> methods = s.target.symbols.Methods(hook.substr(0, sep));
						for (const Function& fn : methods)
						{
							const int extra = ExtraWords(oldWords, Words(fn.name.substr(sep + 2)));
							if (extra <= 0)
								continue;
							if (!best || extra < bestExtra)
							{
								best = &fn;
								bestExtra = extra;
								tie = false;
							}
							else if (extra == bestExtra && fn.address != best->address)
								tie = true;
						}
						if (best && !tie)
						{
							L.target = best->address;
							L.method = "renamed";
							L.confidence = "medium";
							symbol = best->name;
							L.note = "the hook's method is gone; it is the one method of its class whose name adds the fewest words to it";
						}
						else if (L.note.empty())
							L.note = tie ? "several methods of the hook's class could be the renamed one"
							             : "the hook name is not in the scanned build's PDB";
					}
					else if (L.note.empty())
						L.note = "the hook name is not in the scanned build's PDB";
				}
			}
			else if (!L.target && L.note.empty())
				L.note = s.referenceMapped ? "the current pattern does not resolve on the reference build and the scanned build has no PDB"
				                           : "the scanned build has no PDB and no reference build was given";

			if (!L.target)
				return L;

			if (!Scanner::IsFunctionStart(L.target))
			{
				L.target = 0;
				L.note = "the function found has no unwind entry starting there";
				return L;
			}

			// The reference function, when only a name found it.
			if (!L.reference && refSymbols && !symbol.empty())
			{
				const uintptr_t ref = Unique(s.reference.symbols, symbol);
				if (ref && Scanner::IsFunctionStart(ref))
					L.reference = ref;
			}

			// Cross-check against the hook name: an author's label, but for most
			// hooks it is the symbol. A function found by that label has nothing
			// to check it against.
			const bool byLabel = strcmp(L.method, "name") == 0 || strcmp(L.method, "renamed") == 0;
			if (targetSymbols && !byLabel)
			{
				const std::vector<Function> byHook = s.target.symbols.Named(hook);
				if (!byHook.empty())
				{
					bool same = false;
					for (const Function& fn : byHook)
						same = same || fn.address == L.target;
					L.hookNameMatches = same ? 1 : 0;
					if (!same)
					{
						L.confidence = "low";
						L.note = "the hook name is a different function on the scanned build";
					}
				}
				else if (!symbol.empty())
					L.hookNameMatches = symbol == hook ? 1 : 0;
			}
			return L;
		}

		// Builds a pattern for one build's function `fn`, merged with the other
		// build's `otherFn` when there is one to merge with.
		struct Builder
		{
			const Image& image;
			uintptr_t    fn;
			const Image* other     = nullptr;
			uintptr_t    otherFn   = 0;
			uintptr_t    otherSite = 0;  // the other build's call the current pattern follows

			static Stream FunctionStream(const Image& in, uintptr_t start)
			{
				const Scanner::FunctionInfo info = Scanner::DescribeFunction(start);
				const uintptr_t limit = start + kMaxPatternBytes + 15;
				return DecodeRange(in, start, info.valid && info.end < limit ? info.end : limit);
			}

			// A function-start pattern.
			bool Direct(bool merged, Proposal& out) const
			{
				const Stream own = FunctionStream(image, fn);
				const Window w = merged
					? SmallestUnique(MergePrefix(own, FunctionStream(*other, otherFn)), 0, true, { &image, other })
					: SmallestUnique(own, 0, true, { &image });
				if (!w.found)
					return false;
				out = Proposal{};
				out.pattern = w.pattern;
				return true;
			}

			// A window around a call to the function, followed.
			bool Follow(bool merged, Proposal& out) const
			{
				std::vector<CallSite> sites = CallSitesOf(image, fn);
				std::vector<CallSite> otherSites;
				if (merged)
				{
					otherSites = CallSitesOf(*other, otherFn);
					// The call the current pattern follows goes first.
					const auto followed = std::find_if(otherSites.begin(), otherSites.end(),
						[this](const CallSite& c) { return c.site == otherSite; });
					if (otherSite && followed != otherSites.end() && !followed->caller.empty())
						std::stable_partition(sites.begin(), sites.end(), [&](const CallSite& c) {
							return c.caller == followed->caller && c.ordinal == followed->ordinal;
						});
				}

				size_t tried = 0;
				for (const CallSite& site : sites)
				{
					const CallSite* pair = nullptr;
					if (merged)
					{
						for (const CallSite& c : otherSites)
							if (!site.caller.empty() && c.caller == site.caller && c.ordinal == site.ordinal)
								pair = &c;
						if (!pair)
							continue;
					}
					if (++tried > kMaxCallSites)
						break;

					const Stream own = DecodeRange(image, site.from, site.to);
					const size_t k = InstructionAt(own, site.from, site.site);
					if (k == SIZE_MAX)
						continue;

					size_t anchor = k;
					Stream both;
					Window w;
					if (merged)
					{
						const Stream theirs = DecodeRange(*other, pair->from, pair->to);
						const size_t kOther = InstructionAt(theirs, pair->from, pair->site);
						if (kOther == SIZE_MAX)
							continue;
						both = MergeAround(own, k, theirs, kOther, anchor);
						w = SmallestUnique(both, anchor, false, { &image, other });
					}
					else
						w = SmallestUnique(own, k, false, { &image });
					if (!w.found)
						continue;

					const Stream& used = merged ? both : own;
					out = Proposal{};
					out.pattern       = w.pattern;
					out.followRel32At = static_cast<int32_t>(used.Begin(anchor) - used.Begin(w.first));
					return true;
				}
				return false;
			}

			// Prefers the shape of the current request: a call window for a
			// follow request, a function start otherwise.
			bool Build(bool merged, bool follow, Proposal& out) const
			{
				return follow ? (Follow(merged, out) || Direct(merged, out))
				              : (Direct(merged, out) || Follow(merged, out));
			}
		};

		// The loader's own verdict, on each build the proposal is for.
		void Verify(const Session::State& s, const Request& r, const Located& L, Proposal& p)
		{
			Request shaped = r;
			shaped.resultOffset    = 0;
			shaped.followRel32At   = p.followRel32At;
			shaped.followRipDispAt = -1;
			const ScanValidation::Result t = ScanValidation::Resolve(AsRequest(shaped, p.pattern, s.target.module));
			p.uniqueOnTarget = t.Succeeded() && t.address == L.target;
			if (L.reference)
			{
				const ScanValidation::Result ref = ScanValidation::Resolve(AsRequest(shaped, p.pattern, s.reference.module));
				p.uniqueOnReference = ref.Succeeded() && ref.address == L.reference ? 1 : 0;
			}
		}
	}

	Result Session::Propose(const Request& r)
	{
		Result result;
		result.attempted = true;

		if (r.kind != ScanValidation::Kind::FunctionStart && r.kind != ScanValidation::Kind::Any)
		{
			result.note = std::string("proposals cover function-start patterns; this one is ") + ScanValidation::KindName(r.kind);
			return result;
		}
		if (r.followRipDispAt >= 0)
		{
			result.note = "proposals do not cover [rip+disp32] follows";
			return result;
		}
		if (r.patterns.empty())
		{
			result.note = "the request has no pattern";
			return result;
		}

		State& s = *state_;
		s.Load();

		const Located L = Locate(s, r);
		result.located         = L.method;
		result.confidence      = L.confidence;
		result.hookNameMatches = L.hookNameMatches;
		result.note            = L.note;
		result.referenceKeepsCurrent = L.referenceResolves;
		if (!L.target)
			return result;
		result.hasRva = true;
		result.rva    = L.target - s.target.base;
		if (L.reference)
		{
			result.hasReferenceRva = true;
			result.referenceRva    = L.reference - s.reference.base;
		}

		auto addNote = [&result](const std::string& text) {
			result.note += (result.note.empty() ? "" : "; ") + text;
		};
		const bool follow = r.followRel32At >= 0;
		const Builder target{ s.target, L.target, L.reference ? &s.reference : nullptr, L.reference, L.referenceSite };

		// One pattern for both builds; failing that, one for the scanned build.
		Proposal p;
		const bool found = (L.reference && target.Build(true, follow, p)) || target.Build(false, follow, p);
		if (!found)
		{
			addNote("no pattern of " + std::to_string(kMinPatternBytes) + " to " + std::to_string(kMaxPatternBytes)
				+ " bytes at the function or around a call to it is unique");
			return result;
		}
		Verify(s, r, L, p);
		result.proposals.push_back(p);

		// And one for the reference build, unless the current pattern still
		// serves it.
		if (L.reference && p.uniqueOnReference != 1 && !L.referenceResolves)
		{
			const Builder reference{ s.reference, L.reference };
			Proposal q;
			if (reference.Build(false, follow, q))
			{
				Verify(s, r, L, q);
				result.proposals.push_back(q);
			}
			else
				addNote("no pattern is unique on the reference build alone");
		}
		return result;
	}

	void PrintText(FILE* out, const Result& r)
	{
		if (!r.attempted)
			return;
		fprintf(out, "    suggest: located by %s%s%s", r.located, *r.confidence ? ", confidence " : "", r.confidence);
		if (r.hasRva)
			fprintf(out, ", RVA %s", Hex(r.rva).c_str());
		if (r.hasReferenceRva)
			fprintf(out, " (reference RVA %s)", Hex(r.referenceRva).c_str());
		fprintf(out, "\n");
		for (const Proposal& p : r.proposals)
		{
			const bool ref = p.uniqueOnReference == 1;
			const char* builds = p.uniqueOnTarget ? (ref ? "both builds" : "scanned build")
			                                      : (ref ? "reference build" : "verifies nowhere");
			fprintf(out, "      %s: %s", builds, p.pattern.c_str());
			if (p.followRel32At >= 0)
				fprintf(out, "  (follow rel32 at +0x%X)", p.followRel32At);
			else
				fprintf(out, "  (direct)");
			fprintf(out, "\n");
		}
		if (!r.proposals.empty() && r.proposals[0].uniqueOnReference != 1 && r.referenceKeepsCurrent)
			fprintf(out, "      reference build: the current pattern still resolves there\n");
		if (!r.note.empty())
			fprintf(out, "      note: %s\n", r.note.c_str());
	}

	std::string Json(const Result& r)
	{
		std::string out = "{ \"located\": \"" + std::string(r.located) + "\"";
		if (*r.confidence)
			out += ", \"confidence\": \"" + std::string(r.confidence) + "\"";
		if (r.hasRva)
			out += ", \"rva\": \"" + Hex(r.rva) + "\"";
		if (r.hasReferenceRva)
			out += ", \"referenceRva\": \"" + Hex(r.referenceRva) + "\"";
		if (r.hookNameMatches >= 0)
			out += std::string(", \"hookNameMatches\": ") + (r.hookNameMatches ? "true" : "false");
		out += ", \"proposals\": [";
		for (size_t i = 0; i < r.proposals.size(); ++i)
		{
			const Proposal& p = r.proposals[i];
			out += (i ? ", " : "") + std::string("{ \"pattern\": \"") + AobScan::JsonEscape(p.pattern) + "\"";
			out += ", \"followRel32At\": " + std::to_string(p.followRel32At) + ", \"resultOffset\": 0";
			out += std::string(", \"uniqueOnTarget\": ") + (p.uniqueOnTarget ? "true" : "false");
			if (p.uniqueOnReference >= 0)
				out += std::string(", \"uniqueOnReference\": ") + (p.uniqueOnReference ? "true" : "false");
			out += " }";
		}
		out += "]";
		if (r.referenceKeepsCurrent)
			out += ", \"referenceKeepsCurrent\": true";
		if (!r.note.empty())
			out += ", \"note\": \"" + AobScan::JsonEscape(r.note) + "\"";
		return out + " }";
	}
}
