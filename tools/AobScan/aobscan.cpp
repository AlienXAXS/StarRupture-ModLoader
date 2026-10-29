// sr-aobscan -- offline scan-pattern preflight for the game executable.
//
// Maps the exe from disk the way the Windows loader lays it out in memory
// (headers, sections at their virtual addresses, base relocations applied),
// registers its .pdata, and then runs every entry of
// ScanPatterns::PreflightRegistry through the loader's own
// ScanValidation::Resolve -- the same call PatternPreflight::VerifyAllPatterns
// makes in-game. Nothing is launched and nothing is attached to; the exe is
// only ever opened for reading.
//
//   sr-aobscan <game.exe> [--json] [--verbose]
//
// Exit code: 0 when every required pattern is OK, 1 when a required pattern is
// not, 2 when the file could not be read or is not an x64 PE image.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_validation.h"

namespace
{
	constexpr int kExitOk       = 0;
	constexpr int kExitFailed   = 1;
	constexpr int kExitBadInput = 2;

#if defined(MODLOADER_SERVER_BUILD)
	constexpr const char* kTarget = "server";
#else
	constexpr const char* kTarget = "client";
#endif

	struct MappedImage
	{
		uint8_t* base = nullptr;
		size_t   size = 0;
	};

	struct ScopedHandle
	{
		HANDLE h = INVALID_HANDLE_VALUE;
		~ScopedHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
	};

