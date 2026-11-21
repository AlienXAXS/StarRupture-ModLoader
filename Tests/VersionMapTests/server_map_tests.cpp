// VersionMapTests, Server configurations: the server's own table of version
// object map rows (hooks/game/version_map.server.h), compiled into the Server
// build only, and the five places version_map.cpp serves it.
//
// The table is the real one. The five places are checked on the map the loader
// reads (the real table, with test rows added for the field and absent cases),
// each on the exe the table was measured on and on exes that are not it.
#include "test_doubles.h"

#include "hooks/game/scan_patterns.h"
#include "hooks/game/version_map.server.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_interface.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using ScanValidation::Kind;
using ScanValidation::Request;
using ScanValidation::Result;
using VersionMap::Candidate;

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL: %s\n", what);
		}
	}

	uintptr_t Base() { return Scanner::GetImageInfo(GetModuleHandleW(nullptr)).base; }

	// The exe the table was measured on, and exes that are not it.
	const VersionMap::Exe kServer{ 127004, 0x2E3595FF, 0x0E2A8000 };
	const VersionMap::Exe kWrongStamp{ 127004, 0x2E3595FE, 0x0E2A8000 };
	const VersionMap::Exe kWrongSize{ 127004, 0x2E3595FF, 0x0E2A8001 };
	const VersionMap::Exe kClientExe{ 127004, 0xE8972298, 0x0F35C000 };
	const VersionMap::Exe kOtherCL{ 128786, 0x2E3595FF, 0x0E2A8000 };
	const VersionMap::Exe kNoCL{ 0, 0x2E3595FF, 0x0E2A8000 };

	Request Ask(const char* name, Kind kind = Kind::FunctionStart)
	{
		Request r;
		r.name = name;
		r.kind = kind;
		return r;
	}

	Result Key(const char* key, const VersionMap::Exe& running, const std::vector<Candidate>& own = {},
		Kind kind = Kind::FunctionStart)
	{
		TestDoubles::g_log.clear();
		return VersionMap::ResolveMapKey("VmapProbe", Ask(key, kind), own, running, false);
	}

	Candidate AddressRow(const VersionMap::Exe& on, uint32_t rva)
	{
		Candidate c;
		c.gameCL     = on.cl;
		c.imageStamp = on.imageStamp;
		c.imageSize  = on.imageSize;
		c.rva        = rva;
		return c;
	}

	bool At(const Result& r, uintptr_t rva) { return r.Succeeded() && r.address == Base() + rva; }

	// Reflection as the field cases see it: ATest is 0x50 bytes, and has no member the table names.
	VersionMap::LiveField FakeLive(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField f;
		if (className == "ATest" && member.empty())
			f.size = 0x50;
		else
			f.error = "no native class " + className;
		return f;
	}

	// Reflection for the table's own keys: the game reflects none of the classes, so a row stands for the size
	// and a member is served without a class bound.
	VersionMap::LiveField Unreflected(const std::string&, const std::string&)
	{
		VersionMap::LiveField f;
		f.unreflected = true;
		return f;
	}

	// Reflection that knows FTransform and its size (set per case), to try the class bound on a real row.
	uint32_t g_transformSize = 0;
	VersionMap::LiveField SizedTransform(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField f;
		if (className == "FTransform" && member.empty())
			f.size = g_transformSize;
		else
			f.error = "no native class " + className;
		return f;
	}

	bool Field(const char* key, uint32_t flags, uint32_t size, const std::vector<VersionMap::FieldCandidate>& own,
		const VersionMap::Exe& running, VersionMap::FieldValue& out, std::string& error, bool* absent = nullptr)
	{
		TestDoubles::g_log.clear();
		error.clear();
		const std::string name = key;
		const size_t      split = name.find("::");
		VersionMap::FieldRequest request;
		request.key         = name;
		request.className   = name.substr(0, split);
		request.member      = split == std::string::npos ? std::string() : name.substr(split + 2);
		request.flags       = flags;
		request.size        = size;
		request.builtCL     = 0;
		request.builtOffset = 0;
		request.candidates  = own;
		return VersionMap::SelectField("Test", request, running, out, error, nullptr, absent);
	}

	VersionMap::FieldCandidate OwnRow(uint32_t cl, int32_t offset, uint32_t size)
	{
		VersionMap::FieldCandidate c;
		c.gameCL = cl;
		c.offset = offset;
		c.size   = size;
		return c;
	}
}

