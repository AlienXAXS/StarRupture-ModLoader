#include "memory_scanner/version_feed.h"
#include "hooks/game/scan_patterns.h"
#include "logging/logger.h"
#include "third_party/monocypher/monocypher-ed25519.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace VersionFeed
{
	using VersionMap::Exe;
	using VersionMap::MapBuild;
	using VersionMap::MapData;
	using VersionMap::MapFieldRow;
	using VersionMap::MapFunctionRow;

	namespace
	{
		// The public half of each signing key, as 64 hex digits; empty = the
		// slot holds no key. With both empty the loader reads and fetches no
		// feed. Two slots so a key can be rotated: a loader release adds the
		// new key to the free slot, and a later one drops the old.
		//
		// k1 is a TEST key, generated locally for the unit tests and test
		// feeds. Only a build that defines VMAP_TRUST_TEST_KEY (the tests,
		// sr-aobscan and a CI build with /p:VmapTrustTestKey=true) trusts it;
		// any other build skips the slot. k2 is the release key: every build
		// trusts it, with no define.
		constexpr const char* kK1Hex = "e1650a7bb99a2941c9c034f22f2a22fb16f482b67e7f2edc31489c43975020cf";
		constexpr bool        kK1TestOnly = true;
		constexpr const char* kK2Hex = "5457aba109e92926917d74e15932e4ff689f28f83510ec1a9679e7b8e7c81a85";
		constexpr bool        kK2TestOnly = false;

#if defined(VMAP_TRUST_TEST_KEY)
		constexpr bool kTrustTestKeys = true;
#else
		constexpr bool kTrustTestKeys = false;
#endif

		bool HexKey(const char* hex, std::array<uint8_t, 32>& out)
		{
			if (std::strlen(hex) != 64)
				return false;
			for (size_t i = 0; i < 32; ++i)
			{
				int v[2]{};
				for (int h = 0; h < 2; ++h)
				{
					const char c = hex[i * 2 + h];
					v[h] = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
					if (v[h] < 0)
						return false;
				}
				out[i] = static_cast<uint8_t>(v[0] * 16 + v[1]);
			}
			return true;
		}

		// Standard base64 with padding, canonical: the unused low bits are 0.
		bool Base64Decode(const std::string& s, std::vector<uint8_t>& out)
		{
			out.clear();
			if (s.empty() || s.size() % 4 != 0)
				return false;
			const auto val = [](char c) -> int
			{
				if (c >= 'A' && c <= 'Z') return c - 'A';
				if (c >= 'a' && c <= 'z') return c - 'a' + 26;
				if (c >= '0' && c <= '9') return c - '0' + 52;
				if (c == '+') return 62;
				if (c == '/') return 63;
				return -1;
			};
			for (size_t i = 0; i < s.size(); i += 4)
			{
				const bool last = i + 4 == s.size();
				const int pad = !last ? 0 : (s[i + 3] == '=' ? (s[i + 2] == '=' ? 2 : 1) : 0);
				int v[4]{};
				for (int k = 0; k < 4 - pad; ++k)
				{
					v[k] = val(s[i + k]);
					if (v[k] < 0)
						return false;
				}
				if (pad == 2 && (v[1] & 0x0F) != 0)
					return false;
				if (pad == 1 && (v[2] & 0x03) != 0)
					return false;
				const uint32_t n = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12)
					| (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
				out.push_back(static_cast<uint8_t>(n >> 16));
				if (pad < 2)
					out.push_back(static_cast<uint8_t>(n >> 8));
				if (pad < 1)
					out.push_back(static_cast<uint8_t>(n));
			}
			return true;
		}

		bool IsGlobalKind(int kind)
		{
			return kind == static_cast<int>(ScanValidation::Kind::FunctionStart)
				|| kind == static_cast<int>(ScanValidation::Kind::Data);
		}

		bool SameRow(const MapFunctionRow& a, const MapFunctionRow& b)
		{
			return a.kind == b.kind && a.imageStamp == b.imageStamp && a.imageSize == b.imageSize
				&& a.absent == b.absent && a.pattern == b.pattern && a.resultOffset == b.resultOffset
				&& a.followRel32At == b.followRel32At && a.followRipDispAt == b.followRipDispAt
				&& a.rva == b.rva && a.owners == b.owners;
		}

		bool SameRow(const MapFieldRow& a, const MapFieldRow& b)
		{
			return a.offset == b.offset && a.size == b.size && a.boolMask == b.boolMask && a.member == b.member;
		}

		const MapBuild* BuildOf(const MapData& map, uint32_t cl)
		{
			for (const MapBuild& b : map.builds)
				if (b.gameCL == cl)
					return &b;
			return nullptr;
		}

		bool OnRunning(const MapBuild& b, const Exe& running)
		{
			return b.gameCL == running.cl && b.imageStamp == running.imageStamp && b.imageSize == running.imageSize;
		}

		bool OnRunning(const MapFunctionRow& r, const Exe& running)
		{
			return r.gameCL == running.cl && r.imageStamp == running.imageStamp && r.imageSize == running.imageSize;
		}

		std::wstring Widen(const std::string& s)
		{
			return std::wstring(s.begin(), s.end());   // the texts logged here are ASCII
		}
	}

	const std::vector<PublicKey>& CompiledKeys()
	{
		static const std::vector<PublicKey> keys = []
		{
			std::vector<PublicKey> k;
			const auto slot = [&](const char* id, const char* hex, bool testOnly)
			{
				PublicKey p;
				if ((kTrustTestKeys || !testOnly) && HexKey(hex, p.key))
				{
					p.id = id;
					p.testOnly = testOnly;
					k.push_back(p);
				}
			};
			slot("k1", kK1Hex, kK1TestOnly);
			slot("k2", kK2Hex, kK2TestOnly);
			return k;
		}();
		return keys;
	}

	bool ReadSigned(const std::string& file, const std::vector<PublicKey>& keys, SignedFeed& out, std::string& error)
	{
		out = SignedFeed{};
		const auto fail = [&](const std::string& why)
		{
			error = why;
			return false;
		};
		if (file.empty())
			return fail("the file is empty");
		if (file.size() > kMaxFileBytes)
			return fail("the file is over 1 MiB");
		if (file.back() != '\n')
			return fail("the file does not end in a line feed (truncated?)");

		// The sig line is the last line.
		const size_t prevLf = file.size() >= 2 ? file.rfind('\n', file.size() - 2) : std::string::npos;
		const size_t sigStart = prevLf == std::string::npos ? 0 : prevLf + 1;
		const std::string sigLine = file.substr(sigStart, file.size() - 1 - sigStart);
		std::vector<std::string> f;
		for (size_t a = 0;;)
		{
			const size_t tab = sigLine.find('\t', a);
			f.push_back(sigLine.substr(a, tab == std::string::npos ? std::string::npos : tab - a));
			if (tab == std::string::npos)
				break;
			a = tab + 1;
		}
		if (f.size() != 3 || f[0] != "sig")
			return fail("the last line is not a sig line");
		const std::string region = file.substr(0, sigStart);
		if (region.empty())
			return fail("nothing before the sig line");

		const PublicKey* key = nullptr;
		for (const PublicKey& k : keys)
			if (k.id == f[1])
				key = &k;
		if (!key)
			return fail("the file is signed with key '" + f[1] + "', which this loader does not hold");
		std::vector<uint8_t> sig;
		if (!Base64Decode(f[2], sig) || sig.size() != 64)
			return fail("the signature is not 64 bytes of base64");
		if (crypto_ed25519_check(sig.data(), key->key.data(),
			reinterpret_cast<const uint8_t*>(region.data()), region.size()) != 0)
			return fail("the signature does not match the file");

		// Verified. Now the text: `format`, then `seq`, then the lines ParseFeed reads.
		const size_t l1 = region.find('\n');
		const size_t l2 = l1 == std::string::npos ? std::string::npos : region.find('\n', l1 + 1);
		if (l2 == std::string::npos)
			return fail("no seq line");
		const std::string seqLine = region.substr(l1 + 1, l2 - l1 - 1);
		if (seqLine.size() < 5 || seqLine.size() > 17 || seqLine.compare(0, 4, "seq\t") != 0)
			return fail("the second line is not a seq line");
		uint64_t seq = 0;
		if (seqLine[4] == '0')
			return fail("a seq with a leading zero or of 0");
		for (size_t i = 4; i < seqLine.size(); ++i)
		{
			if (seqLine[i] < '0' || seqLine[i] > '9')
				return fail("the seq is not a decimal number");
			seq = seq * 10 + static_cast<uint64_t>(seqLine[i] - '0');
		}

		MapData map;
		std::string parseError;
		if (!VersionMap::ParseFeed(region.substr(0, l1 + 1) + region.substr(l2 + 1), map, parseError))
			return fail(parseError);

		out.seq     = seq;
		out.keyId   = key->id;
		out.testKey = key->testOnly;
		out.map     = std::move(map);
		return true;
	}

	MapData Merge(const MapData& baseline, const MapData& feed, const Exe& running, MergeReport& report)
	{
		report = MergeReport{};
		MapData out;

		// Builds, by CL: the baseline's if it holds the running exe, else the
		// feed's (the newer), else the baseline's.
		std::set<uint32_t> cls;
		for (const MapBuild& b : baseline.builds) cls.insert(b.gameCL);
		for (const MapBuild& b : feed.builds) cls.insert(b.gameCL);
		for (const uint32_t cl : cls)
		{
			const MapBuild* b = BuildOf(baseline, cl);
			const MapBuild* f = BuildOf(feed, cl);
			const MapBuild* pick = b;
			if (b && f)
			{
				// A rebuilt exe of a known CL: the feed's is the newer, unless
				// the loader's own is the running one.
				pick = OnRunning(*b, running) ? b : f;
				++report.discarded;
				if (pick == b && (b->imageStamp != f->imageStamp || b->imageSize != f->imageSize))
					++report.clashes;
			}
			else if (!b)
				pick = f;
			if (pick == f)
				++report.fromFeed;
			out.builds.push_back(*pick);
		}

		// Functions, by (key, fp, CL).
		using FKey = std::tuple<std::string, uint32_t, uint32_t>;
		std::map<FKey, std::pair<const MapFunctionRow*, const MapFunctionRow*>> groups;
		for (const MapFunctionRow& r : baseline.functions)
		{
			if (!IsGlobalKind(r.kind) && r.fp == 0)
			{
				++report.droppedNoFp;
				continue;
			}
			groups[{ r.key, r.fp, r.gameCL }].first = &r;
		}
		for (const MapFunctionRow& r : feed.functions)
			groups[{ r.key, r.fp, r.gameCL }].second = &r;
		for (const auto& g : groups)
		{
			const MapFunctionRow* b = g.second.first;
			const MapFunctionRow* f = g.second.second;
			const MapFunctionRow* pick = b ? b : f;
			if (b && f)
			{
				++report.discarded;
				if (b->absent && !f->absent && b->imageStamp == f->imageStamp && b->imageSize == f->imageSize)
				{
					pick = f;
					++report.absentFilled;
				}
				else if (OnRunning(*b, running))
				{
					pick = b;
					if (!SameRow(*b, *f))
						++report.clashes;
				}
				else
					pick = f;
			}
			if (pick == f)
				++report.fromFeed;
			out.functions.push_back(*pick);
		}

		// Fields, by (key, CL): the baseline's when it holds the running exe.
		std::map<std::pair<std::string, uint32_t>, std::pair<const MapFieldRow*, const MapFieldRow*>> fields;
		for (const MapFieldRow& r : baseline.fields)
			fields[{ r.key, r.gameCL }].first = &r;
		for (const MapFieldRow& r : feed.fields)
			fields[{ r.key, r.gameCL }].second = &r;
		for (const auto& g : fields)
		{
			const MapFieldRow* b = g.second.first;
			const MapFieldRow* f = g.second.second;
			const MapFieldRow* pick = b ? b : f;
			if (b && f)
			{
				++report.discarded;
				const MapBuild* build = BuildOf(baseline, b->gameCL);
				if (build && OnRunning(*build, running))
				{
					pick = b;
					if (!SameRow(*b, *f))
						++report.clashes;
				}
				else
					pick = f;
			}
			if (pick == f)
				++report.fromFeed;
			out.fields.push_back(*pick);
		}
		return out;
	}

	Coverage LoaderCoverage()
	{
		Coverage cov;
		for (const ScanPatterns::PreflightEntry& entry : ScanPatterns::PreflightRegistry)
			if (entry.required)
				cov.functions.push_back({ VersionMap::LoaderEntryKey(entry), VersionMap::LoaderEntryFingerprint(entry) });
		for (const char* key : VersionMap::kLoaderFieldKeys)
			cov.fields.push_back(key);
		return cov;
	}

	size_t MissingKeys(const MapData& map, uint32_t cl, const Coverage& cov)
	{
		size_t missing = 0;
		for (const Need& n : cov.functions)
		{
			const bool have = std::any_of(map.functions.begin(), map.functions.end(), [&](const MapFunctionRow& r)
				{ return r.gameCL == cl && r.key == n.key && r.fp == n.fp && !r.absent; });
			if (!have)
				++missing;
		}
		for (const std::string& k : cov.fields)
		{
			const bool have = std::any_of(map.fields.begin(), map.fields.end(), [&](const MapFieldRow& r)
				{ return r.gameCL == cl && r.key == k; });
			if (!have)
				++missing;
		}
		return missing;
	}

	std::vector<std::pair<uint32_t, size_t>> DropUncovered(MapData& merged, const MapData& baseline, const Coverage& cov)
	{
		std::vector<std::pair<uint32_t, size_t>> refused;
		std::vector<uint32_t> feedOnly;
		for (const MapBuild& b : merged.builds)
			if (!BuildOf(baseline, b.gameCL))
				feedOnly.push_back(b.gameCL);
		for (const uint32_t cl : feedOnly)
		{
			const size_t missing = MissingKeys(merged, cl, cov);
			if (missing == 0)
				continue;
			refused.push_back({ cl, missing });
			merged.builds.erase(std::remove_if(merged.builds.begin(), merged.builds.end(),
				[&](const MapBuild& b) { return b.gameCL == cl; }), merged.builds.end());
			merged.functions.erase(std::remove_if(merged.functions.begin(), merged.functions.end(),
				[&](const MapFunctionRow& r) { return r.gameCL == cl; }), merged.functions.end());
			merged.fields.erase(std::remove_if(merged.fields.begin(), merged.fields.end(),
				[&](const MapFieldRow& r) { return r.gameCL == cl; }), merged.fields.end());
		}
		return refused;
	}

	namespace
	{
		// The file at `path`, at most kMaxFileBytes. False: missing, over the
		// cap or unreadable (`missing` tells the first from the rest).
		bool ReadFileCapped(const std::wstring& path, std::string& out, bool& missing)
		{
			out.clear();
			missing = false;
			HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
			{
				missing = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
				return false;
			}
			LARGE_INTEGER size{};
			bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) <= kMaxFileBytes;
			if (ok)
			{
				out.resize(static_cast<size_t>(size.QuadPart));
				DWORD got = 0;
				ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
			}
			CloseHandle(h);
			if (!ok)
				out.clear();
			return ok;
		}
	}

	bool ReadCache(const std::wstring& path, const std::vector<PublicKey>& keys, SignedFeed& out)
	{
		std::string file, error;
		bool missing = false;
		return !keys.empty() && ReadFileCapped(path, file, missing) && ReadSigned(file, keys, out, error);
	}

	std::shared_ptr<const MapData> BuildFromCache(const std::wstring& path, const std::vector<PublicKey>& keys,
		const Exe& running, const Coverage& cov, uint64_t* seqOut)
	{
		if (keys.empty())
			return nullptr;
		std::string file;
		bool missing = false;
		if (!ReadFileCapped(path, file, missing))
		{
			if (!missing)
				ModLoaderLogger::LogWarn(L"[VersionFeed] The feed cache '%ls' could not be read (or is over 1 MiB); using the loader's own map", path.c_str());
			return nullptr;
		}
		if (file.empty())
			return nullptr;   // the mark a request that got no feed leaves
		SignedFeed feed;
		std::string error;
		if (!ReadSigned(file, keys, feed, error))
		{
			ModLoaderLogger::LogWarn(L"[VersionFeed] The feed cache is not usable (%ls); using the loader's own map", Widen(error).c_str());
			return nullptr;
		}
		MergeReport report;
		MapData merged = Merge(VersionMap::BaselineMap(), feed.map, running, report);
		const auto refused = DropUncovered(merged, VersionMap::BaselineMap(), cov);
		ModLoaderLogger::LogInfo(L"[VersionFeed] Feed seq %llu (key %ls%ls): %zu rows from the feed, %zu discarded (%zu clashed with the loader's map on this exe, %zu absent rows filled, %zu scoped rows without fp dropped)",
			static_cast<unsigned long long>(feed.seq), Widen(feed.keyId).c_str(), feed.testKey ? L", TEST KEY" : L"",
			report.fromFeed, report.discarded, report.clashes, report.absentFilled, report.droppedNoFp);
		for (const auto& r : refused)
			ModLoaderLogger::LogWarn(L"[VersionFeed] CL-%u from the feed is refused: it lacks %zu of the keys the loader needs", r.first, r.second);
		if (seqOut)
			*seqOut = feed.seq;
		return std::make_shared<const MapData>(std::move(merged));
	}

	bool ActivateCached(const std::wstring& path)
	{
		static std::atomic<bool> s_done{ false };
		if (s_done.exchange(true))
			return false;
#ifdef VMAP_TRUST_TEST_KEY
		// The release workflows scan their DLLs for this marker and refuse to
		// publish one that holds it (.github/scripts/assert-no-test-key.ps1).
		static const char kTestKeyMarker[] = "VMAP-TEST-KEY-TRUSTED";
		ModLoaderLogger::LogWarn(L"[VersionFeed] This build trusts the test feed key (%hs); it is for CI test rigs only", kTestKeyMarker);
#endif
		const auto map = BuildFromCache(path, CompiledKeys(), VersionMap::RunningExe(), LoaderCoverage());
		if (!map)
			return false;
		VersionMap::SetActiveMap(map);
		return true;
	}

	bool StoreCache(const std::wstring& path, const std::string& file, const std::vector<PublicKey>& keys, std::string& error)
	{
		SignedFeed incoming;
		if (!ReadSigned(file, keys, incoming, error))
			return false;

		uint64_t cachedSeq = 0;
		std::string old;
		bool missing = false;
		SignedFeed cached;
		std::string ignored;
		if (ReadFileCapped(path, old, missing) && ReadSigned(old, keys, cached, ignored))
			cachedSeq = cached.seq;
		if (incoming.seq <= cachedSeq)
		{
			error = "seq " + std::to_string(incoming.seq) + " is not above the cache's " + std::to_string(cachedSeq);
			return false;
		}

		const std::wstring temp = path + L".tmp";
		HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			error = "cannot create the temp file (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		DWORD wrote = 0;
		const bool ok = WriteFile(h, file.data(), static_cast<DWORD>(file.size()), &wrote, nullptr)
			&& wrote == file.size() && FlushFileBuffers(h);
		CloseHandle(h);
		if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			error = "cannot write the cache (error " + std::to_string(GetLastError()) + ")";
			DeleteFileW(temp.c_str());
			return false;
		}
		return true;
	}
}
