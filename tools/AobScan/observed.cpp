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

		// Whether a global absent row's owner list ("A,B") names `owner`.
		bool ListsOwner(const std::string& owners, const std::string& owner)
		{
			size_t start = 0;
			while (start < owners.size())
			{
				const size_t comma = owners.find(',', start);
				const size_t end   = comma == std::string::npos ? owners.size() : comma;
				if (owners.compare(start, end - start, owner) == 0)
					return true;
				start = end + 1;
			}
			return false;
		}

		using Locator = MapLocator;

		// The map's rows the loader decides a request with, in the map's order
		// (VersionMap::LookUp, which the loader keeps private): the rows of its
		// key, kind and fingerprint for the running build and for the latest
		// other build that has a pattern, none when the key is absent on a
		// build newer than that one or absent on the running build. A global
		// key's absent row binds the owners it lists, a site's its fingerprint.
		// Empty also when the loader refuses, which then reports no address to
		// observe.
		std::vector<Locator> MapRows(const std::string& key, int kind, uint32_t fp, const std::string& owner, uint32_t runningCL,
			std::string* why = nullptr)
		{
			struct Row { uint32_t gameCL; const VersionMap::MapFunctionRow* row; };
			std::vector<Row> rows;
			uint32_t absentCL = 0;
			for (const VersionMap::MapFunctionRow& f : VersionMap::ActiveMap().functions)
			{
				if (f.key != key || f.kind != kind || f.fp != fp)
					continue;
				if (f.absent)
				{
					if (f.gameCL == runningCL)
						return {};   // refused, or set aside for other owners: no row is used
					if (fp != 0 || ListsOwner(f.owners, owner))
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
			{
				if (why && latestCL)
					*why = "absent on CL-" + std::to_string(absentCL) + ", newer than its latest row";
				latestCL = 0;
			}

			std::vector<Locator> out;
			for (const Row& r : rows)
			{
				if (r.gameCL != runningCL && r.gameCL != latestCL)
					continue;
				Locator l;
				l.gameCL          = r.gameCL;
				l.fromMap         = true;
				l.pattern         = r.row->pattern;
				l.resultOffset    = r.row->resultOffset;
				l.followRel32At   = r.row->followRel32At;
				l.followRipDispAt = r.row->followRipDispAt;
				out.push_back(std::move(l));
			}
			if (why && out.empty() && why->empty())
				*why = "no row of the map carries a pattern";
			return out;
		}

		// The map's rows for one registry row (VersionMap::LoaderRows). On the
		// build the code was written for the registry's pattern stands in for
		// the row's, and a site with arithmetic keeps only a row with its own
		// pattern.
		std::vector<Locator> LoaderRows(const ScanPatterns::PreflightEntry& entry, uint32_t runningCL)
		{
			std::vector<Locator> out;
			for (Locator& l : MapRows(VersionMap::LoaderEntryKey(entry), static_cast<int>(entry.kind),
				VersionMap::LoaderEntryFingerprint(entry), "ModLoader", runningCL))
			{
				if (l.gameCL == VersionMap::kBuiltCL)
				{
					l.fromMap         = false;
					l.pattern         = entry.pattern;
					l.resultOffset    = 0;
					l.followRel32At   = entry.followRel32At;
					l.followRipDispAt = entry.followRipDispAt;
				}
				else if (entry.siteArithmetic && l.pattern != entry.pattern)
					continue;
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

		// VersionMap::Select: the row for the running build decides first; then
		// every locator that resolves agrees, the request's own pattern first and
		// the rows after it in the map's order, and the first of them won.
		ObservedLocator Pick(const std::vector<Locator>& rows, const ScanValidation::Request& request,
			const VersionMap::Exe& exe, uintptr_t address)
		{
			ObservedLocator seen;
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

	std::vector<MapLocator> MapLocators(const std::string& key, int kind, uint32_t fp, const std::string& owner,
		uint32_t runningCL, std::string* why)
	{
		return MapRows(key, kind, fp, owner, runningCL, why);
	}

	ObservedLocator ObserveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address)
	{
		return Pick(LoaderRows(entry, exe.cl), request, exe, address);
	}

	ObservedLocator ObservePluginRequest(const char* owner, const std::string& key, uint32_t fp,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address)
	{
		return Pick(MapRows(key, static_cast<int>(request.kind), fp, owner ? owner : "", exe.cl), request, exe, address);
	}
}
