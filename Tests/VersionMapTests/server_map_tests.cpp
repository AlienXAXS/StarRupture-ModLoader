// VersionMapTests, Server configurations: the server's own table of version
// object map rows (hooks/game/version_map.server.h), compiled into the Server
// build only, and the five places version_map.cpp serves it.
//
// The table is the real one. The five places are checked on the map the loader
// reads (the real table, with test rows added for the field and absent cases),
// each on the exe the table was measured on and on exes that are not it.
#include "test_doubles.h"

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

	Request Ask(const char* name)
	{
		Request r;
		r.name = name;
		r.kind = Kind::FunctionStart;
		return r;
	}

	Result Key(const char* key, const VersionMap::Exe& running, const std::vector<Candidate>& own = {})
	{
		TestDoubles::g_log.clear();
		return VersionMap::ResolveMapKey("VmapProbe", Ask(key), own, running, false);
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

	bool Field(const char* key, uint32_t flags, uint32_t size, const std::vector<VersionMap::FieldCandidate>& own,
		const VersionMap::Exe& running, VersionMap::FieldValue& out, std::string& error, bool* absent = nullptr)
	{
		TestDoubles::g_log.clear();
		error.clear();
		const std::string name = key;
		VersionMap::FieldRequest request;
		request.key         = name;
		request.className   = name.substr(0, name.find("::"));
		request.member      = name.substr(name.find("::") + 2);
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
		bool allOnServer = true, allAddress = true;
		for (const VersionMap::MapFunctionRow& f : map.functions)
		{
			allOnServer = allOnServer && f.gameCL == 127004 && f.imageStamp == 0x2E3595FF && f.imageSize == 0x0E2A8000;
			allAddress  = allAddress && f.pattern.empty() && f.rva != 0 && !f.absent && f.kind == 1 && f.fp == 0;
		}
		Check(allOnServer, "table: every function row names the server exe");
		Check(allAddress, "table: every function row is a global function-start address with no pattern");
		Check(map.fields.empty() && map.absentFields.empty(), "table: no field or absent rows");
		const std::vector<uint32_t> builds = VersionMap::SupportedBuilds();
		Check(builds.size() == 1 && builds[0] == 127004, "guard 1, SupportedBuilds: the server's build is the one its table lists");
	}

	// Guard 2, LookUp (through ResolveMapKey): every key serves its row on the server exe and nothing on any other.
	{
		int served = 0, refusedWrongStamp = 0, refusedWrongSize = 0, refusedClient = 0, refusedOtherCL = 0, refusedNoCL = 0;
		for (const VersionMap::Index& index : VersionMap::kServerFunctions)
		{
			const uint32_t rva = VersionMap::kServerFunctionRows[index.first].rva;
			served            += At(Key(index.key, kServer), rva);
			refusedWrongStamp += !Key(index.key, kWrongStamp).Succeeded();
			refusedWrongSize  += !Key(index.key, kWrongSize).Succeeded();
			refusedClient     += !Key(index.key, kClientExe).Succeeded();
			refusedOtherCL    += !Key(index.key, kOtherCL).Succeeded();
			refusedNoCL       += !Key(index.key, kNoCL).Succeeded();
		}
		const int n = static_cast<int>(VersionMap::kServerFunctions.size());
		Check(n == 15 && served == n, "guard 2, LookUp: every key serves its RVA on the server exe");
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
