#include "observed.h"

#include "hooks/game/version_map.generated.h"

namespace AobScan
{
	namespace
	{
		bool LandsOn(const ScanValidation::Request& request, uintptr_t address)
		{
			const ScanValidation::Result r = ScanValidation::Resolve(request);
			return r.Succeeded() && r.address == address;
		}
	}

	ObservedLocator ObserveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address)
	{
		ObservedLocator seen;

		if (exe.cl != VersionMap::kBuiltCL)
		{
			const std::string key = VersionMap::LoaderEntryKey(entry);
			const uint32_t    fp  = VersionMap::LoaderEntryFingerprint(entry);
			for (const VersionMap::MapFunctionRow& row : VersionMap::ActiveMap().functions)
			{
				if (row.key != key || row.fp != fp || row.kind != static_cast<int>(entry.kind)
					|| row.gameCL != exe.cl || row.absent || row.pattern.empty())
					continue;
				ScanValidation::Request asked = request;
				asked.pattern         = row.pattern;
				asked.resultOffset    = row.resultOffset;
				asked.followRel32At   = row.followRel32At;
				asked.followRipDispAt = row.followRipDispAt;
				if (LandsOn(asked, address))
				{
					seen.found           = true;
					seen.fromMap         = true;
					seen.pattern         = row.pattern;
					seen.resultOffset    = row.resultOffset;
					seen.followRel32At   = row.followRel32At;
					seen.followRipDispAt = row.followRipDispAt;
					return seen;
				}
			}
		}

		if (LandsOn(request, address))
		{
			seen.found           = true;
			seen.pattern         = request.pattern;
			seen.resultOffset    = request.resultOffset;
			seen.followRel32At   = request.followRel32At;
			seen.followRipDispAt = request.followRipDispAt;
		}
		return seen;
	}
}
