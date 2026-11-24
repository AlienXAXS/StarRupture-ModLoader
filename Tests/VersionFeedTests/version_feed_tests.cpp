// VersionFeedTests: the signed feed reader (memory_scanner/version_feed.cpp)
// against the fixture map the other version-map tests use, with no network.
// Feeds are signed at run time with a throwaway key made here; one file
// (fixtures/test-feed-k1.txt) is signed by Node's crypto with the TEST key in
// slot k1, to check the two Ed25519 implementations agree.
#include "test_doubles.h"

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_feed.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_catalog.h"
#include "third_party/monocypher/monocypher-ed25519.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

using ScanValidation::Kind;
using ScanValidation::Outcome;
using ScanValidation::Request;
using ScanValidation::Result;
using VersionFeed::Coverage;
using VersionFeed::MergeReport;
using VersionFeed::PublicKey;
using VersionFeed::SignedFeed;
using VersionMap::Exe;
using VersionMap::MapData;

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

	bool Logged(const wchar_t* text)
	{
		for (const std::wstring& line : TestDoubles::g_log)
			if (line.find(text) != std::wstring::npos)
				return true;
		return false;
	}

	uintptr_t Base() { return Scanner::GetImageInfo(GetModuleHandleW(nullptr)).base; }
	bool At(const Result& r, uintptr_t rva) { return r.Succeeded() && r.address == Base() + rva; }

	const Exe kRunning{ 128786, 0x22222222, 0x02000000 };

	// ---- signing ----------------------------------------------------------

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
		uint8_t   secret[64]{};
		PublicKey pub;
	};

	Signer MakeSigner(const char* id, uint8_t salt)
	{
		Signer s;
		uint8_t seed[32];
		for (int i = 0; i < 32; ++i)
			seed[i] = static_cast<uint8_t>(salt * 7 + i);
		crypto_ed25519_key_pair(s.secret, s.pub.key.data(), seed);
		s.pub.id = id;
		return s;
	}

	// `region` is everything the signature covers (it ends in a line feed).
	std::string Sign(const Signer& s, const std::string& region, const char* keyId = nullptr)
	{
		uint8_t sig[64];
		crypto_ed25519_sign(sig, s.secret, reinterpret_cast<const uint8_t*>(region.data()), region.size());
		return region + "sig\t" + (keyId ? keyId : s.pub.id.c_str()) + "\t" + Base64(sig, 64) + "\n";
	}

	std::string Region(uint64_t seq, const std::string& body)
	{
		return "format\t1\nseq\t" + std::to_string(seq) + "\n" + body;
	}

	const char* kBuilds =
		"build\t127004\t0x11111111\t0x01000000\tpublic 1\n"
		"build\t128786\t0x22222222\t0x02000000\tpublic_test_branch 2\n";

	std::string Fn(const char* key, uint32_t fp, uint32_t cl, uint32_t stamp, uint32_t size, int kind, const char* pattern, uint32_t rva)
	{
		char b[512];
		sprintf_s(b, "function\t%s\t0x%08X\t%u\t0x%08X\t0x%08X\t%d\t%s\t0\t-1\t-1\t0x%08X\n", key, fp, cl, stamp, size, kind, pattern, rva);
		return b;
	}

	std::string Absent(const char* key, uint32_t fp, uint32_t cl, uint32_t stamp, uint32_t size, int kind)
	{
		char b[512];
		sprintf_s(b, "absent\t%s\t0x%08X\t%u\t0x%08X\t0x%08X\t%d\t-\n", key, fp, cl, stamp, size, kind);
		return b;
	}

	std::string FieldLine(const char* key, uint32_t cl, uint32_t offset)
	{
		char b[256];
		sprintf_s(b, "field\t%s\t%u\t0x%X\t0x4\t0x00\t-\n", key, cl, offset);
		return b;
	}

	bool Read(const std::string& file, const std::vector<PublicKey>& keys, SignedFeed& out, std::string& error)
	{
		return VersionFeed::ReadSigned(file, keys, out, error);
	}

	bool Rejected(const std::string& file, const std::vector<PublicKey>& keys)
	{
		SignedFeed f;
		std::string e;
		const bool ok = Read(file, keys, f, e);
		return !ok && f.map.builds.empty() && f.map.functions.empty() && f.map.fields.empty() && !e.empty();
	}

	// ---- files ------------------------------------------------------------

	std::wstring TempPath(const wchar_t* name)
	{
		wchar_t dir[MAX_PATH]{};
		GetTempPathW(MAX_PATH, dir);
		return std::wstring(dir) + L"vmap_feed_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
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
		GetFileSizeEx(h, &sz);
		s.resize(static_cast<size_t>(sz.QuadPart));
		DWORD n = 0;
		if (!s.empty())
			ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
		CloseHandle(h);
		return s;
	}

	std::wstring FixturePath(const char* name)
	{
		std::string p = __FILE__;
		p = p.substr(0, p.find_last_of("\\/") + 1) + "fixtures\\" + name;
		return std::wstring(p.begin(), p.end());
	}

	// ---- map queries -------------------------------------------------------

	Result Ask(const char* owner, const char* name, Kind kind, const char* pattern, const Exe& running = kRunning)
	{
		TestDoubles::g_log.clear();
		Request r;
		r.name    = name;
		r.kind    = kind;
		r.pattern = pattern;
		return VersionMap::ResolveMapKey(owner, r, {}, running, false);
	}

	const VersionMap::MapFunctionRow* Row(const MapData& m, const char* key, uint32_t cl, uint32_t fp = 0)
	{
		for (const auto& f : m.functions)
			if (f.key == key && f.gameCL == cl && f.fp == fp)
				return &f;
		return nullptr;
	}

	bool HasCL(const MapData& m, uint32_t cl)
	{
		for (const auto& b : m.builds)
			if (b.gameCL == cl)
				return true;
		return false;
	}
}

