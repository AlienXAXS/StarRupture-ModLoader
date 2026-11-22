// VersionMapTests, Server configurations: the server's own table of version
// object map rows (hooks/game/version_map.server.h), compiled into the Server
// build only, and the five places version_map.cpp serves it.
//
// The table is the real one. The five places are checked on the map the loader
// reads (the real table, with test rows added for the field and absent cases),
// each on the exe the table was measured on and on exes that are not it.
//
// The downloaded server file (version-map-server.v1.txt) is checked at the end:
// signed here with a seeded key, put in a cache file the way the end-to-end test
// seeds one, read onto the real table, and served for a second server CL.
#include "test_doubles.h"

#include "hooks/game/scan_patterns.h"
#include "hooks/game/version_map.server.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_feed.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_interface.h"
#include "third_party/monocypher/monocypher-ed25519.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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

	bool Logged(const wchar_t* text)
	{
		for (const std::wstring& line : TestDoubles::g_log)
			if (line.find(text) != std::wstring::npos)
				return true;
		return false;
	}

	// ---- the downloaded server file, signed here with a seeded key ----------

	std::string Base64(const uint8_t* p, size_t n)
	{
		static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string s;
		for (size_t i = 0; i < n; i += 3)
		{
			const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
			s += a[v >> 18];
			s += a[(v >> 12) & 63];
			s += i + 1 < n ? a[(v >> 6) & 63] : '=';
			s += i + 2 < n ? a[v & 63] : '=';
		}
		return s;
	}

	struct Signer
	{
		uint8_t                 secret[64]{};
		VersionFeed::PublicKey  pub;
	};

	Signer MakeSigner(const char* id, uint8_t salt)
	{
		Signer s;
		uint8_t seed[32];
		for (int i = 0; i < 32; ++i)
			seed[i] = static_cast<uint8_t>(salt * 7 + i);
		crypto_ed25519_key_pair(s.secret, s.pub.key.data(), seed);
		s.pub.id = id;
		s.pub.testOnly = true;
		return s;
	}

	std::string Sign(const Signer& s, uint64_t seq, const std::string& body)
	{
		const std::string region = "format\t1\nseq\t" + std::to_string(seq) + "\n" + body;
		uint8_t sig[64];
		crypto_ed25519_sign(sig, s.secret, reinterpret_cast<const uint8_t*>(region.data()), region.size());
		return region + "sig\t" + s.pub.id + "\t" + Base64(sig, 64) + "\n";
	}

	// A function row: an address-only one (pattern "-"), or one with the pattern the address was verified with.
	std::string FnRow(const std::string& key, uint32_t fp, const VersionMap::Exe& on, int kind, uint32_t rva,
		const std::string& pattern = "-", int followRel32At = -1, int followRipDispAt = -1)
	{
		char head[256];
		sprintf_s(head, "function\t%s\t0x%08X\t%u\t0x%08X\t0x%08X\t%d\t", key.c_str(), fp, on.cl, on.imageStamp, on.imageSize, kind);
		char tail[128];
		sprintf_s(tail, "\t0\t%d\t%d\t0x%08X\n", followRel32At, followRipDispAt, rva);
		return std::string(head) + pattern + tail;
	}

	std::string FieldRow(const char* key, uint32_t cl, uint32_t offset, uint32_t size)
	{
		char b[256];
		sprintf_s(b, "field\t%s\t%u\t0x%X\t0x%X\t0x00\t-\n", key, cl, offset, size);
		return b;
	}

	std::wstring TempPath(const wchar_t* name)
	{
		wchar_t dir[MAX_PATH]{};
		GetTempPathW(MAX_PATH, dir);
		return std::wstring(dir) + L"vmap_server_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
	}

	bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

	void Put(const std::wstring& p, const std::string& text)
	{
		HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		DWORD n = 0;
		if (h != INVALID_HANDLE_VALUE)
		{
			WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
			CloseHandle(h);
		}
	}

	std::string Slurp(const std::wstring& p)
	{
		std::string s;
		HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return s;
		LARGE_INTEGER sz{};
		if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0)
		{
			s.resize(static_cast<size_t>(sz.QuadPart));
			DWORD got = 0;
			ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &got, nullptr);
			s.resize(got);
		}
		CloseHandle(h);
		return s;
	}

	// Reflection for the loader's own members on a second server CL: the game answers, and a row only cross-checks.
	VersionMap::LiveField LoaderLive(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField f;
		if (className == "UWorld" && member == "GameState")
		{
			f.offset = 0x1B8;
			f.size   = 8;
		}
		else if (className == "UWorld" && member == "NetDriver")
		{
			f.offset = 0x2C0;   // the feed's row says 0x2D0
			f.size   = 8;
		}
		else
			f.error = "no member " + className + "::" + member;
		return f;
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
		Check(map.fields.size() == VersionMap::kServerFields.size() && map.fields.size() == 110, "table: every field row is in the map (90 members, 20 sizes)");
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
		Check(members == 90 && sizes == 20, "table: 90 member rows and 20 size rows");
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
		Check(members == 90 && sizes == 20, "guard 4, RowsFor: the table's keys are 90 members and 20 sizes");
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

		// The HTTP server hook reads the request's verb flags (a uint16) by name, with no row of its own.
		const char* verbKey = "FHttpServerRequest::Verb";
		Check(Field(verbKey, PLUGIN_FIELD_NATIVE, 2, {}, kServer, v, e) && v.offset == 0x20 && v.size == 2 && v.boolMask == 0,
			"http verb: the request's verb flags are served on the server exe at 0x20, 2 bytes");
		Check(!Field(verbKey, PLUGIN_FIELD_NATIVE, 2, {}, kOtherCL, v, e) && !Field(verbKey, PLUGIN_FIELD_NATIVE, 2, {}, kClientExe, v, e),
			"http verb: no row on another server CL or on the client exe, so the hook leaves the method as Other");
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

	// ---- The downloaded server file --------------------------------------------------------------------------
	// A second server game build (CL 128786), named by a signed file in the server's cache, and a third (CL 130000)
	// whose block has no rows of the loader's own. The cache is read onto the real table, as at start.
	{
		const VersionMap::Exe kNew{ 128786, 0x33333333, 0x03000000 };
		const VersionMap::Exe kNewRebuilt{ 128786, 0x33333334, 0x03000000 };
		const VersionMap::Exe kNewWrongSize{ 128786, 0x33333333, 0x03000001 };
		const VersionMap::Exe kThird{ 130000, 0x44444444, 0x04000000 };

		const Signer sk = MakeSigner("k1", 1);
		const std::vector<VersionFeed::PublicKey> keys{ sk.pub };
		const std::wstring cache = TempPath(L"version-map-server.v1.cache");
		DeleteFileW(cache.c_str());

		// The built-in value of one key on the server's own exe, to see it is not overridden.
		uint32_t builtinSaveData = 0;
		for (const VersionMap::Index& index : VersionMap::kServerFunctions)
			if (strcmp(index.key, "UCrSaveSubsystem::GetSaveData") == 0)
				builtinSaveData = VersionMap::kServerFunctionRows[index.first].rva;
		Check(builtinSaveData != 0, "feed: the built-in table holds the key the feed is made to disagree on");

		// The loader's own entries, by their registry names (as in guard 2 above); the feed gives CL 128786 a row for each.
		struct Global { const char* registryName; uint32_t rva; };
		const Global globals[] = {
			{ "GObjects (GUObjectArray.ObjObjects)", 0x4100 },
			{ "GNames (NamePoolData)",               0x4200 },
			{ "FName::AppendString",                 0x4300 },
			{ "GWorld",                              0x4400 },
		};
		const auto registryEntry = [](const char* name) -> const ScanPatterns::PreflightEntry*
		{
			for (const ScanPatterns::PreflightEntry& e : ScanPatterns::PreflightRegistry)
				if (strcmp(e.name, name) == 0)
					return &e;
			return nullptr;
		};

		std::string body =
			"build\t127004\t0x2E3595FF\t0x0E2A8000\tserver steam_build 25642172\n"
			"build\t128786\t0x33333333\t0x03000000\tserver steam_build 2\n"
			"build\t130000\t0x44444444\t0x04000000\tserver steam_build 3\n";
		body += FnRow("Test::FeedFn", 0, kNew, 1, 0x1234);
		body += FnRow("UCrSaveSubsystem::GetSaveData", 0, kNew, 1, 0x5678);
		body += FnRow("UCrSaveSubsystem::GetSaveData", 0, VersionMap::Exe{ 127004, 0x2E3595FF, 0x0E2A8000 }, 1, 0x9999);   // the built-in row says otherwise
		body += FnRow("Test::ThirdFn", 0, kThird, 1, 0x7777);
		for (const Global& g : globals)
		{
			const ScanPatterns::PreflightEntry* e = registryEntry(g.registryName);
			Check(e != nullptr, "feed: the loader's entry is in the preflight registry");
			if (e)
				body += FnRow(VersionMap::LoaderEntryKey(*e), VersionMap::LoaderEntryFingerprint(*e), kNew, static_cast<int>(e->kind), g.rva,
					e->pattern, e->followRel32At, e->followRipDispAt);
		}
		body += FieldRow("FTransform", 128786, 0x0, 0x40);
		body += FieldRow("FTransform::Translation", 128786, 0x20, 0x18);
		body += FieldRow("UWorld::GameState", 128786, 0x1B8, 0x8);
		body += FieldRow("UWorld::NetDriver", 128786, 0x2D0, 0x8);
		const std::string file = Sign(sk, 10, body);

		// Wrong file, both ways. A client file (a build line labelled as a client's) is signed and verifies, and a
		// server loader refuses it; a server file is refused as a client's.
		{
			const std::string clientFile = Sign(sk, 20, "build\t127004\t0x11111111\t0x01000000\tpublic 1\n");
			VersionFeed::SignedFeed read;
			std::string e;
			Check(!VersionFeed::ReadSigned(clientFile, keys, read, e) && e.find("not a dedicated server file") != std::string::npos && read.map.builds.empty(),
				"feed, wrong file: this server build refuses a client file, and says why");
			Check(!VersionFeed::ReadSigned(file, keys, read, e, VersionFeed::Flavor::Client) && e.find("dedicated server file") != std::string::npos,
				"feed, wrong file: and refuses nothing of its own: the server file is a client loader's refusal");
			Check(VersionFeed::ReadSigned(file, keys, read, e) && read.seq == 10 && read.map.builds.size() == 3,
				"feed, wrong file: the server file reads as this build's own");
			Check(!VersionFeed::ReadSigned(Sign(sk, 21, ""), keys, read, e), "feed, wrong file: a file with no build line is not a server file");
			Check(!VersionFeed::ReadSigned(Sign(sk, 22, "build\t127004\t0x2E3595FF\t0x0E2A8000\tserver steam_build 1\nbuild\t128786\t0x33333333\t0x03000000\tpublic 2\n"), keys, read, e),
				"feed, wrong file: one build line without the server label and the file is refused");

			// In the cache: the refusal is logged and the built-in table stays in force.
			Put(cache, clientFile);
			TestDoubles::g_log.clear();
			Check(!VersionFeed::BuildFromCache(cache, keys, kServer, VersionFeed::LoaderCoverage()) && Logged(L"dedicated server file"),
				"feed, wrong file: a client file in the server cache is refused and logged, and the built-in table stays");
			DeleteFileW(cache.c_str());
		}

		// The cache: written once through a temp file, never replaced by an equal or older seq, never by a client file.
		{
			std::string e;
			Check(VersionFeed::StoreCache(cache, file, keys, e) && Slurp(cache) == file && !Exists(cache + L".tmp"), "feed, cache: a verified server file is written, and its temp file is gone");
			Check(!VersionFeed::StoreCache(cache, Sign(sk, 9, body), keys, e) && Slurp(cache) == file && e.find("not above") != std::string::npos,
				"feed, cache: a lower seq does not replace it");
			Check(!VersionFeed::StoreCache(cache, file, keys, e) && Slurp(cache) == file, "feed, cache: an equal seq does not replace it");
			Check(!VersionFeed::StoreCache(cache, Sign(sk, 30, "build\t127004\t0x11111111\t0x01000000\tpublic 1\n"), keys, e) && Slurp(cache) == file,
				"feed, cache: a higher client file does not replace it");
			Check(VersionFeed::StoreCache(cache, Sign(sk, 11, body), keys, e) && Slurp(cache) == Sign(sk, 11, body), "feed, cache: a higher seq replaces it");
			VersionFeed::SignedFeed read;
			Check(VersionFeed::ReadCache(cache, keys, read) && read.seq == 11 && read.keyId == "k1", "feed, cache: it reads back, verified again");
			DeleteFileW(cache.c_str());
		}

		// Signature failure: a changed byte, a key the loader does not hold, a file cut short. None reaches the map.
		{
			std::string tampered = file;
			tampered[tampered.find("Test::FeedFn") + 6] ^= 1;
			Put(cache, tampered);
			TestDoubles::g_log.clear();
			Check(!VersionFeed::BuildFromCache(cache, keys, kNew, VersionFeed::LoaderCoverage()) && Logged(L"signature does not match"),
				"feed, signature: a changed byte in the cache is refused and logged");
			Put(cache, file);
			const std::vector<VersionFeed::PublicKey> other{ MakeSigner("k1", 9).pub };
			TestDoubles::g_log.clear();
			Check(!VersionFeed::BuildFromCache(cache, other, kNew, VersionFeed::LoaderCoverage()), "feed, signature: a file signed by another key than the loader's is refused");
			Put(cache, file.substr(0, file.size() - 20));
			Check(!VersionFeed::BuildFromCache(cache, keys, kNew, VersionFeed::LoaderCoverage()), "feed, signature: a cut file is refused");
			VersionFeed::SignedFeed read;
			std::string e;
			Check(!VersionFeed::StoreCache(cache, tampered, keys, e) && !VersionFeed::ReadSigned(tampered, keys, read, e), "feed, signature: a changed file is not stored either");
			DeleteFileW(cache.c_str());
			Check(!VersionFeed::BuildFromCache(cache, keys, kNew, VersionFeed::LoaderCoverage()), "feed: no cache, no map");
			Check(!VersionMap::ServesExe(kNew) && VersionMap::SupportedBuilds().size() == 1, "feed: with nothing read the server serves its own exe only");
		}

		// Seeded cache, read onto the real table.
		Put(cache, file);
		TestDoubles::g_log.clear();
		uint64_t seq = 0;
		const auto map = VersionFeed::BuildFromCache(cache, keys, kNew, VersionFeed::LoaderCoverage(), &seq);
		Check(map && seq == 10 && Logged(L"Feed seq 10 (key k1, TEST KEY)"), "feed, seeded: the cache is read, and logged as a TEST key");
		Check(map && map->builds.size() == 3 && !Logged(L"is refused"),
			"feed, coverage: the server's loader coverage is empty, so the new server CLs stay (no loader row is missing)");
		if (map)
		{
			// Without the rule: the client's list (its registry entries and field keys) is not in a server file, and would drop both CLs.
			TestDoubles::g_log.clear();
			const auto strict = VersionFeed::BuildFromCache(cache, keys, kNew, VersionFeed::LoaderCoverage(VersionFeed::Flavor::Client));
			Check(strict && strict->builds.size() == 1 && Logged(L"CL-128786 from the feed is refused"),
				"feed, coverage: the client's coverage would have dropped the new server CLs, which is why the server's is empty");

			VersionMap::SetActiveMap(map);

			// The gate and ServesExe: a second server CL is supported, and only its own exe is served.
			const std::vector<uint32_t> builds = VersionMap::SupportedBuilds();
			Check(builds.size() == 3 && VersionMap::IsSupported(L"1.0.0.0-CL-128786") && VersionMap::IsSupported(L"1.0.0.0-CL-127004") && !VersionMap::IsSupported(L"1.0.0.0-CL-999999"),
				"feed, gate: the server accepts the new CL from the file, still its own, and no other");
			Check(VersionMap::ServesExe(kServer) && VersionMap::ServesExe(kNew) && VersionMap::ServesExe(kThird), "feed, serves: each exe a build line names is served");
			Check(!VersionMap::ServesExe(kNewRebuilt) && !VersionMap::ServesExe(kNewWrongSize) && !VersionMap::ServesExe(kClientExe) && !VersionMap::ServesExe(kWrongStamp),
				"feed, serves: a rebuilt exe of the same CL, another image size, and the client's exe are not");

			// A server block for another exe is never served: each key answers on its own exe only.
			Check(At(Key("Test::FeedFn", kNew), 0x1234), "feed, functions: a key from the file is served on the second server exe");
			Check(!Key("Test::FeedFn", kNewRebuilt).Succeeded() && !Key("Test::FeedFn", kNewWrongSize).Succeeded() && !Key("Test::FeedFn", kServer).Succeeded()
				&& !Key("Test::FeedFn", kThird).Succeeded() && !Key("Test::FeedFn", kClientExe).Succeeded() && !Key("Test::FeedFn", kOtherCL).Succeeded(),
				"feed, functions: and on no other exe, whatever CL, stamp or size it shares");
			Check(At(Key("Test::ThirdFn", kThird), 0x7777) && !Key("Test::ThirdFn", kNew).Succeeded(), "feed, functions: the third CL's block serves on its exe only");
			Check(At(Key("UCrSaveSubsystem::GetSaveData", kNew), 0x5678), "feed, precedence: the same key on the new CL comes from the file");
			// Running on the second CL, the built-in table is not the running exe's, so the file's newer row for CL 127004 is
			// the one the map holds (the client's rule too: the built-in table wins only on the exe the loader is running).
			Check(At(Key("UCrSaveSubsystem::GetSaveData", kServer), 0x9999), "feed, precedence: on another exe the file's row replaces the built-in one");

			// The loader's own entries: from the file's rows on the second CL (the row decides), by their own patterns on a CL
			// whose block has none.
			for (const Global& g : globals)
			{
				const ScanPatterns::PreflightEntry* e = registryEntry(g.registryName);
				if (!e)
					continue;
				Request request = Ask(e->name, static_cast<Kind>(static_cast<int>(e->kind)));
				request.pattern         = e->pattern;
				request.followRel32At   = e->followRel32At;
				request.followRipDispAt = e->followRipDispAt;
				const std::string what = std::string("feed, loader entries: ") + g.registryName;
				TestDoubles::g_rvas[e->pattern] = g.rva;
				Check(At(VersionMap::ResolveLoaderEntry(*e, request, kNew), g.rva), (what + " lands on the file's row on the second server CL").c_str());
				TestDoubles::g_rvas[e->pattern] = 0x1234;
				Check(!VersionMap::ResolveLoaderEntry(*e, request, kNew).Succeeded(), (what + ": a scan that disagrees with the file's row is refused").c_str());
				Check(At(VersionMap::ResolveLoaderEntry(*e, request, kThird), 0x1234), (what + " resolves by its own pattern where the block has no row").c_str());
				TestDoubles::g_rvas.erase(e->pattern);
			}

			// Fields, per build. The loader's own members come from the game (the file's row only cross-checks): the
			// game's answer stands, and a member the game cannot answer is a failure, with the row beside it of no use.
			// A plugin's native member and a size come from the file's row for the exe.
			{
				VersionMap::SetLiveFieldSource(LoaderLive);
				VersionMap::FieldValue v;
				std::string e;
				TestDoubles::g_log.clear();
				Check(VersionMap::SelectField("ModLoader", VersionMap::LoaderFieldRequest("UWorld::GameState", 8, 0x1B0), kNew, v, e) && v.offset == 0x1B8,
					"feed, fields: a loader member is the game's answer on the second server CL, and its row agrees");
				TestDoubles::g_log.clear();
				Check(VersionMap::SelectField("ModLoader", VersionMap::LoaderFieldRequest("UWorld::NetDriver", 8, 0x2B0), kNew, v, e) && v.offset == 0x2C0 && Logged(L"the game wins"),
					"feed, fields: where the file's row disagrees with the game, the game wins, and says so");
				Check(!VersionMap::SelectField("ModLoader", VersionMap::LoaderFieldRequest("ACrCrafter::CraftComponent", 8, 0x100), kNew, v, e) && e.find("reflection") != std::string::npos,
					"feed, fields: a loader member the game cannot answer fails on the second server CL (the loader's compiled offset is not used)");
				Check(VersionMap::SelectField("ModLoader", VersionMap::LoaderFieldRequest("UWorld::GameState", 8, 0x1B0), kNewRebuilt, v, e) && v.offset == 0x1B8,
					"feed, fields: a loader member is the game's answer on a rebuilt exe of that CL too (the game, not a row, is what answers)");
				VersionMap::SetLiveFieldSource(Unreflected);
				Check(Field("FTransform::Translation", PLUGIN_FIELD_NATIVE, 0x18, { OwnRow(127004, 0x20, 0x18) }, kNew, v, e) && v.offset == 0x20 && v.size == 0x18,
					"feed, fields: a plugin's native member is served on the second server CL from the file's row");
				Check(Field("FTransform", 0, 0x40, {}, kNew, v, e) && v.size == 0x40, "feed, fields: a struct size is served from the file's row");
				Check(!Field("UCrGameInstance::ServerSessionName", PLUGIN_FIELD_NATIVE, 0x10, { OwnRow(127004, 0x290, 0x10) }, kNew, v, e),
					"feed, fields: a native member the file has no row for is refused on the second server CL (the plugin's 127004 row is not its row)");
				Check(!Field("FTransform::Translation", PLUGIN_FIELD_NATIVE, 0x18, { OwnRow(127004, 0x20, 0x18) }, kNewRebuilt, v, e),
					"feed, fields: ...and no row of the file serves on a rebuilt exe of that CL");
				VersionMap::SetLiveFieldSource(nullptr);
			}

			// Plugin admission on a second server CL: a migrated plugin that asked for the globals runs.
			{
				PluginInfo info = { "T", "1", "t", "t", 72, PLUGIN_TARGET_SERVER };
				info.gameBuiltCL = 127004;
				info.mapFlags    = PLUGIN_MAP_MIGRATED;
				const VersionMap::PluginGameBuild built = VersionMap::ReadPluginGameBuild(info);
				Check(VersionMap::OffBuildRefusal(kNew, 72, built, true).empty(), "feed, admission: a migrated plugin built for CL 127004 loads on the second server CL");
			}

			VersionMap::SetActiveMap(nullptr);
		}

		// The same file on the server's own exe: the built-in row wins, and the clash is counted and logged.
		{
			TestDoubles::g_log.clear();
			const auto own = VersionFeed::BuildFromCache(cache, keys, kServer, VersionFeed::LoaderCoverage());
			Check(own && Logged(L"1 clashed"), "feed, precedence: on the server's own exe the one row that disagrees is counted as a clash");
			if (own)
			{
				VersionMap::SetActiveMap(own);
				Check(At(Key("UCrSaveSubsystem::GetSaveData", kServer), builtinSaveData), "feed, precedence: the built-in row wins on the exe it was measured on");
				Check(At(Key("Test::FeedFn", kNew), 0x1234) && VersionMap::ServesExe(kNew), "feed, precedence: the file's rows for the other CL are still served");
				VersionMap::SetActiveMap(nullptr);
			}
		}

		// The same cache read twice gives the same map; deleting it returns the server to its table.
		Check(VersionFeed::BuildFromCache(cache, keys, kNew, VersionFeed::LoaderCoverage()) != nullptr, "feed: the seeded cache reads the same each start");
		DeleteFileW(cache.c_str());
		DeleteFileW((cache + L".etag").c_str());
		Check(!VersionMap::ServesExe(kNew) && VersionMap::ServesExe(kServer) && VersionMap::SupportedBuilds().size() == 1,
			"feed: without the cache the server is back on its built-in table");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
