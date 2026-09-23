#include "memory_scanner/patch_overlay.h"
#include "memory_scanner/image_info.h"
#include "hooks/hook_broker.h"

#include <algorithm>
#include <cstring>

namespace
{
	bool RangesIntersect(uintptr_t aStart, uintptr_t aEnd, uintptr_t bStart, uintptr_t bEnd)
	{
		return aStart < bEnd && bStart < aEnd;
	}

	bool MatchesBuffer(const uint8_t* candidate, const std::vector<Scanner::PatternByte>& pattern)
	{
		for (size_t i = 0; i < pattern.size(); ++i)
			if (!pattern[i].wildcard && candidate[i] != pattern[i].value)
				return false;
		return true;
	}
}

namespace PatchOverlay
{
	Snapshot::Snapshot()
	{
		for (const Hooks::Broker::PatchedRange& range : Hooks::Broker::GetPatchedRanges())
		{
			if (range.originalBytes.empty())
				continue;

			Range r;
			r.address       = range.address;
			r.originalBytes = range.originalBytes;
			m_ranges.push_back(std::move(r));
		}

		std::sort(m_ranges.begin(), m_ranges.end(),
			[](const Range& a, const Range& b) { return a.address < b.address; });
	}

	bool Snapshot::Overlaps(uintptr_t start, size_t length) const
	{
		const uintptr_t end = start + length;
		for (const Range& range : m_ranges)
		{
			if (range.address >= end)
				break;                       // sorted, nothing further can overlap
			if (RangesIntersect(start, end, range.address, range.End()))
				return true;
		}
		return false;
	}

	void Snapshot::Reconstruct(uintptr_t start, size_t length, std::vector<uint8_t>& out) const
	{
		out.resize(length);
		memcpy(out.data(), reinterpret_cast<const void*>(start), length);

		// Every patch touching this window, not just the one that prompted it --
		// two hooked functions can sit close enough together that one window
		// covers both, and reconstructing only one of them would leave the other
		// one's JMP stub in the buffer.
		const uintptr_t end = start + length;
		for (const Range& range : m_ranges)
		{
			if (range.address >= end)
				break;
			if (!RangesIntersect(start, end, range.address, range.End()))
				continue;

			const uintptr_t copyStart = (std::max)(start, range.address);
			const uintptr_t copyEnd   = (std::min)(end, range.End());

			memcpy(out.data() + (copyStart - start),
			       range.originalBytes.data() + (copyStart - range.address),
			       static_cast<size_t>(copyEnd - copyStart));
		}
	}

	bool Snapshot::MatchesAt(uintptr_t address, const std::vector<Scanner::PatternByte>& pattern) const
	{
		if (pattern.empty())
			return false;

		if (!Overlaps(address, pattern.size()))
			return MatchesBuffer(reinterpret_cast<const uint8_t*>(address), pattern);

		std::vector<uint8_t> buffer;
		Reconstruct(address, pattern.size(), buffer);
		return MatchesBuffer(buffer.data(), pattern);
	}

	size_t Snapshot::Collect(uintptr_t start, size_t size,
		const std::vector<Scanner::PatternByte>& pattern,
		size_t maxResults, size_t hardCap,
		std::vector<uintptr_t>& out) const
	{
		out.clear();

		const size_t patLen = pattern.size();
		if (patLen == 0 || size < patLen)
			return 0;

		// Nothing is hooked: the live image IS the shipped image, so hand straight
		// back to the scanner and keep its memchr fast path untouched. This is the
		// case during the loader's own preflight, which is the pass that actually
		// costs something.
		if (m_ranges.empty())
			return Scanner::CollectMatches(start, size, pattern, maxResults, hardCap, out);

		std::vector<uintptr_t> found;

		// Pass 1: the live image, minus anything a patch touches. A match
		// overlapping a patch is not trustworthy either way -- it may be an
		// artefact of our own JMP stub, or it may be genuine and merely
		// coincidental -- so it is dropped here and re-decided in pass 2, where
		// the bytes are right.
		std::vector<uintptr_t> live;
		Scanner::CollectMatches(start, size, pattern, hardCap, hardCap, live);
		for (uintptr_t address : live)
		{
			if (!Overlaps(address, patLen))
				found.push_back(address);
		}

		// Pass 2: a reconstructed window around each patch, wide enough to catch
		// every match that could overlap it -- a match may begin up to patLen-1
		// bytes before the patch and still run into it.
		const uintptr_t regionEnd = start + size;
		std::vector<uint8_t> buffer;

		for (const Range& range : m_ranges)
		{
			if (range.End() <= start || range.address >= regionEnd)
				continue;

			const uintptr_t windowStart = (std::max)(start,
				range.address > (patLen - 1) ? range.address - (patLen - 1) : start);
			const uintptr_t windowEnd = (std::min)(regionEnd, range.End() + (patLen - 1));

			if (windowEnd <= windowStart || (windowEnd - windowStart) < patLen)
				continue;

			Reconstruct(windowStart, static_cast<size_t>(windowEnd - windowStart), buffer);

			const size_t last = buffer.size() - patLen;
			for (size_t i = 0; i <= last; ++i)
			{
				if (!MatchesBuffer(buffer.data() + i, pattern))
					continue;

				const uintptr_t address = windowStart + i;

				// Only matches that actually touch this patch belong here.
				// Anything else in the window is unaffected by it and was already
				// counted in pass 1; taking it again would double-count.
				if (RangesIntersect(address, address + patLen, range.address, range.End()))
					found.push_back(address);
			}
		}

		// A match can straddle two patches and be found in both windows.
		std::sort(found.begin(), found.end());
		found.erase(std::unique(found.begin(), found.end()), found.end());

		for (uintptr_t address : found)
		{
			if (out.size() < maxResults)
				out.push_back(address);
		}

		return found.size();
	}
}
