#include "memory_scanner/image_info.h"

#include <cstring>

namespace
{
	// UNWIND_INFO's first byte packs Version in the low 3 bits and Flags in the
	// high 5. UNW_FLAG_CHAININFO (0x4) means this entry is a separated chunk and
	// the RUNTIME_FUNCTION of its parent follows the unwind codes.
	constexpr uint8_t kUnwFlagChainInfo = 0x4;

	// Follow at most this many chain links. A well-formed image chains once;
	// the cap is there so a corrupt or hand-edited .pdata cannot spin forever.
	constexpr int kMaxChainDepth = 8;

	struct UnwindInfoHeader
	{
		uint8_t versionAndFlags;
		uint8_t sizeOfProlog;
		uint8_t countOfCodes;
		uint8_t frameRegisterAndOffset;
	};

	const IMAGE_NT_HEADERS* GetNtHeaders(uintptr_t base)
	{
		if (!base)
			return nullptr;

		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return nullptr;

		return nt;
	}
}

namespace Scanner
{
	ImageInfo GetImageInfo(HMODULE module)
	{
		ImageInfo info;

		const auto base = reinterpret_cast<uintptr_t>(module);
		const IMAGE_NT_HEADERS* nt = GetNtHeaders(base);
		if (!nt)
			return info;

		info.valid = true;
		info.base  = base;
		info.size  = nt->OptionalHeader.SizeOfImage;
		return info;
	}

	SectionInfo GetSectionForAddress(HMODULE module, uintptr_t addr)
	{
		SectionInfo out;

		const auto base = reinterpret_cast<uintptr_t>(module);
		const IMAGE_NT_HEADERS* nt = GetNtHeaders(base);
		if (!nt)
			return out;

		if (addr < base || addr >= base + nt->OptionalHeader.SizeOfImage)
			return out;

		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			const uintptr_t start = base + section[i].VirtualAddress;

			// VirtualSize is the meaningful extent; SizeOfRawData can be larger
			// (file alignment padding) or smaller (.bss-style tail).
			size_t size = section[i].Misc.VirtualSize;
			if (size == 0)
				size = section[i].SizeOfRawData;

			if (addr < start || addr >= start + size)
				continue;

			memcpy(out.name, section[i].Name, 8);
			out.name[8]      = '\0';
			out.valid        = true;
			out.start        = start;
			out.size         = size;
			out.executable   = (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
			out.writable     = (section[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
			out.initialized  = (section[i].Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0
			                || (section[i].Characteristics & IMAGE_SCN_CNT_CODE) != 0;
			return out;
		}

		return out;
	}

	std::string DescribeSection(HMODULE module, uintptr_t addr)
	{
		const SectionInfo s = GetSectionForAddress(module, addr);
		if (!s.valid)
			return "<outside image>";
		return s.name;
	}

	FunctionInfo DescribeFunction(uintptr_t addr)
	{
		FunctionInfo out;
		if (!addr)
			return out;

		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(addr), &imageBase, nullptr);
		if (!rf || !imageBase)
			return out;

		out.valid = true;
		out.start = static_cast<uintptr_t>(imageBase) + rf->BeginAddress;
		out.end   = static_cast<uintptr_t>(imageBase) + rf->EndAddress;

		// Walk the chain to the primary entry. RtlLookupFunctionEntry hands back
		// the chunk that covers the address, not the function it belongs to, so
		// without this a cold chunk reads as a function of its own -- and its
		// first byte would pass a "function start" check while being a place no
		// caller ever enters.
		PRUNTIME_FUNCTION current = rf;
		for (int depth = 0; depth < kMaxChainDepth; ++depth)
		{
			const auto* unwind = reinterpret_cast<const UnwindInfoHeader*>(
				static_cast<uintptr_t>(imageBase) + current->UnwindInfoAddress);

			const uint8_t flags = static_cast<uint8_t>(unwind->versionAndFlags >> 3);
			if ((flags & kUnwFlagChainInfo) == 0)
				break;

			out.chainedChunk = true;

			// The parent RUNTIME_FUNCTION follows the unwind code array, which is
			// padded to an even count of 2-byte slots.
			const size_t codeSlots = (unwind->countOfCodes + 1) & ~static_cast<size_t>(1);
			const uintptr_t parent = reinterpret_cast<uintptr_t>(unwind)
			                       + sizeof(UnwindInfoHeader)
			                       + codeSlots * 2;

			current   = reinterpret_cast<PRUNTIME_FUNCTION>(parent);
			out.start = static_cast<uintptr_t>(imageBase) + current->BeginAddress;
		}

		return out;
	}

	bool IsFunctionStart(uintptr_t addr)
	{
		const FunctionInfo fn = DescribeFunction(addr);
		return fn.valid && !fn.chainedChunk && fn.start == addr;
	}

	size_t GetFunctionLength(uintptr_t addr)
	{
		const FunctionInfo fn = DescribeFunction(addr);
		if (!fn.valid || fn.end <= addr)
			return 0;
		return static_cast<size_t>(fn.end - addr);
	}
}