	bool ReadAt(HANDLE file, uint64_t offset, void* dest, size_t length)
	{
		LARGE_INTEGER pos;
		pos.QuadPart = static_cast<LONGLONG>(offset);
		if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN))
			return false;

		auto* out = static_cast<uint8_t*>(dest);
		while (length > 0)
		{
			const DWORD chunk = length > 0x10000000 ? 0x10000000 : static_cast<DWORD>(length);
			DWORD got = 0;
			if (!ReadFile(file, out, chunk, &got, nullptr) || got == 0)
				return false;
			out    += got;
			length -= got;
		}
		return true;
	}

	size_t AlignUp(size_t value, size_t alignment)
	{
		return alignment ? (value + alignment - 1) / alignment * alignment : value;
	}

	bool ApplyRelocations(MappedImage& image, IMAGE_NT_HEADERS64* nt, std::string& error)
	{
		const auto delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(image.base))
		                 - static_cast<int64_t>(nt->OptionalHeader.ImageBase);
		if (delta == 0)
			return true;

		const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
		if (dir.VirtualAddress == 0 || dir.Size == 0)
		{
			error = "the image is not at its preferred base and has no relocation table";
			return false;
		}

		size_t offset = 0;
		while (offset + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size)
		{
			const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image.base + dir.VirtualAddress + offset);
			if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
				break;

			const size_t entries = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
			const auto*  list    = reinterpret_cast<const uint16_t*>(block + 1);
			for (size_t i = 0; i < entries; ++i)
			{
				const unsigned type = list[i] >> 12;
				const size_t   rva  = block->VirtualAddress + (list[i] & 0x0FFF);
				if (type == IMAGE_REL_BASED_ABSOLUTE)
					continue;
				if (type != IMAGE_REL_BASED_DIR64 || rva + sizeof(int64_t) > image.size)
				{
					error = "unsupported base relocation type " + std::to_string(type);
					return false;
				}
				int64_t value;
				memcpy(&value, image.base + rva, sizeof(value));
				value += delta;
				memcpy(image.base + rva, &value, sizeof(value));
			}
			offset += block->SizeOfBlock;
		}

		// The OS loader records the base it actually used in the mapped header.
		nt->OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(image.base);
		return true;
	}

	// Lays the file out as the OS loader would. The file is only read, and
	// opened shared with writers so a running game does not matter.
	bool MapImage(const wchar_t* path, MappedImage& out, std::string& error)
	{
		ScopedHandle file;
		file.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file.h == INVALID_HANDLE_VALUE)
		{
			error = "cannot open the file (error " + std::to_string(GetLastError()) + ")";
			return false;
		}

		LARGE_INTEGER fileSize{};
		GetFileSizeEx(file.h, &fileSize);

		IMAGE_DOS_HEADER dos{};
		if (!ReadAt(file.h, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
		{
			error = "not a PE file (no MZ header)";
			return false;
		}

		IMAGE_NT_HEADERS64 ntFile{};
		if (!ReadAt(file.h, static_cast<uint64_t>(dos.e_lfanew), &ntFile, sizeof(ntFile))
		    || ntFile.Signature != IMAGE_NT_SIGNATURE
		    || ntFile.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
		    || ntFile.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			error = "not an x64 PE image";
			return false;
		}

		const size_t imageSize    = ntFile.OptionalHeader.SizeOfImage;
		const size_t headersSize  = ntFile.OptionalHeader.SizeOfHeaders;
		const size_t sectionAlign = ntFile.OptionalHeader.SectionAlignment;
		if (imageSize == 0 || headersSize == 0 || headersSize > imageSize)
		{
			error = "implausible SizeOfImage / SizeOfHeaders";
			return false;
		}

		// Commit zeroed pages for the whole image: the loader scans all of
		// SizeOfImage, so section padding and .bss-style tails have to read as
		// zero exactly as they do in a mapped module.
		auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (!base)
		{
			error = "cannot allocate " + std::to_string(imageSize) + " bytes for the image";
			return false;
		}

		MappedImage image{ base, imageSize };

		if (!ReadAt(file.h, 0, base, headersSize))
		{
			error = "cannot read the PE headers";
			VirtualFree(base, 0, MEM_RELEASE);
			return false;
		}

		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos.e_lfanew);
		const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			const IMAGE_SECTION_HEADER& s = sections[i];
			if (s.SizeOfRawData == 0)
				continue;

			// Raw bytes land at the section's RVA; whatever of the page-aligned
			// virtual extent the file does not cover stays zero.
			const size_t extent = AlignUp(s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData, sectionAlign);
			const size_t length = s.SizeOfRawData < extent ? s.SizeOfRawData : extent;

			if (static_cast<uint64_t>(s.VirtualAddress) + length > imageSize
			    || static_cast<uint64_t>(s.PointerToRawData) + length > static_cast<uint64_t>(fileSize.QuadPart)
			    || !ReadAt(file.h, s.PointerToRawData, base + s.VirtualAddress, length))
			{
				error = "cannot read section " + std::string(reinterpret_cast<const char*>(s.Name), strnlen(reinterpret_cast<const char*>(s.Name), 8));
				VirtualFree(base, 0, MEM_RELEASE);
				return false;
			}
		}

		if (!ApplyRelocations(image, nt, error))
		{
			VirtualFree(base, 0, MEM_RELEASE);
			return false;
		}

		// RtlLookupFunctionEntry is what the loader's function-start checks ask.
		// A mapped module is known to it already; this image is not, so register
		// its exception directory the way a JIT would.
		const IMAGE_DATA_DIRECTORY pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if (pdata.VirtualAddress && pdata.Size >= sizeof(RUNTIME_FUNCTION))
		{
			const DWORD count = pdata.Size / sizeof(RUNTIME_FUNCTION);
			if (!RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(base + pdata.VirtualAddress), count,
			                         reinterpret_cast<DWORD64>(base)))
			{
				error = "RtlAddFunctionTable failed";
				VirtualFree(base, 0, MEM_RELEASE);
				return false;
			}
		}

		out = image;
		return true;
	}

	const char* ShortKind(ScanPatterns::PatternKind kind)
	{
		switch (kind)
		{
		case ScanPatterns::PatternKind::FunctionStart: return "function";
		case ScanPatterns::PatternKind::InFunction:    return "infunction";
		case ScanPatterns::PatternKind::Code:          return "code";
		case ScanPatterns::PatternKind::Data:          return "data";
		case ScanPatterns::PatternKind::VTable:        return "vtable";
		case ScanPatterns::PatternKind::Any:           return "any";
		}
		return "?";
	}

	const char* ResultName(ScanValidation::Outcome outcome)
	{
		switch (outcome)
		{
		case ScanValidation::Outcome::Ok:              return "OK";
		case ScanValidation::Outcome::NoMatch:         return "MISSING";
		case ScanValidation::Outcome::MultipleMatches: return "AMBIGUOUS";
		case ScanValidation::Outcome::WrongKind:       return "WRONGKIND";
		case ScanValidation::Outcome::BadRequest:      return "BADREQUEST";
		}
		return "?";
	}

	struct Row
	{
		const ScanPatterns::PreflightEntry* entry = nullptr;
		ScanValidation::Result              result;
		bool                                hasRva = false;
		uint64_t                            rva    = 0;
	};

	std::string JsonEscape(const std::string& s)
	{
		std::string out;
		for (const unsigned char c : s)
		{
			switch (c)
			{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					sprintf_s(buf, "\\u%04x", c);
					out += buf;
				}
				else
					out += static_cast<char>(c);
			}
		}
		return out;
	}

	void PrintDetail(const std::string& detail)
	{
		size_t start = 0;
		while (start <= detail.size())
		{
			const size_t end = detail.find("\r\n", start);
			const std::string line = detail.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!line.empty())
				printf("    %s\n", line.c_str());
			if (end == std::string::npos)
				break;
			start = end + 2;
		}
	}
}