int main()
{
	const Signer sk1 = MakeSigner("k1", 1);
	const Signer sk2 = MakeSigner("k2", 2);
	const std::vector<PublicKey> keys{ sk1.pub, sk2.pub };

	// ---- verify, then parse -----------------------------------------------
	const std::string body = std::string(kBuilds)
		+ Fn("Test::MapOnly", 0, 128786, 0x22222222, 0x02000000, 1, "AA 01", 0x1000);
	const std::string good = Sign(sk1, Region(100, body));
	{
		SignedFeed f;
		std::string e;
		Check(Read(good, keys, f, e) && f.seq == 100 && f.keyId == "k1" && !f.testKey
			&& f.map.builds.size() == 2 && f.map.functions.size() == 1, "a valid file is admitted");
		Check(Read(Sign(sk2, Region(7, body)), keys, f, e) && f.keyId == "k2" && f.seq == 7, "the second slot verifies");
		Check(!Read(Sign(sk2, Region(7, body), "k1"), keys, f, e) && e.find("does not match") != std::string::npos,
			"a key ID that names the wrong key fails the signature");
	}
	{
		std::string flipped = good;
		flipped[flipped.find("AA 01") + 1] ^= 1;
		Check(Rejected(flipped, keys), "a flipped byte in the data is rejected");
		flipped = good;
		flipped[flipped.size() - 5] = flipped[flipped.size() - 5] == 'A' ? 'B' : 'A';
		Check(Rejected(flipped, keys), "a flipped byte in the signature is rejected");
		Check(Rejected(Sign(sk1, Region(100, body), "k9"), keys), "an unknown key ID is rejected");
		Check(Rejected(good.substr(0, good.size() / 2), keys), "a truncated file is rejected");
		Check(Rejected(good.substr(0, good.size() - 1), keys), "a file without its final line feed is rejected");
		Check(Rejected(good.substr(0, good.find("sig\t")), keys), "a file without a sig line is rejected");
		Check(Rejected(std::string(), keys), "an empty file is rejected");
		Check(Rejected(good, {}), "a loader with no keys reads nothing");
		SignedFeed f;
		std::string e;
		Check(!Read(std::string(VersionFeed::kMaxFileBytes + 1, '\n'), keys, f, e) && e.find("1 MiB") != std::string::npos,
			"an oversize file is rejected");
		// Signed but oversize: the cap comes before everything else.
		std::string big = Region(100, body + std::string(VersionFeed::kMaxFileBytes, 'x'));
		Check(Rejected(Sign(sk1, big), keys), "a signed file over 1 MiB is rejected");
		Check(Rejected(good + good, keys), "a second file appended is rejected");
		Check(Rejected(Sign(sk1, "format\t2\nseq\t100\n" + body), keys), "another format line is rejected");
		Check(Rejected(Sign(sk1, "format\t1\n" + body), keys), "no seq line is rejected");
		Check(Rejected(Sign(sk1, Region(0, body)), keys), "seq 0 is rejected");
		Check(Rejected(Sign(sk1, "format\t1\nseq\t0100\n" + body), keys), "a seq with a leading zero is rejected");
		Check(Rejected(Sign(sk1, "format\t1\nseq\t1x\n" + body), keys), "a seq that is not a number is rejected");
		Check(Rejected(Sign(sk1, "format\t1\nseq\t1\r\n" + body), keys), "CRLF is rejected");
		Check(Rejected(Sign(sk1, Region(100, body + Fn("Owner/Site", 0, 128786, 0x22222222, 0x02000000, 2, "AF 01", 0x1))), keys),
			"a scoped feed row with no fp rejects the file");
		Check(Rejected(Sign(sk1, Region(100, body + "field\tATest::X\t128786\t0x30\t0x4\t0x00\n")), keys), "a known row with too few columns rejects the file");
		Check(Rejected(Sign(sk1, Region(100, body + "field\tATest::X\t128786\t0xzz\t0x4\t0x00\t-\n")), keys), "a known row with bad hex rejects the file");
		Check(Rejected(Sign(sk1, Region(100, body + "\n")), keys), "an empty line rejects the file");
		Check(Rejected(Sign(sk1, Region(100, body + "sig\tk1\tAAAA\n")), keys), "a sig line that is not last rejects the file");
		// A valid signature does not make a bad feed good, and a bad one stays out of the parser.
		Check(Rejected(Sign(sk1, Region(100, std::string(kBuilds) + Fn("Test::MapOnly", 0, 999999, 0x22222222, 0x02000000, 1, "AA 01", 0x1000))), keys),
			"a row with no build line is rejected");
	}
	{
		// A newer feed's rows of a kind this loader does not know are skipped, and the rest of the feed applies.
		const std::string newer = std::string(kBuilds) + "param\tATest\t1\n"
			+ Fn("Test::MapOnly", 0, 128786, 0x22222222, 0x02000000, 1, "AA 01", 0x1000) + "flag\t1\n" + "param\tATest\t2\n";
		SignedFeed f;
		std::string e;
		TestDoubles::g_log.clear();
		Check(Read(Sign(sk1, Region(100, newer)), keys, f, e) && f.map.builds.size() == 2 && f.map.functions.size() == 1
			&& f.map.functions[0].pattern == "AA 01", "a signed feed with rows of unknown kinds is admitted and the rest applies");
		size_t summaries = 0;
		for (const std::wstring& line : TestDoubles::g_log)
			summaries += line.find(L"skipped") != std::wstring::npos;
		Check(summaries == 1 && Logged(L"skipped 3 rows of unknown kinds: param, flag"), "one summary line counts the skipped rows");

		// Extra trailing columns on a known kind are ignored.
		std::string fn = Fn("Test::MapOnly", 0, 128786, 0x22222222, 0x02000000, 1, "AA 01", 0x1000);
		fn.insert(fn.size() - 1, "\tnote\tmore");
		std::string fld = FieldLine("ATest::Native", 128786, 0x30);
		fld.insert(fld.size() - 1, "\tnote");
		Check(Read(Sign(sk1, Region(100, std::string(kBuilds) + fn + fld)), keys, f, e) && f.map.functions.size() == 1
			&& f.map.functions[0].rva == 0x1000 && f.map.fields.size() == 1 && f.map.fields[0].offset == 0x30,
			"extra trailing columns on known rows are ignored and the rows applied");

		// The signature still covers the unknown rows: changing one is a bad signature.
		std::string tampered = Sign(sk1, Region(100, newer));
		tampered[tampered.find("flag") + 1] ^= 1;
		Check(Rejected(tampered, keys), "a tampered unknown row fails the signature");

		// The cache reads a newer feed too.
		const std::wstring path = TempPath(L"newer.cache");
		DeleteFileW(path.c_str());
		Check(VersionFeed::StoreCache(path, Sign(sk1, Region(12, newer)), keys, e), "a feed with unknown kinds is cached");
		const Coverage none;
		uint64_t seq = 0;
		TestDoubles::g_log.clear();
		auto m = VersionFeed::BuildFromCache(path, keys, kRunning, none, &seq);
		Check(m && seq == 12 && Logged(L"Feed seq 12") && !Logged(L"not usable"), "a cached feed with unknown kinds is read on launch");
		DeleteFileW(path.c_str());
	}
	{
		// The Node-signed fixture: the two Ed25519 implementations agree, and the compiled slot k1 is the TEST key.
		const std::string fixture = Slurp(FixturePath("test-feed-k1.txt"));
		SignedFeed f;
		std::string e;
#if defined(VMAP_TRUST_TEST_KEY)
		Check(!fixture.empty() && Read(fixture, VersionFeed::CompiledKeys(), f, e) && f.seq == 1761900000 && f.keyId == "k1"
			&& f.testKey && f.map.builds.size() == 2, "a feed signed by Node with the TEST key verifies against slot k1");
#endif
#if defined(VMAP_TRUST_TEST_KEY)
		Check(!VersionFeed::ReadSigned(fixture, VersionFeed::CompiledKeys(), f, e, VersionFeed::Flavor::Server) && e.find("not a dedicated server file") != std::string::npos,
			"the Node-signed client fixture is refused by a server loader: it verifies, and is the wrong file");
#endif
		std::string flipped = fixture;
		flipped[flipped.find("AA 55") + 1] ^= 1;
		Check(Rejected(flipped, VersionFeed::CompiledKeys()), "the Node-signed fixture with a flipped byte is rejected");
#if defined(VMAP_TRUST_TEST_KEY)
		Check(VersionFeed::CompiledKeys().size() == 2 && VersionFeed::CompiledKeys()[0].id == "k1" && VersionFeed::CompiledKeys()[0].testOnly
			&& VersionFeed::CompiledKeys()[1].id == "k2" && !VersionFeed::CompiledKeys()[1].testOnly, "a test build trusts the TEST key k1 and the release key k2");
#else
		// Built without the define, as a release is: the TEST key is skipped and the fixture signed with it is refused.
		Check(VersionFeed::CompiledKeys().size() == 1 && VersionFeed::CompiledKeys()[0].id == "k2" && !VersionFeed::CompiledKeys()[0].testOnly,
			"a release-style build trusts only the release key k2");
		Check(!Read(fixture, VersionFeed::CompiledKeys(), f, e), "a release-style build refuses a feed signed with the TEST key");
#endif
	}

	// ---- the merge --------------------------------------------------------
	const MapData& baseline = VersionMap::BaselineMap();
	{
		// A row that clashes with the baseline on the running exe: the baseline stays.
		SignedFeed f;
		std::string e;
		Check(Read(Sign(sk1, Region(5, std::string(kBuilds)
			+ Fn("Test::MapOnly", 0, 128786, 0x22222222, 0x02000000, 1, "AA 99", 0x9900)
			+ Fn("Test::MapOnly", 0, 127004, 0x11111111, 0x01000000, 1, "AA 00", 0x0F00))), keys, f, e), "clash feed reads");
		MergeReport rep;
		const MapData merged = VersionFeed::Merge(baseline, f.map, kRunning, rep);
		const auto* r = Row(merged, "Test::MapOnly", 128786);
		Check(r && r->pattern == "AA 01" && r->rva == 0x1000, "a clashing row on the running exe leaves the baseline in force");
		Check(rep.clashes == 1 && rep.discarded == 2 + 2, "the clash and the repeats are counted as discarded");
		Check(merged.functions.size() == baseline.functions.size() && merged.fields.size() == baseline.fields.size(), "an identical feed adds no rows");
		Check(!baseline.absentFields.empty() && merged.absentFields.size() == baseline.absentFields.size(),
			"the loader's compiled absent fields survive a merge, as a feed has none");
	}
	{
		// A feed locator over a baseline `absent` row on the same exe is used.
		SignedFeed f;
		std::string e;
		Check(Read(Sign(sk1, Region(5, std::string(kBuilds) + Fn("Test::Absent", 0, 128786, 0x22222222, 0x02000000, 1, "AB 77", 0x7700))), keys, f, e), "absent-fill feed reads");
		MergeReport rep;
		const MapData merged = VersionFeed::Merge(baseline, f.map, kRunning, rep);
		const auto* r = Row(merged, "Test::Absent", 128786);
		Check(r && !r->absent && r->rva == 0x7700 && rep.absentFilled == 1, "a feed locator replaces a baseline absent row on the same exe");
		VersionMap::SetActiveMap(std::make_shared<const MapData>(merged));
		TestDoubles::g_rvas["AB 77"] = 0x7700;
		Check(At(Ask("Listed", "Test::Absent", Kind::FunctionStart, ""), 0x7700), "the filled key resolves");
		VersionMap::SetActiveMap(nullptr);
		// A feed absent over a baseline locator changes nothing.
		Check(Read(Sign(sk1, Region(6, std::string(kBuilds) + Absent("Test::MapOnly", 0, 128786, 0x22222222, 0x02000000, 1))), keys, f, e), "absent feed reads");
		const MapData merged2 = VersionFeed::Merge(baseline, f.map, kRunning, rep);
		const auto* r2 = Row(merged2, "Test::MapOnly", 128786);
		Check(r2 && !r2->absent && r2->rva == 0x1000, "a feed absent never replaces a baseline locator on the running exe");
	}
	{
		// One scoped key and exe: a baseline absent for fp1 and a feed locator for fp2 both survive.
		const uint32_t fp1 = VersionMap::Fingerprint(2, "AF 01", 0, -1, -1, 0);
		const uint32_t fp2 = VersionMap::Fingerprint(2, "AF 02", 0, -1, -1, 0);
		MapData base;
		base.builds.push_back({ 128786, 0x22222222, 0x02000000, "b" });
		VersionMap::MapFunctionRow a;
		a.key = "VmapProbe/MergeSite"; a.gameCL = 128786; a.imageStamp = 0x22222222; a.imageSize = 0x02000000;
		a.kind = 2; a.fp = fp1; a.absent = true;
		base.functions.push_back(a);
		SignedFeed f;
		std::string e;
		Check(Read(Sign(sk1, Region(5, std::string("build\t128786\t0x22222222\t0x02000000\tb\n")
			+ Fn("VmapProbe/MergeSite", fp2, 128786, 0x22222222, 0x02000000, 2, "AF 02", 0x9000))), keys, f, e), "scoped feed reads");
		MergeReport rep;
		const MapData merged = VersionFeed::Merge(base, f.map, kRunning, rep);
		Check(Row(merged, "VmapProbe/MergeSite", 128786, fp1) && Row(merged, "VmapProbe/MergeSite", 128786, fp1)->absent
			&& Row(merged, "VmapProbe/MergeSite", 128786, fp2) && !Row(merged, "VmapProbe/MergeSite", 128786, fp2)->absent
			&& merged.functions.size() == 2, "an absent for fp1 and a locator for fp2 both survive the merge");
		VersionMap::SetActiveMap(std::make_shared<const MapData>(merged));
		TestDoubles::g_rvas["AF 01"] = 0x9100;
		TestDoubles::g_rvas["AF 02"] = 0x9000;
		Result r1 = Ask("VmapProbe", "MergeSite", Kind::InFunction, "AF 01");
		Check(r1.outcome == Outcome::NoMatch && Logged(L"(refused)"), "an fp1 request is refused");
		Check(At(Ask("VmapProbe", "MergeSite", Kind::InFunction, "AF 02"), 0x9000), "an fp2 request uses the locator");
		VersionMap::SetActiveMap(nullptr);

		// The same with both rows in the baseline and no feed.
		MapData both = base;
		VersionMap::MapFunctionRow l = a;
		l.fp = fp2; l.absent = false; l.pattern = "AF 02"; l.rva = 0x9000;
		both.functions.push_back(l);
		const MapData mergedBoth = VersionFeed::Merge(both, MapData{}, kRunning, rep);
		Check(mergedBoth.functions.size() == 2, "both rows in the baseline survive a merge with no feed");

		// A scoped baseline row with fp 0 matches no request: dropped and counted.
		MapData noFp = base;
		VersionMap::MapFunctionRow z = a;
		z.key = "VmapProbe/NoFp"; z.fp = 0; z.absent = false; z.pattern = "AF 03"; z.rva = 0x9300;
		noFp.functions.push_back(z);
		const MapData mergedNoFp = VersionFeed::Merge(noFp, MapData{}, kRunning, rep);
		Check(rep.droppedNoFp == 1 && !Row(mergedNoFp, "VmapProbe/NoFp", 128786, 0), "a scoped baseline row with fp 0 is dropped and counted");
	}
	{
		// A feed row for another exe of a known CL: one row per key and CL, and Select does not refuse it.
		const Exe rebuilt{ 127004, 0x33333333, 0x03000000 };
		SignedFeed f;
		std::string e;
		Check(Read(Sign(sk1, Region(5, std::string("build\t127004\t0x33333333\t0x03000000\trebuilt\n")
			+ Fn("Test::MapOnly", 0, 127004, 0x33333333, 0x03000000, 1, "AA 77", 0x1234))), keys, f, e), "rebuilt-exe feed reads");
		MergeReport rep;
		const MapData merged = VersionFeed::Merge(baseline, f.map, rebuilt, rep);
		size_t rows = 0;
		for (const auto& r : merged.functions)
			rows += r.key == "Test::MapOnly" && r.gameCL == 127004;
		Check(rows == 1 && Row(merged, "Test::MapOnly", 127004)->imageStamp == 0x33333333, "another exe of a known CL gives one row per key and CL");
		VersionMap::SetActiveMap(std::make_shared<const MapData>(merged));
		TestDoubles::g_rvas["AA 77"] = 0x1234;
		Check(At(Ask("Plug", "Test::MapOnly", Kind::FunctionStart, "", rebuilt), 0x1234), "Select does not refuse the rebuilt exe's row");
		VersionMap::SetActiveMap(nullptr);
		// The same feed read on the exe the baseline has: the baseline's row stays.
		const MapData onOld = VersionFeed::Merge(baseline, f.map, Exe{ 127004, 0x11111111, 0x01000000 }, rep);
		Check(Row(onOld, "Test::MapOnly", 127004)->imageStamp == 0x11111111 && rep.clashes >= 1, "on the baseline's own exe the baseline's rows stay");
	}

	// A struct size is an ordinary field row whose key is the class alone (offset 0, the size, mask 0).
	{
		SignedFeed f;
		std::string e;
		Check(Read(Sign(sk1, Region(5, std::string(kBuilds) + "field\tFCrEnviroWaveSettings\t128786\t0x0\t0x2C\t0x00\t-\n")), keys, f, e)
			&& f.map.fields.size() == 1 && f.map.fields[0].key == "FCrEnviroWaveSettings" && f.map.fields[0].gameCL == 128786
			&& f.map.fields[0].offset == 0 && f.map.fields[0].size == 0x2C && f.map.fields[0].boolMask == 0,
			"a class-keyed size row (no ::) parses as a field row");
	}

	// ---- per-CL coverage ---------------------------------------------------
	{
		Coverage cov;
		cov.functions.push_back({ "Test::MapOnly", 0 });
		cov.functions.push_back({ "ModLoader/Test::Site", 0x0BD817FA });
		cov.fields.push_back("ATest::Native");
		const std::string b130 = "build\t130000\t0x44444444\t0x04000000\tfeed\n";
		const auto full = [&](const char* extra)
		{
			return std::string(kBuilds) + b130
				+ Fn("Test::MapOnly", 0, 130000, 0x44444444, 0x04000000, 1, "AA 30", 0x3000)
				+ Fn("ModLoader/Test::Site", 0x0BD817FA, 130000, 0x44444444, 0x04000000, 2, "B0 30", 0x3100)
				+ extra
				+ FieldLine("ATest::Native", 130000, 0x30);
		};
		const auto cover = [&](const std::string& feedBody, std::vector<std::pair<uint32_t, size_t>>& refused, MapData& merged)
		{
			SignedFeed f;
			std::string e;
			if (!Read(Sign(sk1, Region(5, feedBody)), keys, f, e))
				return false;
			MergeReport rep;
			merged = VersionFeed::Merge(baseline, f.map, kRunning, rep);
			refused = VersionFeed::DropUncovered(merged, baseline, cov);
			return true;
		};
		std::vector<std::pair<uint32_t, size_t>> refused;
		MapData merged;
		Check(cover(full(""), refused, merged) && refused.empty() && HasCL(merged, 130000), "a CL only the feed knows, with every needed key, is admitted");
		VersionMap::SetActiveMap(std::make_shared<const MapData>(merged));
		Check(VersionMap::IsSupported(L"1.0.0.0-CL-130000") && VersionMap::IsSupported(L"1.0.0.0-CL-127004"), "the gate passes the feed's CL and the baseline's");
		VersionMap::SetActiveMap(nullptr);
		Check(!VersionMap::IsSupported(L"1.0.0.0-CL-130000"), "and the baseline alone does not");
		Check(VersionFeed::MissingKeys(merged, 130000, cov) == 0, "no key is missing on the admitted CL");

		// Missing the global key.
		const std::string noGlobal = std::string(kBuilds) + b130
			+ Fn("ModLoader/Test::Site", 0x0BD817FA, 130000, 0x44444444, 0x04000000, 2, "B0 30", 0x3100) + FieldLine("ATest::Native", 130000, 0x30);
		Check(cover(noGlobal, refused, merged) && refused.size() == 1 && refused[0] == std::make_pair(130000u, size_t(1)) && !HasCL(merged, 130000)
			&& Row(merged, "ModLoader/Test::Site", 130000, 0x0BD817FA) == nullptr, "a CL missing a needed key is refused, with its rows, and the count is given");
		// The scoped entry has a locator only under another fp.
		const std::string otherFp = std::string(kBuilds) + b130
			+ Fn("Test::MapOnly", 0, 130000, 0x44444444, 0x04000000, 1, "AA 30", 0x3000)
			+ Fn("ModLoader/Test::Site", 0x11111111, 130000, 0x44444444, 0x04000000, 2, "B0 30", 0x3100) + FieldLine("ATest::Native", 130000, 0x30);
		Check(cover(otherFp, refused, merged) && refused.size() == 1 && refused[0].second == 1, "a required ModLoader/ entry with a locator only under another fp is refused");
		// Only an absent row.
		const std::string onlyAbsent = std::string(kBuilds) + b130
			+ Fn("Test::MapOnly", 0, 130000, 0x44444444, 0x04000000, 1, "AA 30", 0x3000)
			+ Absent("ModLoader/Test::Site", 0x0BD817FA, 130000, 0x44444444, 0x04000000, 2) + FieldLine("ATest::Native", 130000, 0x30);
		Check(cover(onlyAbsent, refused, merged) && refused.size() == 1, "an absent row is not a locator");
		// No field row.
		const std::string noField = std::string(kBuilds) + b130
			+ Fn("Test::MapOnly", 0, 130000, 0x44444444, 0x04000000, 1, "AA 30", 0x3000)
			+ Fn("ModLoader/Test::Site", 0x0BD817FA, 130000, 0x44444444, 0x04000000, 2, "B0 30", 0x3100);
		Check(cover(noField, refused, merged) && refused.size() == 1, "a CL with no row for a loader field key is refused");
		// A CL the baseline already holds is not gated.
		Check(cover(std::string(kBuilds), refused, merged) && refused.empty() && HasCL(merged, 128786), "a CL the baseline holds passes the gate unchanged");
	}

	// ---- the real registry's coverage --------------------------------------
	{
		const Coverage real = VersionFeed::LoaderCoverage();
		size_t required = 0;
		for (const auto& e : ScanPatterns::PreflightRegistry)
			required += e.required;
		Check(real.functions.size() == required && real.fields.size() == std::size(VersionMap::kLoaderFieldKeys),
			"the loader's coverage is its required registry entries and its field keys");
	}

	// ---- the cache ---------------------------------------------------------
	{
		const std::wstring path = TempPath(L"a.cache");
		DeleteFileW(path.c_str());
		std::string e;
		Check(VersionFeed::StoreCache(path, Sign(sk1, Region(10, body)), keys, e) && Slurp(path) == Sign(sk1, Region(10, body)), "the first verified file is cached");
		Check(!Exists(path + L".tmp"), "the temp file is gone after a replace");
		const std::string at10 = Slurp(path);
		Check(!VersionFeed::StoreCache(path, Sign(sk1, Region(9, body)), keys, e) && Slurp(path) == at10
			&& e.find("not above") != std::string::npos, "a lower seq never replaces the cache");
		Check(!VersionFeed::StoreCache(path, Sign(sk2, Region(10, body)), keys, e) && Slurp(path) == at10, "an equal seq never replaces the cache");
		std::string tampered = Sign(sk1, Region(50, body));
		tampered[tampered.find("AA 01") + 1] ^= 1;
		Check(!VersionFeed::StoreCache(path, tampered, keys, e) && Slurp(path) == at10, "a file that fails verification leaves the cache as it was");
		Check(VersionFeed::StoreCache(path, Sign(sk1, Region(11, body)), keys, e) && Slurp(path) == Sign(sk1, Region(11, body)), "a higher seq replaces it");

		// Read back: verified on every launch.
		const Coverage none;
		uint64_t seq = 0;
		TestDoubles::g_log.clear();
		auto m = VersionFeed::BuildFromCache(path, keys, kRunning, none, &seq);
		Check(m && seq == 11 && !m->builds.empty() && Logged(L"Feed seq 11"), "the cache is verified, merged and logged");
		// One line per build in the file, after the summary.
		{
			const std::string twoBuilds = std::string(kBuilds) + "build\t130000\t0x44444444\t0x04000000\tfeed\n"
				+ Fn("Test::MapOnly", 0, 128786, 0x22222222, 0x02000000, 1, "AA 01", 0x1000)
				+ Fn("Test::MapOnly", 0, 130000, 0x44444444, 0x04000000, 1, "AA 30", 0x3000)
				+ FieldLine("ATest::Native", 130000, 0x30);
			Put(path, Sign(sk1, Region(12, twoBuilds)));
			TestDoubles::g_log.clear();
			Check(VersionFeed::BuildFromCache(path, keys, kRunning, none) != nullptr
				&& Logged(L"CL 128786 (this exe): 1 rows")
				&& Logged(L"CL 130000 (other exe, stamp 0x44444444, size 0x04000000): 2 rows, a build the loader does not have, added to the version map"),
				"the log names each build in the file, whether it is this exe, and what the merge did with it");		}
		Put(path, at10.substr(0, at10.size() - 20));
		TestDoubles::g_log.clear();
		Check(!VersionFeed::BuildFromCache(path, keys, kRunning, none) && Logged(L"not usable"), "a damaged cache leaves the loader's own map in force");
		// A cache signed with a key since dropped: unverifiable, ignored; the next file may take its place.
		Put(path, Sign(sk1, Region(900, body)));
		const std::vector<PublicKey> onlyK2{ sk2.pub };
		Check(!VersionFeed::BuildFromCache(path, onlyK2, kRunning, none), "a cache signed with a dropped key is ignored");
		Check(VersionFeed::StoreCache(path, Sign(sk2, Region(5, body)), onlyK2, e), "and counts as seq 0 for the next file");
		// Missing and oversize.
		DeleteFileW(path.c_str());
		TestDoubles::g_log.clear();
		Check(!VersionFeed::BuildFromCache(path, keys, kRunning, none) && TestDoubles::g_log.empty(), "no cache is not an event");
		Put(path, std::string(VersionFeed::kMaxFileBytes + 1, 'x'));
		Check(!VersionFeed::BuildFromCache(path, keys, kRunning, none), "an oversize cache is not read");
		Check(!VersionFeed::BuildFromCache(path, {}, kRunning, none), "a loader with no keys reads no cache");
		DeleteFileW(path.c_str());
	}

	// ---- the two files: a loader reads its own and refuses the other's ----------
	// A server file and a client file share the grammar and the signature, and
	// differ in the source label of their build lines ("server ..."). This is the
	// client build's test: the server flavor is passed explicitly to cover both
	// directions here; the server build's own cases are in version_map_server_tests.
	{
		using VersionFeed::Flavor;
		const std::string serverBody = "build\t127004\t0x2E3595FF\t0x0E2A8000\tserver steam_build 1\n"
			"build\t128786\t0x33333333\t0x03000000\tserver steam_build 2\n"
			+ Fn("Test::ServerOnly", 0, 128786, 0x33333333, 0x03000000, 1, "-", 0x1000);
		const std::string serverFile = Sign(sk1, Region(30, serverBody));
		const std::string clientFile = Sign(sk1, Region(30, body));
		SignedFeed f;
		std::string e;

		Check(Read(clientFile, keys, f, e) && f.map.builds.size() == 2 && VersionFeed::ReadSigned(clientFile, keys, f, e, Flavor::Client),
			"flavor: a client file is read by a client loader");
		Check(VersionFeed::ReadSigned(serverFile, keys, f, e, Flavor::Server) && f.seq == 30 && f.map.builds.size() == 2,
			"flavor: a server file is read by a server loader");
		Check(!VersionFeed::ReadSigned(serverFile, keys, f, e, Flavor::Client) && e.find("dedicated server file") != std::string::npos && f.map.builds.empty(),
			"flavor: a client loader refuses a server file, and says why");
		Check(!VersionFeed::ReadSigned(clientFile, keys, f, e, Flavor::Server) && e.find("not a dedicated server file") != std::string::npos && f.map.builds.empty(),
			"flavor: a server loader refuses a client file, and says why");

		// Each label has to agree: one build of the other kind and the file is the other's.
		const std::string mixed = Sign(sk1, Region(31, "build\t127004\t0x2E3595FF\t0x0E2A8000\tserver steam_build 1\n"
			"build\t128786\t0x22222222\t0x02000000\tpublic_test_branch 2\n"));
		Check(!VersionFeed::ReadSigned(mixed, keys, f, e, Flavor::Server) && !VersionFeed::ReadSigned(mixed, keys, f, e, Flavor::Client),
			"flavor: a file with a build line of each kind is refused by both");
		// The label is "server " with the space: "servers" or "Server" are not it.
		const std::string lookalike = Sign(sk1, Region(32, "build\t127004\t0x2E3595FF\t0x0E2A8000\tservers steam_build 1\n"));
		Check(!VersionFeed::ReadSigned(lookalike, keys, f, e, Flavor::Server) && VersionFeed::ReadSigned(lookalike, keys, f, e, Flavor::Client),
			"flavor: only the label \"server \" marks a server file");

		// A server file names a server exe: with no build line there is nothing to tell it by.
		const std::string noBuilds = Sign(sk1, Region(33, ""));
		const bool clientOk = VersionFeed::ReadSigned(noBuilds, keys, f, e, Flavor::Client);
		Check(!VersionFeed::ReadSigned(noBuilds, keys, f, e, Flavor::Server) && (clientOk || !e.empty()),
			"flavor: a server loader refuses a file with no build line");

		// The check follows the signature: an unsigned or wrongly signed server file says nothing of flavor.
		std::string forged = serverFile;
		forged[forged.find("Test::ServerOnly") + 6] ^= 1;
		Check(!VersionFeed::ReadSigned(forged, keys, f, e, Flavor::Server) && e.find("signature") != std::string::npos,
			"flavor: a tampered server file fails on its signature first");

		// The loader's own coverage: the client's list is what it was; the server's is empty, so a server CL in the file is
		// never dropped for a key the server file does not carry.
		const Coverage clientCoverage = VersionFeed::LoaderCoverage(Flavor::Client);
		const Coverage serverCoverage = VersionFeed::LoaderCoverage(Flavor::Server);
		Check(!clientCoverage.functions.empty() && !clientCoverage.fields.empty() && VersionFeed::LoaderCoverage().functions.size() == clientCoverage.functions.size(),
			"flavor: this client build's coverage is the loader's registry entries and field keys");
		Check(serverCoverage.functions.empty() && serverCoverage.fields.empty(), "flavor: the server's coverage is empty");

		// The cache: a server file never lands in a client cache, and the one cached stays.
		const std::wstring path = TempPath(L"flavor.cache");
		DeleteFileW(path.c_str());
		Check(!VersionFeed::StoreCache(path, serverFile, keys, e) && !Exists(path) && !Exists(path + L".tmp"),
			"flavor: a client loader's cache takes no server file");
		Check(VersionFeed::StoreCache(path, clientFile, keys, e) && !VersionFeed::StoreCache(path, Sign(sk1, Region(40, serverBody)), keys, e)
			&& Slurp(path) == clientFile, "flavor: ...and keeps the client file it has");
		DeleteFileW(path.c_str());
		Check(VersionFeed::StoreCache(path, serverFile, keys, e, Flavor::Server) && Slurp(path) == serverFile
			&& VersionFeed::ReadCache(path, keys, f, Flavor::Server) && !VersionFeed::ReadCache(path, keys, f, Flavor::Client),
			"flavor: a server loader's cache takes a server file, which only a server loader reads");
		TestDoubles::g_log.clear();
		Check(!VersionFeed::BuildFromCache(path, keys, kRunning, Coverage{}) && Logged(L"dedicated server file"),
			"flavor: a client loader finding a server file in its cache logs the refusal and runs on its own map");
		DeleteFileW(path.c_str());
	}

#if defined(VMAP_TRUST_TEST_KEY)
	// ---- one generation per process (the cache holds the TEST-key fixture) ----
	{
		const std::wstring path = TempPath(L"b.cache");
		Put(path, Slurp(FixturePath("test-feed-k1.txt")));
		const MapData* before = &VersionMap::ActiveMap();
		TestDoubles::g_log.clear();
		Check(VersionFeed::ActivateCached(path), "ActivateCached puts a verified feed in force");
		Check(&VersionMap::ActiveMap() != before && Logged(L"TEST KEY") && Logged(L"CL-130001 from the feed is refused"),
			"the fixture's CL lacks the loader's keys and is refused; the log names the TEST key");
		Check(!VersionMap::IsSupported(L"1.0.0.0-CL-130001"), "the refused CL is not supported");
		const MapData* once = &VersionMap::ActiveMap();
		Put(path, std::string("garbage"));
		Check(!VersionFeed::ActivateCached(path) && &VersionMap::ActiveMap() == once, "a second call changes nothing");
		DeleteFileW(path.c_str());
	}
#endif

	// ---- the signed plugin catalog: verify-only entry, cache, accessor ------
	{
		const std::string catBody = std::string("kind\tplugin-catalog\n")
			+ "plugin\tAlpha\t1.2.0\n"
			+ "plugin\tBeta Two\t0.3\n"
			+ "feature\tAlpha\tfast-mode\t-\tFast mode\n"
			+ "feature\tBeta Two\tsmoke\t128786\tSmoke effect\n";
		const std::string cat10 = Sign(sk1, Region(10, catBody));
		const std::wstring path = TempPath(L"catalog.cache");
		DeleteFileW(path.c_str());
		std::string e;
		const auto Served = [] { const auto t = PluginCatalog::AccessorText(); return t ? *t : std::string("<none>"); };

		// VerifySigned agrees with ReadSigned on the map files: same seq, key and verdict.
		{
			SignedFeed f;
			VersionFeed::VerifiedBody v;
			Check(Read(good, keys, f, e) && VersionFeed::VerifySigned(good, keys, v, e)
				&& v.seq == f.seq && v.keyId == f.keyId && v.testKey == f.testKey && v.body == Region(100, body),
				"VerifySigned and ReadSigned agree on a valid map file, and the body is everything before the sig line");
			std::string flipped = good;
			flipped[flipped.find("AA 01") + 1] ^= 1;
			Check(!Read(flipped, keys, f, e) && !VersionFeed::VerifySigned(flipped, keys, v, e) && !e.empty(),
				"both refuse a map file with a flipped byte");
			Check(!VersionFeed::VerifySigned(Sign(sk1, Region(100, body), "k9"), keys, v, e), "VerifySigned refuses an unknown key ID");
			Check(!VersionFeed::VerifySigned(good.substr(0, good.size() - 1), keys, v, e), "VerifySigned refuses a file that does not end in a line feed");
#if defined(VMAP_TRUST_TEST_KEY)
			const std::string fixture = Slurp(FixturePath("test-feed-k1.txt"));
			Check(Read(fixture, VersionFeed::CompiledKeys(), f, e)
				&& VersionFeed::VerifySigned(fixture, VersionFeed::CompiledKeys(), v, e)
				&& v.seq == f.seq && v.seq == 1761900000 && v.keyId == "k1" && v.testKey,
				"VerifySigned agrees with ReadSigned on the Node-signed fixture");
#endif
		}

		// Nothing stored yet.
		Check(PluginCatalog::AccessorText() == nullptr && PluginCatalog::CopyAccessorText(nullptr, 0) == -1,
			"the accessor has no text, and answers -1, before any catalog");
		PluginCatalog::LoadCache(path, keys);
		Check(PluginCatalog::AccessorText() == nullptr && PluginCatalog::CopyAccessorText(nullptr, 0) == -1,
			"a missing cache file loads nothing, quietly");
		{
			char guard[4] = { 'x', 'x', 'x', 'x' };
			Check(PluginCatalog::CopyAccessorText(guard, sizeof(guard)) == -1 && guard[0] == 'x', "-1 writes nothing to the buffer");
		}

		// A good file is accepted and cached.
		Check(PluginCatalog::Store(path, cat10, keys, e) == PluginCatalog::Outcome::Stored, "a good signed catalog is stored");
		Check(Slurp(path) == cat10, "the cache file is the signed file, byte for byte");
		const std::string accessor10 = "format\t1\nseq\t10\n" + catBody;
		Check(Served() == accessor10, "the accessor serves the stored catalog");

		// Refusals keep the last good file.
		Check(PluginCatalog::Store(path, cat10, keys, e) == PluginCatalog::Outcome::Unchanged && Slurp(path) == cat10,
			"the same file again is unchanged");
		Check(PluginCatalog::Store(path, Sign(sk2, Region(10, catBody)), keys, e) == PluginCatalog::Outcome::Unchanged && Slurp(path) == cat10,
			"the same body signed by another key is unchanged");
		Check(PluginCatalog::Store(path, Sign(sk1, Region(9, catBody)), keys, e) == PluginCatalog::Outcome::RefusedRollback
			&& e.find("below") != std::string::npos && Slurp(path) == cat10, "a lower seq is refused as a rollback");
		Check(PluginCatalog::Store(path, Sign(sk1, Region(10, catBody + "feature\tAlpha\textra\t*\tExtra\n")), keys, e) == PluginCatalog::Outcome::RefusedConflict
			&& Slurp(path) == cat10, "the same seq with another body is refused as a conflict");
		std::string flipped = cat10;
		flipped[flipped.find("Alpha") + 1] ^= 1;
		Check(PluginCatalog::Store(path, flipped, keys, e) == PluginCatalog::Outcome::Rejected && Slurp(path) == cat10,
			"a bad signature is rejected and the last good file stays");
		Check(PluginCatalog::Store(path, Sign(sk1, "format\t2\nseq\t20\n" + catBody), keys, e) == PluginCatalog::Outcome::Rejected
			&& e.find("format") != std::string::npos && Slurp(path) == cat10, "an unknown header version is rejected");
		Check(PluginCatalog::Store(path, Sign(sk1, Region(20, catBody), "k9"), keys, e) == PluginCatalog::Outcome::Rejected && Slurp(path) == cat10,
			"an unknown key ID is rejected");
		Check(PluginCatalog::Store(path, Sign(sk1, Region(20, std::string("kind\tversion-map\n") + "plugin\tAlpha\t1.0\n")), keys, e) == PluginCatalog::Outcome::Rejected
			&& Slurp(path) == cat10, "a signed file of another kind is rejected");
		Check(PluginCatalog::Store(path, Sign(sk1, Region(20, std::string("kind\tplugin-catalog\n"))), keys, e) == PluginCatalog::Outcome::Rejected
			&& Slurp(path) == cat10, "a signed catalog with no plugin is rejected");
		Check(PluginCatalog::Store(path, cat10.substr(0, cat10.size() - 1), keys, e) == PluginCatalog::Outcome::Rejected && Slurp(path) == cat10,
			"a file cut before its last line feed is rejected");
		Check(PluginCatalog::Store(path, cat10.substr(0, cat10.size() - 20), keys, e) == PluginCatalog::Outcome::Rejected && Slurp(path) == cat10,
			"a file cut inside the signature is rejected");
		Check(PluginCatalog::Store(path, cat10.substr(0, cat10.find("sig\t")), keys, e) == PluginCatalog::Outcome::Rejected && Slurp(path) == cat10,
			"a file with no sig line is rejected");
		Check(PluginCatalog::Store(path, "", keys, e) == PluginCatalog::Outcome::Rejected && Slurp(path) == cat10, "an empty file is rejected");
		Check(Served() == accessor10, "the accessor still serves the last good catalog after every refusal");

		// A higher seq replaces the cache and the accessor.
		const std::string body11 = std::string("kind\tplugin-catalog\n") + "plugin\tAlpha\t1.3.0\n";
		const std::string cat11 = Sign(sk2, Region(11, body11));
		Check(PluginCatalog::Store(path, cat11, keys, e) == PluginCatalog::Outcome::Stored && Slurp(path) == cat11
			&& Served() == "format\t1\nseq\t11\n" + body11, "a higher seq replaces the cache and the accessor");

		// The copy convention (the same as GetDeclaredFeatures), on the stored text.
		{
			const std::string text = "format\t1\nseq\t11\n" + body11;
			const int len = static_cast<int>(text.size());
			char big[512];
			Check(PluginCatalog::CopyAccessorText(nullptr, 0) == len, "a null buffer with size 0 measures");
			char marker[2] = { 'x', 'x' };
			Check(PluginCatalog::CopyAccessorText(marker, 0) == len && marker[0] == 'x', "size 0 writes nothing");
			Check(PluginCatalog::CopyAccessorText(marker, 1) == len && marker[0] == '\0' && marker[1] == 'x', "size 1 writes just the NUL");
			Check(PluginCatalog::CopyAccessorText(big, sizeof(big)) == len && text == big, "a big buffer gets the whole text, NUL-terminated");
			char exact[512];
			Check(PluginCatalog::CopyAccessorText(exact, static_cast<size_t>(len) + 1) == len && text == exact, "a buffer of length + 1 holds it all");
			char cut[512];
			std::memset(cut, 'x', sizeof(cut));
			Check(PluginCatalog::CopyAccessorText(cut, static_cast<size_t>(len)) == len && std::string(cut) == text.substr(0, text.size() - 1) && cut[len - 1] == '\0',
				"a buffer of exactly the length is cut by one and keeps its NUL");
			Check(PluginCatalog::CopyAccessorText(cut, 7) == len && std::string(cut) == text.substr(0, 6), "a small buffer is cut to size - 1 plus NUL, the full length returned");
		}

		// A title with multi-byte characters is served byte for byte, and the length counts bytes.
		{
			const std::string utf8Body = std::string("kind\tplugin-catalog\n") + "plugin\tAlpha\t1.3.0\n"
				+ "feature\tAlpha\tcafe\t-\tCaf\xC3\xA9 \xE2\x9C\xA6 \xF0\x9F\x97\xBA\n";
			const std::string utf8Text = "format\t1\nseq\t12\n" + utf8Body;
			Check(PluginCatalog::Store(path, Sign(sk1, Region(12, utf8Body)), keys, e) == PluginCatalog::Outcome::Stored
				&& Served() == utf8Text && PluginCatalog::CopyAccessorText(nullptr, 0) == static_cast<int>(utf8Text.size()),
				"a title with two-, three- and four-byte characters is stored and measured in bytes");
			std::string buf(utf8Text.size() + 1, '\0');
			Check(PluginCatalog::CopyAccessorText(buf.data(), buf.size()) == static_cast<int>(utf8Text.size()) && buf.compare(0, utf8Text.size(), utf8Text) == 0,
				"and copied whole");
			// Put the last good text back for the cache tests below.
			Check(PluginCatalog::Store(path, Sign(sk1, Region(11, body11)), keys, e) == PluginCatalog::Outcome::RefusedRollback && Served() == utf8Text,
				"a rollback after it leaves it served");
			Check(PluginCatalog::Store(path, Sign(sk1, Region(13, body11)), keys, e) == PluginCatalog::Outcome::Stored
				&& Served() == "format\t1\nseq\t13\n" + body11, "and a higher seq replaces it");
		}

		// The cache survives a reload from disk, and a cache that no longer verifies loads nothing new.
		{
			const std::wstring other = TempPath(L"catalog2.cache");
			Put(other, cat10);
			PluginCatalog::LoadCache(other, keys);
			Check(Served() == accessor10, "LoadCache reads the cache file back into the accessor");
			Put(other, flipped);
			TestDoubles::g_log.clear();
			PluginCatalog::LoadCache(other, keys);
			Check(Served() == accessor10 && Logged(L"not usable"),
				"a cache file that fails verification is logged and leaves the copy in memory as it was");
			PluginCatalog::LoadCache(other, std::vector<PublicKey>{});
			Check(Served() == accessor10, "no keys loads nothing");
			DeleteFileW(other.c_str());
		}
		DeleteFileW(path.c_str());
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
