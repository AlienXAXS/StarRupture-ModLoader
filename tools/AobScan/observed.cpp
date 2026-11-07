#include "observed.h"

#include "hooks/game/version_map.generated.h"

#include <algorithm>
#include <vector>

namespace AobScan
{
	namespace
	{
		bool LandsOn(const ScanValidation::Request& request, uintptr_t address)
		{
			const ScanValidation::Result r = ScanValidation::Resolve(request);
			return r.Succeeded() && r.address == address;
		}

		// Whether a global absent row's owner list ("A,B") names the loader.
		bool ListsLoader(const std::string& owners)
		{
			size_t start = 0;
			while (start < owners.size())
			{
				const size_t comma = owners.find(',', start);
				const size_t end   = comma == std::string::npos ? owners.size() : comma;
				if (owners.compare(start, end - start, "ModLoader") == 0)
					return true;
				start = end + 1;
			}
			return false;
		}

		struct Locator
		{
			uint32_t    gameCL = 0;
			bool        fromMap = false;   // the row's own pattern, not the registry's standing in for it
			std::string pattern;
			int32_t     resultOffset    = 0;
			int32_t     followRel32At   = -1;
			int32_t     followRipDispAt = -1;
		};

		// The map's rows the loader decides a registry row with, in the map's
		// order (VersionMap::LoaderRows over LookUp, which the loader keeps
		// private): the rows of its key, kind and fingerprint for the running
		// build and for the latest other build that has a pattern, none when
		// the key is absent on a build newer than that one or absent on the
		// running build. On the build the code was written for the registry's
		// pattern stands in for the row's, and a site with arithmetic keeps
		// only a row with its own pattern. Empty also when the loader refuses,
		// which then reports no address to observe.
		std::vector<Locator> LoaderRows(const ScanPatterns::PreflightEntry& entry, uint32_t runningCL)
		{
			const std::string key  = VersionMap::LoaderEntryKey(entry);
			const uint32_t    fp   = VersionMap::LoaderEntryFingerprint(entry);
			const int         kind = static_cast<int>(entry.kind);

			struct Row { uint32_t gameCL; const VersionMap::MapFunctionRow* row; };
			std::vector<Row> rows;
			uint32_t absentCL = 0;
			for (const VersionMap::MapFunctionRow& f : VersionMap::ActiveMap().functions)
			{
				if (f.key != key || f.kind != kind || f.fp != fp)
					continue;
				if (f.absent)
				{
					const bool binds = fp != 0 || ListsLoader(f.owners);
					if (f.gameCL == runningCL)
						return {};   // refused, or set aside for other owners: no row is used
					if (binds)
						absentCL = std::max(absentCL, f.gameCL);
					continue;
				}
				rows.push_back({ f.gameCL, &f });
			}

			uint32_t latestCL = 0;
			for (const Row& r : rows)
				if (r.gameCL != runningCL && !r.row->pattern.empty() && r.gameCL > latestCL)
					latestCL = r.gameCL;
			if (absentCL > latestCL)
				latestCL = 0;

			std::vector<Locator> out;
			for (const Row& r : rows)
			{
				if (r.gameCL != runningCL && r.gameCL != latestCL)
					continue;
				Locator l;
				l.gameCL = r.gameCL;
				if (r.gameCL == VersionMap::kBuiltCL)
				{
					l.pattern         = entry.pattern;
					l.followRel32At   = entry.followRel32At;
					l.followRipDispAt = entry.followRipDispAt;
				}
				else
				{
					if (entry.siteArithmetic && r.row->pattern != entry.pattern)
						continue;
					l.fromMap         = true;
					l.pattern         = r.row->pattern;
					l.resultOffset    = r.row->resultOffset;
					l.followRel32At   = r.row->followRel32At;
					l.followRipDispAt = r.row->followRipDispAt;
				}
				out.push_back(std::move(l));
			}
			return out;
		}

		bool Take(const Locator& l, const ScanValidation::Request& request, uintptr_t address, ObservedLocator& seen)
		{
			if (l.pattern.empty())
				return false;
			ScanValidation::Request asked = request;
			asked.pattern         = l.pattern;
			asked.resultOffset    = l.resultOffset;
			asked.followRel32At   = l.followRel32At;
			asked.followRipDispAt = l.followRipDispAt;
			if (!LandsOn(asked, address))
				return false;
			seen.found           = true;
			seen.fromMap         = l.fromMap;
			seen.pattern         = l.pattern;
			seen.resultOffset    = l.resultOffset;
			seen.followRel32At   = l.followRel32At;
			seen.followRipDispAt = l.followRipDispAt;
			return true;
		}
	}

	ObservedLocator ObserveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address)
	{
		ObservedLocator seen;
		const std::vector<Locator> rows = LoaderRows(entry, exe.cl);

		// VersionMap::Select: the row for the running build decides first; then
		// every locator that resolves agrees, the request's own pattern first and
		// the rows after it in the map's order, and the first of them won.
		for (const Locator& l : rows)
			if (exe.cl && l.gameCL == exe.cl && Take(l, request, address, seen))
				return seen;

		Locator own;
		own.pattern         = request.pattern;
		own.resultOffset    = request.resultOffset;
		own.followRel32At   = request.followRel32At;
		own.followRipDispAt = request.followRipDispAt;
		if (Take(own, request, address, seen))
			return seen;

		for (const Locator& l : rows)
			if (l.gameCL != exe.cl && Take(l, request, address, seen))
				return seen;
		return seen;
	}
}
