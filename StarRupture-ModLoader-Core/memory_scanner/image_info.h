#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Structural facts about a loaded PE image, used to decide whether a pattern
// match is the thing its author said it was.
//
// Two independent sources, neither of which needs symbols:
//
//   * The section table -- is this address in an executable section, in
//     read-only data, in writable data, or outside the image entirely.
//
//   * The x64 exception directory (.pdata) -- one RUNTIME_FUNCTION per compiled
//     function, which makes "is this address the start of a function" an exact
//     table lookup rather than a prologue heuristic, and "how far into a
//     function did I land" free with it. Reached through RtlLookupFunctionEntry
//     so the OS does the binary search (the same call the stack sampler uses).
//
// Everything here reads read-only mapped PE data. No allocation on the hot
// path, safe to call from Stage 1 with the game main thread parked.
// ---------------------------------------------------------------------------

namespace Scanner
{
	struct ImageInfo
	{
		bool      valid = false;
		uintptr_t base  = 0;
		size_t    size  = 0;   // SizeOfImage
	};

	// Validates the DOS/NT headers before reporting anything. An invalid image
	// yields valid == false rather than a crash.
	ImageInfo GetImageInfo(HMODULE module);

	struct SectionInfo
	{
		bool      valid       = false;
		char      name[9]     = {};  // ".text", ".rdata", ... (NUL-terminated)
		uintptr_t start       = 0;
		size_t    size        = 0;   // Misc.VirtualSize
		bool      executable  = false;
		bool      writable    = false;
		bool      initialized = false;
	};

	// The section containing addr, or a default-constructed (invalid) one when
	// addr is outside the image or in the header padding before the first
	// section.
	SectionInfo GetSectionForAddress(HMODULE module, uintptr_t addr);

	// ".text" / ".rdata" / "<outside image>" -- for failure report lines, where
	// naming the section is what tells an author their "code" pattern matched a
	// string table.
	std::string DescribeSection(HMODULE module, uintptr_t addr);

	struct FunctionInfo
	{
		bool      valid        = false; // false = no unwind entry covers addr
		uintptr_t start        = 0;     // primary entry, chain info followed
		uintptr_t end          = 0;     // exclusive, end of the contiguous span
		bool      chainedChunk = false; // addr sits outside [start, end)
	};

	// Resolves addr through the exception directory to the function it belongs
	// to: `start` is the primary entry with UNW_FLAG_CHAININFO followed, and
	// `end` is the end of the last RUNTIME_FUNCTION contiguous with it that
	// chains back to it.
	//
	// That span matters because MSVC splits one function's unwind data across
	// several adjacent entries, the first of which is often just the prolog --
	// four bytes is common. [start, end) is the whole function; one chunk is not.
	//
	// chainedChunk means addr fell OUTSIDE that span: it is in a chunk the
	// compiler moved elsewhere, which no caller ever enters. An ordinary
	// mid-function address is not chained, however many entries the function's
	// unwind data happens to use.
	//
	// valid == false means no unwind entry covers the address at all -- leaf
	// thunks and hand-written assembly stubs legitimately have none, so this is
	// "unknown", not "not a function".
	FunctionInfo DescribeFunction(uintptr_t addr);

	// True only when addr is the primary entry point of a function: an exact
	// RUNTIME_FUNCTION BeginAddress match that is not a chained chunk. A cold
	// chunk's first byte answers false -- it is a real code address, but
	// detouring it is not what anyone means by hooking a function.
	bool IsFunctionStart(uintptr_t addr);

	// Bytes from addr to the end of its function, or 0 when nothing covers it.
	// Hooks::Hook writes a 14-byte absolute JMP, so a shorter function cannot
	// be detoured without overwriting whatever follows it.
	size_t GetFunctionLength(uintptr_t addr);
}