int wmain(int argc, wchar_t** argv)
{
	const wchar_t* path    = nullptr;
	bool           json    = false;
	bool           verbose = false;

	for (int i = 1; i < argc; ++i)
	{
		if (wcscmp(argv[i], L"--json") == 0)         json = true;
		else if (wcscmp(argv[i], L"--verbose") == 0) verbose = true;
		else if (!path && argv[i][0] != L'-')        path = argv[i];
		else
		{
			fprintf(stderr, "usage: sr-aobscan <game.exe> [--json] [--verbose]\n");
			return kExitBadInput;
		}
	}
	if (!path)
	{
		fprintf(stderr, "usage: sr-aobscan <game.exe> [--json] [--verbose]\n");
		return kExitBadInput;
	}

	MappedImage image;
	std::string error;
	if (!MapImage(path, image, error))
	{
		fprintf(stderr, "sr-aobscan: %ls: %s\n", path, error.c_str());
		return kExitBadInput;
	}

	const auto module = reinterpret_cast<HMODULE>(image.base);
	const auto base   = reinterpret_cast<uintptr_t>(image.base);

	std::vector<Row> rows;
	for (const ScanPatterns::PreflightEntry& entry : ScanPatterns::PreflightRegistry)
	{
		// The same request PatternPreflight builds, aimed at the mapped image
		// instead of the process's own main module.
		ScanValidation::Request request;
		request.name          = entry.name;
		request.pattern       = entry.pattern;
		request.kind          = static_cast<ScanValidation::Kind>(static_cast<int>(entry.kind));
		request.followRel32At = entry.followRel32At;
		request.module        = module;

		Row row;
		row.entry  = &entry;
		row.result = ScanValidation::Resolve(request);

		if (row.result.Succeeded())
		{
			row.hasRva = true;
			row.rva    = row.result.address - base;
		}
		else if (row.result.rawMatch)
		{
			row.hasRva = true;
			row.rva    = row.result.rawMatch - base;
		}
		rows.push_back(std::move(row));
	}

	size_t ok = 0, missing = 0, ambiguous = 0, other = 0, requiredFailing = 0;
	for (const Row& row : rows)
	{
		switch (row.result.outcome)
		{
		case ScanValidation::Outcome::Ok:              ++ok; break;
		case ScanValidation::Outcome::NoMatch:         ++missing; break;
		case ScanValidation::Outcome::MultipleMatches: ++ambiguous; break;
		default:                                       ++other; break;
		}
		if (!row.result.Succeeded() && row.entry->required)
			++requiredFailing;
	}

	if (json)
	{
		printf("{\n  \"target\": \"%s\",\n  \"imageSize\": %zu,\n", kTarget, image.size);
		printf("  \"summary\": { \"total\": %zu, \"ok\": %zu, \"missing\": %zu, \"ambiguous\": %zu, "
		       "\"other\": %zu, \"requiredFailing\": %zu },\n  \"patterns\": [\n",
			rows.size(), ok, missing, ambiguous, other, requiredFailing);
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const Row& row = rows[i];
			printf("    { \"name\": \"%s\", \"kind\": \"%s\", \"required\": %s, \"result\": \"%s\", \"matches\": %zu",
				JsonEscape(row.entry->name).c_str(), ShortKind(row.entry->kind),
				row.entry->required ? "true" : "false", ResultName(row.result.outcome), row.result.matchCount);
			if (row.hasRva)
				printf(", \"rva\": \"0x%llX\"", static_cast<unsigned long long>(row.rva));
			if (!row.result.Succeeded())
				printf(", \"detail\": \"%s\"", JsonEscape(row.result.detail).c_str());
			printf(" }%s\n", i + 1 < rows.size() ? "," : "");
		}
		printf("  ]\n}\n");
	}
	else
	{
		printf("sr-aobscan (%s build): %ls\n", kTarget, path);
		printf("%-10s %-52s %-10s %-4s %7s  %s\n", "RESULT", "NAME", "KIND", "REQ", "MATCHES", "RVA");
		for (const Row& row : rows)
		{
			char rva[24] = "-";
			if (row.hasRva)
				sprintf_s(rva, "0x%llX", static_cast<unsigned long long>(row.rva));
			printf("%-10s %-52s %-10s %-4s %7zu  %s\n", ResultName(row.result.outcome), row.entry->name,
				ShortKind(row.entry->kind), row.entry->required ? "yes" : "no", row.result.matchCount, rva);
			if (!row.result.Succeeded() && (verbose || row.entry->required))
				PrintDetail(row.result.detail);
		}
		printf("%zu patterns: %zu OK, %zu MISSING, %zu AMBIGUOUS, %zu other; %zu required failing\n",
			rows.size(), ok, missing, ambiguous, other, requiredFailing);
	}

	return requiredFailing == 0 ? kExitOk : kExitFailed;
}
