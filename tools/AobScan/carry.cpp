#include "carry.h"

#include "observed.h"
#include "plugin_scan.h"

#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_validation.h"

#include <cstdio>
#include <set>
#include <tuple>

namespace AobScan
{
	namespace
	{
		using Group = std::tuple<std::string, int32_t, uint32_t>;   // key, kind, fp

		const char* ResultName(CarryResult result)
		{
			switch (result)
			{
			case CarryResult::Match:     return "match";
			case CarryResult::Ambiguous: return "ambiguous";
			default:                     return "none";
			}
		}

		std::string Hex8(uint32_t value)
		{
			char buf[16];
			sprintf_s(buf, "0x%08X", value);
			return buf;
		}

		// The owners the key's absent rows list on builds newer than `fromCL`:
		// the loader withdraws the tried row for them (VersionMap::LookUp).
		std::string BoundOwners(const Group& group, uint32_t fromCL)
		{
			std::string owners;
			for (const VersionMap::MapFunctionRow& f : VersionMap::ActiveMap().functions)
				if (f.absent && f.key == std::get<0>(group) && f.kind == std::get<1>(group) && f.fp == std::get<2>(group)
					&& f.gameCL > fromCL && !f.owners.empty())
					owners += (owners.empty() ? "" : ",") + f.owners;
			return owners;
		}

		CarryRow Resolve(const Group& group, HMODULE module, const VersionMap::Exe& exe, uintptr_t base)
		{
			CarryRow row;
			row.key  = std::get<0>(group);
			row.kind = std::get<1>(group);
			row.fp   = std::get<2>(group);

			const size_t slash = row.key.find('/');
			const std::string owner = row.fp != 0 && slash != std::string::npos ? row.key.substr(0, slash) : std::string();
			std::string why;
			const std::vector<MapLocator> found = MapLocators(row.key, row.kind, row.fp, owner, exe.cl, &why);
			if (found.empty())
			{
				row.reason = "no locator to carry: " + why;
				return row;
			}

			// With no row for exe.cl the lookup leaves one: the latest build's.
			const MapLocator& tried = found.front();
			row.fromCL          = tried.gameCL;
			row.pattern         = tried.pattern;
			row.resultOffset    = tried.resultOffset;
			row.followRel32At   = tried.followRel32At;
			row.followRipDispAt = tried.followRipDispAt;
			if (row.fp == 0)
				row.owners = BoundOwners(group, tried.gameCL);

			ScanValidation::Request request;
			request.name   = row.key;
			request.kind   = static_cast<ScanValidation::Kind>(row.kind);
			request.module = module;

			VersionMap::Candidate candidate;
			candidate.gameCL          = tried.gameCL;
			candidate.pattern         = tried.pattern;
			candidate.resultOffset    = tried.resultOffset;
			candidate.followRel32At   = tried.followRel32At;
			candidate.followRipDispAt = tried.followRipDispAt;
			candidate.fromMap         = true;

			const ScanValidation::Result decided =
				VersionMap::Select(owner.empty() ? "AobScan" : owner.c_str(), request, { candidate }, exe);
			if (decided.Succeeded())
			{
				row.result  = CarryResult::Match;
				row.matches = decided.matchCount;
				row.hasRva  = true;
				row.rva     = static_cast<uint32_t>(decided.address - base);
				return row;
			}

			// Select reports a pattern that found too many places as no locator
			// resolving; the pattern's own answer says which it was.
			request.pattern         = tried.pattern;
			request.resultOffset    = tried.resultOffset;
			request.followRel32At   = tried.followRel32At;
			request.followRipDispAt = tried.followRipDispAt;
			const ScanValidation::Result own = ScanValidation::Resolve(request);
			row.matches = own.matchCount;
			row.result  = own.outcome == ScanValidation::Outcome::MultipleMatches
				|| decided.outcome == ScanValidation::Outcome::MultipleMatches
				? CarryResult::Ambiguous : CarryResult::None;
			row.reason  = decided.detail;
			return row;
		}
	}

	Carry ScanCarry(HMODULE module, const VersionMap::Exe& exe)
	{
		Carry carry;
		carry.cl = exe.cl;
		const uintptr_t base = Scanner::GetImageInfo(module).base;

		// The map's keys in the order it holds them, each under the kind and
		// fingerprint it is kept with (a site key has one row set per request).
		std::vector<Group> groups;
		std::set<Group> seen, rowed, absent;
		for (const VersionMap::MapFunctionRow& f : VersionMap::ActiveMap().functions)
		{
			const Group g{ f.key, f.kind, f.fp };
			if (seen.insert(g).second)
				groups.push_back(g);
			if (f.gameCL == exe.cl)
			{
				rowed.insert(g);
				if (f.absent)
					absent.insert(g);
			}
		}

		carry.keys = groups.size();
		for (const Group& g : groups)
		{
			if (rowed.count(g))
			{
				++carry.withRow;
				carry.absentRow += absent.count(g);
				continue;
			}
			carry.rows.push_back(Resolve(g, module, exe, base));
		}
		return carry;
	}

	std::string CarryJson(const Carry& carry)
	{
		size_t match = 0, none = 0, ambiguous = 0;
		for (const CarryRow& r : carry.rows)
		{
			match     += r.result == CarryResult::Match;
			none      += r.result == CarryResult::None;
			ambiguous += r.result == CarryResult::Ambiguous;
		}

		std::string out;
		char line[512];
		sprintf_s(line, "{\n  \"kind\": \"carry\",\n  \"cl\": %u,\n", carry.cl);
		out += line;
		sprintf_s(line, "  \"summary\": { \"keys\": %zu, \"withRow\": %zu, \"absentRow\": %zu, \"carried\": %zu, "
			"\"match\": %zu, \"none\": %zu, \"ambiguous\": %zu },\n  \"rows\": [\n",
			carry.keys, carry.withRow, carry.absentRow, carry.rows.size(), match, none, ambiguous);
		out += line;
		for (size_t i = 0; i < carry.rows.size(); ++i)
		{
			const CarryRow& r = carry.rows[i];
			out += "    { \"key\": \"" + JsonEscape(r.key) + "\", \"fromCL\": ";
			out += r.fromCL ? std::to_string(r.fromCL) : "null";
			out += ", \"matches\": " + std::to_string(r.matches) + ", \"rva\": ";
			out += r.hasRva ? "\"" + Hex8(r.rva) + "\"" : "null";
			out += std::string(", \"result\": \"") + ResultName(r.result) + "\"";
			if (r.result != CarryResult::Match)
				out += ", \"reason\": \"" + JsonEscape(JsonDetail(r.reason)) + "\"";
			out += ", \"kind\": " + std::to_string(r.kind) + ", \"fp\": \"" + Hex8(r.fp) + "\"";
			out += ", \"pattern\": \"" + JsonEscape(r.pattern) + "\"";
			out += ", \"resultOffset\": " + std::to_string(r.resultOffset)
				+ ", \"followRel32At\": " + std::to_string(r.followRel32At)
				+ ", \"followRipDispAt\": " + std::to_string(r.followRipDispAt);
			if (!r.owners.empty())
				out += ", \"owners\": \"" + JsonEscape(r.owners) + "\"";
			out += i + 1 < carry.rows.size() ? " },\n" : " }\n";
		}
		out += "  ]\n}\n";
		return out;
	}
}
