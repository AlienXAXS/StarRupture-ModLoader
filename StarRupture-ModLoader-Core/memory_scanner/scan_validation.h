#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// ScanValidation -- the single place that turns an AOB into an address, and the
// only one allowed to say a resolve succeeded.
//
// Two rules, and both of them are absolute:
//
//   1. A pattern must match EXACTLY ONCE in the whole image. Two matches is a
//      failure, not a coin flip. The old first-match-wins behaviour meant a
//      pattern that stopped being unique after a game update silently resolved
//      to whichever copy sat at the lower address -- and then got memoized into
//      scan_cache.ini, so every later launch went straight to the wrong address
//      with no scan and no symptom.
//
//   2. The match must BE the thing its author declared it to be. A pattern that
//      is supposed to land on a function entry and lands 0x37 bytes inside some
//      unrelated function, or in a string table, is a failure -- being told
//      "your pattern matched, here is a wrong address" is worse than being told
//      nothing, because a detour gets written over it.
//
// Note what rule 1 rules out: the scan CANNOT be narrowed to executable
// sections for a code pattern. Narrowing would make a .rdata match invisible
// rather than fatal, which is exactly the failure rule 2 exists to catch. So
// the whole image is always scanned and every match is classified. That is why
// Scanner's inner loop grew a memchr anchor and why ScanCache grew a match
// count -- without those two, being this strict would cost a full unaccelerated
// pass per pattern on every launch.
//
// Kind checks are applied to the FINAL address (after resultOffset and
// followRel32At), while uniqueness applies to the raw pattern matches. One
// resolve, two independent verdicts -- the failure detail always says which of
// them failed, because "it did not resolve" is not actionable on its own.
// ---------------------------------------------------------------------------

namespace ScanValidation
{
	// Mirrors PluginScanKind in plugins/plugin_interface.h. Kept as a separate
	// enum so the loader's own preflight registry does not have to include the
	// plugin ABI header, and so a change to one is a deliberate change to both.
	enum class Kind
	{
		Unspecified = 0,  // refused: the caller has to say what it is scanning for
		FunctionStart,    // primary entry of a function, per .pdata, long enough to hook
		InFunction,       // anywhere inside a function that has unwind info
		Code,             // any executable section
		Data,             // initialised, non-executable section
		VTable,           // data, and the first `vtableSlots` slots point at function starts
		Any,              // no structural check -- uniqueness only
	};

	struct Request
	{
		std::string name;              // what is being resolved, for the report
		std::string pattern;           // IDA-style AOB
		Kind        kind          = Kind::Unspecified;
		HMODULE     module        = nullptr;  // null = the game exe (main module)
		int32_t     resultOffset  = 0;        // added to the match before the kind check
		int32_t     followRel32At = -1;       // >= 0: decode an E8/E9 rel32 here and use its target
		uint32_t    vtableSlots   = 0;        // VTable only; 0 means 1
	};

	enum class Outcome
	{
		Ok = 0,
		BadRequest,       // empty name/pattern, unparseable pattern, no kind declared
		NoMatch,          // the pattern is not in the image at all
		MultipleMatches,  // matched more than once
		WrongKind,        // matched once, but the address is not what was declared
	};

	struct Result
	{
		Outcome     outcome    = Outcome::BadRequest;
		uintptr_t   address    = 0;   // final address, meaningful only when Ok
		uintptr_t   rawMatch   = 0;   // the pattern match itself, before offset/rel32
		size_t      matchCount = 0;
		std::string detail;           // CRLF-separated, ready to drop into a report

		bool Succeeded() const { return outcome == Outcome::Ok; }
	};

	// Resolves and validates. Never throws; a malformed request comes back as
	// BadRequest with a detail line saying what was wrong with it.
	//
	// Cost: one full image pass on a cold cache, stopping at the second match.
	// The expensive full enumeration (every match, classified, symbolised) runs
	// only when the resolve has already failed -- that launch is broken anyway,
	// and the report is the only thing that will fix it.
	Result Resolve(const Request& request);

	// "function start", "code", ... -- for log lines and report text.
	const char* KindName(Kind kind);

	// Parses a kind from its short name ("function", "code", "data", "vtable",
	// "infunction", "any"). Returns Unspecified for anything it does not know.
	Kind ParseKind(const char* name);

	// One line describing an address: section, containing function, symbol name
	// when a PDB happens to be available. Exposed because the preload manager
	// and the console both want to describe an address the same way the failure
	// report does.
	std::string DescribeAddress(HMODULE module, uintptr_t address);
}
