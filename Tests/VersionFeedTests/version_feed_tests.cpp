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

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
