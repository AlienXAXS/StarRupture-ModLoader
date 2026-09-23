#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <vector>

#include "memory_scanner/scanner.h"

// ---------------------------------------------------------------------------
// PatchOverlay -- scanning the image as it SHIPPED, not as we have left it.
//
// An AOB comes from the binary on disk. That is what the author's disassembler
// showed them, and it is the only version of the bytes anyone outside this
// process has ever seen. By the time a plugin scans, though, the loader has
// stamped a 14-byte absolute JMP over the entry of UObject::ProcessEvent,
// AActor::BeginPlay, UGameEngine::Tick, FEngineLoop::Init and two dozen more --
// which is precisely the set of functions a mod wants.
//
// Scanning the live image therefore answers a different question than the one
// that was asked, and it goes wrong in both directions:
//
//   * A pattern anchored on a hooked function's entry stops matching. Best case
//     that is a clean "not found". Worst case the real site is masked, some
//     OTHER location becomes the only match, and the resolve succeeds on an
//     address that has nothing to do with the intent -- passing the uniqueness
//     check on the way past.
//
//   * A pattern containing FF 25 00 00 00 00 starts matching our own stubs,
//     inventing matches that do not exist in the shipped binary. With a second
//     match now fatal, that refuses a plugin for something the loader did.
//
// It is not an ordering problem and cannot be fixed by moving a phase: whoever
// scans second sees whatever the first one patched. Ordinary plugins have had
// this the longest, since OnPluginLoadHooks runs in Stage 3, after every loader
// hook is in.
//
// So the scanner reads through the patches. Hooks::Broker knows every address
// it has touched and the bytes that used to be there; this turns that into a
// scan whose results match the file on disk.
//
// Cost: nothing at all when no hooks are installed (the loader's own preflight,
// which is the expensive 60-pattern pass, runs before any of them). After that,
// one extra pass over a few hundred bytes per patched address.
// ---------------------------------------------------------------------------

namespace PatchOverlay
{
	// The patched regions as of construction. Build one per resolve and reuse it
	// across the calls below -- each construction takes the broker's lock.
	class Snapshot
	{
	public:
		Snapshot();

		// True when nothing is hooked, which is the fast path callers should
		// branch on: with no patches the overlay cannot change any answer, so
		// they can use Scanner directly and keep its early exits.
		bool Empty() const { return m_ranges.empty(); }

		// Does the pattern match at exactly this address, in the shipped image?
		// Used to re-validate a scan-cache hit, which would otherwise go stale
		// for every hooked function and force a full re-scan on every launch.
		bool MatchesAt(uintptr_t address, const std::vector<Scanner::PatternByte>& pattern) const;

		// Matches over [start, start+size), as the shipped image would have
		// them: matches that exist only because of a patch are dropped, and
		// matches a patch was hiding are added back.
		//
		// Returns the total found, storing at most maxResults of them and giving
		// up after hardCap so a degenerate pattern cannot run away.
		size_t Collect(uintptr_t start, size_t size,
		               const std::vector<Scanner::PatternByte>& pattern,
		               size_t maxResults, size_t hardCap,
		               std::vector<uintptr_t>& out) const;

	private:
		struct Range
		{
			uintptr_t            address = 0;
			std::vector<uint8_t> originalBytes;

			uintptr_t End() const { return address + originalBytes.size(); }
		};

		bool Overlaps(uintptr_t start, size_t length) const;

		// Copies [start, start+length) out of the live image and then puts every
		// overlapping patch's original bytes back over the top.
		void Reconstruct(uintptr_t start, size_t length, std::vector<uint8_t>& out) const;

		std::vector<Range> m_ranges;
	};
}