int main()
{
	// The table: one build, the exe it was measured on, and a row per key on that build only.
	{
		const VersionMap::MapData& map = VersionMap::BaselineMap();
		Check(map.builds.size() == 1 && map.builds[0].gameCL == 127004 && map.builds[0].imageStamp == 0x2E3595FF
			&& map.builds[0].imageSize == 0x0E2A8000, "table: one build, the server exe (CL 127004, stamp 2E3595FF, size 0E2A8000)");
		Check(!map.functions.empty() && map.functions.size() == VersionMap::kServerFunctionRows.size(), "table: every function row is in the map");
		Check(map.functions.size() == 30, "table: 27 function starts, 2 data globals and the loader's GObjects row");
		bool allOnServer = true, allAddress = true;
		int starts = 0, data = 0, scoped = 0;
		for (const VersionMap::MapFunctionRow& f : map.functions)
		{
			allOnServer = allOnServer && f.gameCL == 127004 && f.imageStamp == 0x2E3595FF && f.imageSize == 0x0E2A8000;
			allAddress  = allAddress && f.pattern.empty() && f.rva != 0 && !f.absent
				&& ((f.kind == 1 && f.fp == 0) || (f.kind == 4 && f.fp == 0) || (f.kind == 6 && f.fp != 0));
			starts += f.kind == 1;
			data   += f.kind == 4;
			scoped += f.kind == 6;
		}
		Check(allOnServer, "table: every function row names the server exe");
		Check(allAddress, "table: every function row is an address with no pattern (a start or a global, with a fingerprint only on the loader's key)");
		Check(starts == 27 && data == 2 && scoped == 1, "table: 27 function starts, GNames and GWorld as data, and one scoped key");

		// Field rows: every one is on the build, no absent rows, and a size row is a class key (no "::") at offset 0.
		Check(map.fields.size() == VersionMap::kServerFields.size() && map.fields.size() == 109, "table: every field row is in the map (89 members, 20 sizes)");
		Check(map.absentFields.empty(), "table: no absent rows");
		int members = 0, sizes = 0;
		bool fieldsOnBuild = true, sizesWellFormed = true, masksWellFormed = true;
		for (const VersionMap::MapFieldRow& f : map.fields)
		{
			fieldsOnBuild = fieldsOnBuild && f.gameCL == 127004 && f.size != 0;
			if (f.key.find("::") == std::string::npos)
			{
				++sizes;
				sizesWellFormed = sizesWellFormed && f.offset == 0 && f.boolMask == 0 && f.member.empty();
			}
			else
			{
				++members;
				masksWellFormed = masksWellFormed && (f.boolMask == 0 || f.boolMask == 0xFF || (f.boolMask & (f.boolMask - 1)) == 0);
			}
		}
		Check(fieldsOnBuild, "table: every field row is for CL 127004 and has a size");
		Check(members == 89 && sizes == 20, "table: 89 member rows and 20 size rows");
		Check(sizesWellFormed, "table: a size row is a class key at offset 0 with no bool mask");
		Check(masksWellFormed, "table: a bool mask is 0xFF (a bool) or one bit (a bit field)");

		// The index arrays are in strcmp order with one row per key, so the lookups can bisect them.
		bool functionsSorted = true, fieldsSorted = true, indexesMatch = true;
		for (size_t i = 0; i < VersionMap::kServerFunctions.size(); ++i)
		{
			indexesMatch    = indexesMatch && VersionMap::kServerFunctions[i].first == i && VersionMap::kServerFunctions[i].count == 1;
			functionsSorted = functionsSorted && (i == 0 || strcmp(VersionMap::kServerFunctions[i - 1].key, VersionMap::kServerFunctions[i].key) < 0);
		}
		for (size_t i = 0; i < VersionMap::kServerFields.size(); ++i)
		{
			indexesMatch = indexesMatch && VersionMap::kServerFields[i].first == i && VersionMap::kServerFields[i].count == 1;
			fieldsSorted = fieldsSorted && (i == 0 || strcmp(VersionMap::kServerFields[i - 1].key, VersionMap::kServerFields[i].key) < 0);
		}
		Check(functionsSorted && fieldsSorted, "table: both key lists are sorted with no key twice");
		Check(indexesMatch, "table: each index entry points at its own single row");

		// Values the PDB gave, for a few rows of each kind.
		bool anchors = true;
		for (const VersionMap::MapFieldRow& f : map.fields)
		{
			if (f.key == "FTransform::Translation")
				anchors = anchors && f.offset == 0x20 && f.size == 0x18;   // the size a plugin reads, as on the client's row
			if (f.key == "FCrCraftingFragment::CraftingMultiplier")
				anchors = anchors && f.offset == 0x70 && f.size == 4;
			if (f.key == "UCrReplicationGraph::ClassRepNodePolicies")
				anchors = anchors && f.offset == 0x5E0 && f.size == 0x80;
		}
		Check(anchors, "table: the measured values for FTransform::Translation, CraftingMultiplier and ClassRepNodePolicies");
		const std::vector<uint32_t> builds = VersionMap::SupportedBuilds();
		Check(builds.size() == 1 && builds[0] == 127004, "guard 1, SupportedBuilds: the server's build is the one its table lists");
	}

	// Guard 2, LookUp (through ResolveMapKey): every key serves its row on the server exe and nothing on any other.
	{
		// The plugin-readable keys: function starts and the two data globals, each asked as its own kind. The loader's
		// scoped GObjects key (kind 6) is read through the loader's own request, below.
		int n = 0, served = 0, refusedWrongStamp = 0, refusedWrongSize = 0, refusedClient = 0, refusedOtherCL = 0, refusedNoCL = 0;
		for (const VersionMap::Index& index : VersionMap::kServerFunctions)
		{
			const VersionMap::FunctionRow& row = VersionMap::kServerFunctionRows[index.first];
			if (row.kind == 6)
				continue;
			const Kind kind = static_cast<Kind>(row.kind);
			++n;
			served            += At(Key(index.key, kServer, {}, kind), row.rva);
			refusedWrongStamp += !Key(index.key, kWrongStamp, {}, kind).Succeeded();
			refusedWrongSize  += !Key(index.key, kWrongSize, {}, kind).Succeeded();
			refusedClient     += !Key(index.key, kClientExe, {}, kind).Succeeded();
			refusedOtherCL    += !Key(index.key, kOtherCL, {}, kind).Succeeded();
			refusedNoCL       += !Key(index.key, kNoCL, {}, kind).Succeeded();
		}
		Check(n == 29 && served == n, "guard 2, LookUp: every key serves its RVA on the server exe");
		// A key is served as the kind it was recorded with, and no other.
		Check(!Key("GWorld", kServer).Succeeded() && !Key("UCrSaveSubsystem::GetSaveData", kServer, {}, Kind::Data).Succeeded(),
			"guard 2, LookUp: a global is not served as a function start, nor a function start as data");
		Check(refusedWrongStamp == n, "guard 2, LookUp: no key serves on an exe with the wrong time stamp");
		Check(refusedWrongSize == n, "guard 2, LookUp: no key serves on an exe with the wrong image size");
		Check(refusedClient == n, "guard 2, LookUp: no key serves on the client exe's identity");
		Check(refusedOtherCL == n, "guard 2, LookUp: no key serves on another CL with the server's stamp and size");
		Check(refusedNoCL == n, "guard 2, LookUp: no key serves on an exe with no CL");

		const char* key = "UCrSaveSubsystem::GetSaveData";
		Check(At(Key(key, kWrongStamp, { AddressRow(kWrongStamp, 0x1000) }), 0x1000),
			"guard 2, LookUp: off on the wrong exe, the plugin's own row decides");
		Check(!Key("UCrSaveSubsystem::NoSuchFunction", kServer).Succeeded(), "guard 2, LookUp: a key the table does not hold resolves to nothing");
		Check(!Key("UCrSaveSubsystem::GetSaveData", VersionMap::Exe{}).Succeeded(), "guard 2, LookUp: an exe that is not identified resolves to nothing");
	}

	// Guard 2 again, with a row that carries a pattern. Such a row is rescanned on the running exe, so only the
	// guard keeps it off an exe that is not the one the table was measured on.
	{
		TestDoubles::g_rvas["AA 01"] = 0x1000;
		VersionMap::MapData data = VersionMap::BaselineMap();
		data.functions.push_back({ "Test::PatternRow", 127004, 0x2E3595FF, 0x0E2A8000, 1, 0, false, "AA 01", 0, -1, -1, 0x1000, "" });
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(data));
		Check(At(Key("Test::PatternRow", kServer), 0x1000), "guard 2, LookUp: a row with a pattern is served on the server exe");
		Check(!Key("Test::PatternRow", kWrongStamp).Succeeded(), "guard 2, LookUp: a row with a pattern is not served on an exe with the wrong time stamp");
		Check(!Key("Test::PatternRow", kWrongSize).Succeeded(), "guard 2, LookUp: a row with a pattern is not served on an exe with the wrong image size");
		Check(!Key("Test::PatternRow", kClientExe).Succeeded(), "guard 2, LookUp: a row with a pattern is not served on the client exe's identity");
		VersionMap::SetActiveMap(nullptr);
	}

	// Guard 2 for the loader's own keys: the Game-SDK globals (GObjects, GNames, FName::AppendString, GWorld) are read
	// by the loader's request for each, the registry row the server build now carries (the client's patterns), by key
	// and fingerprint, and land on the row's address on the server exe only.
	{
		struct Global { const char* registryName; const char* key; int kind; uint32_t rva; };
		const Global globals[] = {
			{ "GObjects (GUObjectArray.ObjObjects)", "ModLoader/GObjects (GUObjectArray.ObjObjects)", 6, 0x0D3A1A50 },
			{ "GNames (NamePoolData)",               "GNames (NamePoolData)",                          4, 0x0D2BD600 },
			{ "FName::AppendString",                 "FName::AppendString",                            1, 0x0142A250 },
			{ "GWorld",                              "GWorld",                                         4, 0x0D50EE38 },
		};
		for (const Global& g : globals)
		{
			const ScanPatterns::PreflightEntry* found = nullptr;
			for (const ScanPatterns::PreflightEntry& e : ScanPatterns::PreflightRegistry)
				if (strcmp(e.name, g.registryName) == 0)
					found = &e;
			const std::string what = std::string("guard 2, LookUp: ") + g.registryName;
			Check(found != nullptr, (what + " is in the server build's preflight registry").c_str());
			if (!found)
				continue;
			const ScanPatterns::PreflightEntry& entry = *found;

			Request request = Ask(entry.name, static_cast<Kind>(static_cast<int>(entry.kind)));
			request.pattern         = entry.pattern;
			request.followRel32At   = entry.followRel32At;
			request.followRipDispAt = entry.followRipDispAt;

			const VersionMap::FunctionRow* row = nullptr;
			for (const VersionMap::Index& index : VersionMap::kServerFunctions)
				if (strcmp(index.key, g.key) == 0)
					row = &VersionMap::kServerFunctionRows[index.first];
			Check(VersionMap::LoaderEntryKey(entry) == g.key && row && row->kind == g.kind && row->rva == g.rva
				&& row->fp == VersionMap::LoaderEntryFingerprint(entry),
				(what + " has the key, kind and fingerprint of the loader's request for it").c_str());

			// The pattern finds the address the row records: the row and the scan agree on the server exe.
			TestDoubles::g_rvas[entry.pattern] = g.rva;
			TestDoubles::g_log.clear();
			Check(At(VersionMap::ResolveLoaderEntry(entry, request, kServer), g.rva),
				(what + ": the loader's request lands on the row's address on the server exe").c_str());

			// The scan finds another address: on the server exe the row decides and the request fails, on an exe that is
			// not the server's the row is not read and the scan stands.
			TestDoubles::g_rvas[entry.pattern] = 0x1234;
			Check(!VersionMap::ResolveLoaderEntry(entry, request, kServer).Succeeded(),
				(what + ": on the server exe a scan that disagrees with the row is refused").c_str());
			for (const VersionMap::Exe* other : { &kWrongStamp, &kWrongSize, &kClientExe, &kOtherCL, &kNoCL })
				Check(At(VersionMap::ResolveLoaderEntry(entry, request, *other), 0x1234),
					(what + ": the row is not read on an exe that is not the server's").c_str());
			TestDoubles::g_rvas.erase(entry.pattern);
		}
	}

	// ServesExe: the server's map serves the exe it was measured on and no other; a map with no builds serves none.
	{
		Check(VersionMap::ServesExe(kServer), "ServesExe: true on the exe the table was measured on");
		for (const VersionMap::Exe* other : { &kWrongStamp, &kWrongSize, &kClientExe, &kOtherCL, &kNoCL })
			Check(!VersionMap::ServesExe(*other), "ServesExe: false on an exe that is not the server's");
		Check(!VersionMap::ServesExe(VersionMap::Exe{}), "ServesExe: false on an exe that is not identified");
		VersionMap::MapData none = VersionMap::BaselineMap();
		none.builds.clear();
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(none));
		Check(!VersionMap::ServesExe(kServer), "ServesExe: false on every exe when the map has no builds");
		VersionMap::SetActiveMap(nullptr);
	}

	// What plugins are handed on the server: GObjects and FName::AppendString always, GWorld only off the build the
	// loader was made for (the server's CL 127004 here).
	{
		Check(VersionMap::SdkGlobalsWithheld(kServer, 0x1000, 0x2000, 0x3000).empty(), "globals: all four found on the server exe, handed out");
		Check(VersionMap::SdkGlobalsWithheld(kServer, 0x1000, 0x2000, 0).empty(), "globals: GWorld missing on the server's own build, still handed out");
		Check(VersionMap::SdkGlobalsWithheld(kServer, 0, 0x2000, 0x3000).find("GObjects") != std::string::npos, "globals: GObjects missing on the server exe, withheld");
		Check(VersionMap::SdkGlobalsWithheld(kServer, 0x1000, 0, 0x3000).find("FName::AppendString") != std::string::npos, "globals: FName::AppendString missing on the server exe, withheld");
		Check(VersionMap::SdkGlobalsWithheld(kOtherCL, 0x1000, 0x2000, 0x3000).empty(), "globals: all found on another server CL, handed out");
		Check(VersionMap::SdkGlobalsWithheld(kOtherCL, 0x1000, 0x2000, 0).find("GWorld") != std::string::npos, "globals: GWorld missing on another server CL, withheld");
	}

	// Admission of a plugin off the server's build: a migrated one loads when it asked for the globals, not otherwise.
	{
		PluginInfo info = { "T", "1", "t", "t", 72, PLUGIN_TARGET_SERVER };
		info.gameBuiltCL = 127004;
		info.mapFlags    = PLUGIN_MAP_MIGRATED;
		const VersionMap::PluginGameBuild built = VersionMap::ReadPluginGameBuild(info);
		Check(VersionMap::OffBuildRefusal(kOtherCL, 72, built, true).empty(), "admission: a migrated plugin built for CL 127004 loads on another server CL when it asked for the globals");
		Check(VersionMap::OffBuildRefusal(kOtherCL, 72, built, false).find("GetSdkGlobals") != std::string::npos,
			"admission: a migrated plugin that did not ask for the globals is refused on another server CL");
		Check(VersionMap::OffBuildRefusal(kServer, 72, built, false).empty(), "admission: and loads on the server's own build either way");
		info.mapFlags = 0;
		const VersionMap::PluginGameBuild plain = VersionMap::ReadPluginGameBuild(info);
		Check(VersionMap::OffBuildRefusal(kOtherCL, 72, plain, true).find("PLUGIN_MAP_MIGRATED") != std::string::npos,
			"admission: a plugin that is not migrated is refused on another server CL, globals or not");
	}

	// Guard 4 on the table's own rows: every member and size row is served on the server exe with the row's values,
	// and on no other exe.
	VersionMap::SetLiveFieldSource(Unreflected);
	{
		int members = 0, sizes = 0, served = 0, notServed = 0, notServedOwnRow = 0;
		VersionMap::FieldValue v;
		std::string e;
		for (const VersionMap::Index& index : VersionMap::kServerFields)
		{
			const VersionMap::FieldRow& row = VersionMap::kServerFieldRows[index.first];
			const bool isSize = strchr(index.key, ':') == nullptr;
			members += !isSize;
			sizes   += isSize;
			// A size request carries no flags; a member is native, so only the table can place it.
			const uint32_t flags = isSize ? 0 : PLUGIN_FIELD_NATIVE;
			const bool ok = Field(index.key, flags, row.size, {}, kServer, v, e)
				&& v.offset == row.offset && v.size == row.size && v.boolMask == row.boolMask;
			served += ok;
			if (!ok)
				printf("  not served: %s (%s)\n", index.key, e.c_str());
			bool elsewhere = true;
			for (const VersionMap::Exe* other : { &kWrongStamp, &kWrongSize, &kClientExe, &kOtherCL, &kNoCL })
				elsewhere = elsewhere && !Field(index.key, flags, row.size, {}, *other, v, e);
			notServed += elsewhere;
			// A native member whose own row disagrees with the table's is refused on the server exe.
			notServedOwnRow += !isSize && !Field(index.key, flags, row.size, { OwnRow(127004, row.offset + 8, row.size) }, kServer, v, e);
		}
		Check(members == 89 && sizes == 20, "guard 4, RowsFor: the table's keys are 89 members and 20 sizes");
		Check(served == members + sizes, "guard 4, RowsFor: every member and size row is served on the server exe, with its offset, size and bool mask");
		Check(notServed == members + sizes, "guard 4, RowsFor: no table row is served on an exe that is not the server's");
		Check(notServedOwnRow == members, "guard 4, RowsFor: a member whose own row disagrees with the table's is refused on the server exe");
	}

	// The native members the shipped server plugins ask for, each as the plugin sends it (its key, the size it is
	// compiled for and its own CL-127004 row): served with that row on the server exe, and from the table alone.
	VersionMap::SetLiveFieldSource(Unreflected);
	{
		struct PluginRequest { const char* key; uint32_t size; int32_t offset; };
		const PluginRequest requests[] = {
			{ "FCrGridEntityHandle::EntityHandle",              0x8,  0x0 },
			{ "FCrGridEntityHandle::Data",                      0x10, 0x8 },
			{ "FTransform::Translation",                        0x18, 0x20 },
			{ "UCrGameInstance::ServerSessionName",             0x10, 0x290 },
			{ "UMassEntitySubsystem::EntityManager",            0x10, 0x38 },
			{ "UMassActorSubsystem::ActorManager",              0x10, 0x38 },
			{ "UCrEnviroWaveTimerSubsystem::NextWaveTimer",     0x4,  0x7C },
			{ "UCrEnviroWaveTimerSubsystem::bWaitingForNextWave", 0x1, 0x80 },
			{ "UCrEnviroWaveTimerSubsystem::bStopWaves",        0x1,  0x83 },
			{ "UCrEnviroWaveTimerSubsystem::TimerActor",        0x8,  0x90 },
		};
		int withOwn = 0, tableOnly = 0;
		VersionMap::FieldValue v;
		std::string e;
		for (const PluginRequest& r : requests)
		{
			withOwn   += Field(r.key, PLUGIN_FIELD_NATIVE, r.size, { OwnRow(127004, r.offset, r.size) }, kServer, v, e)
				&& v.offset == r.offset && v.size == r.size;
			tableOnly += Field(r.key, PLUGIN_FIELD_NATIVE, r.size, {}, kServer, v, e) && v.offset == r.offset && v.size == r.size;
		}
		const int n = static_cast<int>(sizeof(requests) / sizeof(requests[0]));
		Check(withOwn == n, "guard 4, RowsFor: each native member a shipped server plugin asks for is served on the server exe with the plugin's own row");
		Check(tableOnly == n, "guard 4, RowsFor: and from the table's row alone");
	}
	VersionMap::SetLiveFieldSource(nullptr);

	// Guard 5 on a real row: FTransform::Translation is 0x18 bytes at 0x20, so it needs a class of 0x38 bytes.
	VersionMap::SetLiveFieldSource(SizedTransform);
	{
		VersionMap::FieldValue v;
		std::string e;
		g_transformSize = 0x30;
		Check(!Field("FTransform::Translation", PLUGIN_FIELD_NATIVE, 0x18, {}, kServer, v, e) && e.find("past the end of FTransform") != std::string::npos,
			"guard 5, bound: a real table row past the end of its class is refused on the server exe");
		g_transformSize = 0x38;
		Check(Field("FTransform::Translation", PLUGIN_FIELD_NATIVE, 0x18, {}, kServer, v, e) && v.offset == 0x20 && v.size == 0x18,
			"guard 5, bound: the same row inside a class of 0x38 bytes is served");
		g_transformSize = 0x30;
		Check(Field("FTransform::Translation", PLUGIN_FIELD_NATIVE, 0x18, { OwnRow(127004, 0x20, 0x18) }, kWrongStamp, v, e) && v.offset == 0x20,
			"guard 5, bound: off on an exe that is not the server's, the same row as the plugin's own is served unbounded");
	}
	VersionMap::SetLiveFieldSource(nullptr);

	// Guards 3 to 5 read the map the loader holds: the real table, with field and absent rows added for the cases.
	VersionMap::SetLiveFieldSource(FakeLive);
	{
		VersionMap::MapData data = VersionMap::BaselineMap();
		data.absentFields.push_back({ "ATest::Gone", 127004 });
		data.fields.push_back({ "ATest::Mapped", 127004, 0x20, 8, 0, "" });
		data.fields.push_back({ "ATest::PastEnd", 127004, 0x4C, 8, 0, "" });
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(data));

		VersionMap::FieldValue v;
		std::string e;
		bool absent = false;

		// Guard 3, AbsentOnBuild: a member the map lists as absent is refused on the server exe, and asked of the game elsewhere.
		absent = false;
		Check(!Field("ATest::Gone", PLUGIN_FIELD_NATIVE, 8, {}, kServer, v, e, &absent) && absent && e == "not in this game version (CL-127004)",
			"guard 3, AbsentOnBuild: a member the table lists as absent is refused on the server exe");
		for (const VersionMap::Exe* other : { &kWrongStamp, &kWrongSize, &kClientExe, &kOtherCL })
		{
			absent = false;
			Field("ATest::Gone", PLUGIN_FIELD_NATIVE, 8, {}, *other, v, e, &absent);
			Check(!absent && e.find("not in this game version") == std::string::npos,
				"guard 3, AbsentOnBuild: off on an exe that is not the server's");
		}

		// Guard 4, RowsFor: a member the map has a row for is served from it on the server exe. Elsewhere the plugin's
		// own rows are all there is, and a native member with none is refused.
		Check(Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, {}, kServer, v, e) && v.offset == 0x20 && v.size == 8,
			"guard 4, RowsFor: a native member is served from the table's row on the server exe");
		Check(Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, { OwnRow(127004, 0x20, 8) }, kServer, v, e) && v.offset == 0x20,
			"guard 4, RowsFor: the plugin's own row agreeing with the table's is served");
		Check(!Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, { OwnRow(127004, 0x28, 8) }, kServer, v, e),
			"guard 4, RowsFor: a native member whose own row disagrees with the table's is refused on the server exe");
		for (const VersionMap::Exe* other : { &kWrongStamp, &kWrongSize, &kClientExe, &kOtherCL })
		{
			Check(!Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, {}, *other, v, e),
				"guard 4, RowsFor: off on an exe that is not the server's, a native member with no own row is refused");
			Check(other->cl != 127004 || (Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, { OwnRow(127004, 0x28, 8) }, *other, v, e) && v.offset == 0x28),
				"guard 4, RowsFor: off on an exe that is not the server's, the plugin's own row is served as it is");
		}

		// Guard 5, the class bound: a member the table gave a row for is bounded by its class (ATest is 0x50 bytes).
		Check(!Field("ATest::PastEnd", PLUGIN_FIELD_NATIVE, 8, {}, kServer, v, e) && e.find("past the end of ATest") != std::string::npos,
			"guard 5, bound: a table row past the end of its class is refused on the server exe");
		Check(Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, {}, kServer, v, e) && v.offset == 0x20,
			"guard 5, bound: a table row inside its class is served on the server exe");
		// Off for a member without a table row: the offset compiled into the plugin stands, as it did before the table.
		Check(Field("ATest::Compiled", PLUGIN_FIELD_NATIVE, 8, { OwnRow(127004, 0x4C, 8) }, kServer, v, e) && v.offset == 0x4C,
			"guard 5, bound: off for a native member with no table row, its own row is served unbounded on the server exe");
		// Off on an exe that is not the server's: the table is not read, and neither is the bound.
		for (const VersionMap::Exe* other : { &kWrongStamp, &kWrongSize, &kClientExe })
			Check(Field("ATest::PastEnd", PLUGIN_FIELD_NATIVE, 8, { OwnRow(127004, 0x4C, 8) }, *other, v, e) && v.offset == 0x4C,
				"guard 5, bound: off on an exe that is not the server's, the plugin's own row is served unbounded");

		// A map with no builds serves nothing, on any exe, the server's included.
		VersionMap::MapData none = data;
		none.builds.clear();
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(none));
		Check(VersionMap::SupportedBuilds().empty(), "guard 1, SupportedBuilds: a map with no builds supports none");
		Check(!Key("UCrSaveSubsystem::GetSaveData", kServer).Succeeded(), "guard 2, LookUp: a map with no builds serves no function row");
		absent = false;
		Field("ATest::Gone", PLUGIN_FIELD_NATIVE, 8, {}, kServer, v, e, &absent);
		Check(!absent, "guard 3, AbsentOnBuild: a map with no builds lists nothing as absent");
		Check(!Field("ATest::Mapped", PLUGIN_FIELD_NATIVE, 8, {}, kServer, v, e), "guard 4, RowsFor: a map with no builds serves no field row");

		VersionMap::SetActiveMap(nullptr);
	}
	VersionMap::SetLiveFieldSource(nullptr);

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
