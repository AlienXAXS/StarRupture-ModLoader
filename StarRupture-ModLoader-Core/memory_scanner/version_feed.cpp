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
		// feeds. Only a build that defines VMAP_TRUST_TEST_KEY (the tests and
		// sr-aobscan) trusts it; any other build skips the slot. k2 is the release
		// key: every build trusts it, with no define.
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

		// A server row holds on one exe: CL, time stamp and image size. Cuts both
		// sides down, before the merge, to one exe's rows per build. A file line
		// that is the table's exe adds rows only for names the table has no row
		// for, of any kind or fingerprint, an absent one included. A line for
		// another exe of that CL shares nothing with the table: the side whose
		// line the merge keeps supplies the whole build.
		void BindToExe(MapData& baseline, MapData& feed, const Exe& running, MergeReport& report)
		{
			const auto eraseRows = [](auto& rows, const auto& unwanted)
			{
				rows.erase(std::remove_if(rows.begin(), rows.end(), unwanted), rows.end());
			};
			for (const MapBuild& own : baseline.builds)
			{
				const MapBuild* theirs = BuildOf(feed, own.gameCL);
				if (!theirs)
					continue;
				const uint32_t cl = own.gameCL;
				const bool sameExe = own.imageStamp == theirs->imageStamp && own.imageSize == theirs->imageSize;
				const bool onRunning = OnRunning(own, running);

				std::set<std::string> names;
				for (const MapFunctionRow& r : baseline.functions)
					if (r.gameCL == cl)
						names.insert(r.key);
				for (const MapFieldRow& r : baseline.fields)
					if (r.gameCL == cl)
						names.insert(r.key);
				for (const VersionMap::MapAbsentField& a : baseline.absentFields)
					if (a.gameCL == cl)
						names.insert(a.key);

				if (!sameExe && !onRunning)
				{
					const auto ofThisBuild = [&](const auto& r) { return r.gameCL == cl; };
					const size_t before = baseline.functions.size() + baseline.fields.size();
					eraseRows(baseline.functions, ofThisBuild);
					eraseRows(baseline.fields, ofThisBuild);
					eraseRows(baseline.absentFields, ofThisBuild);
					report.discarded += before - baseline.functions.size() - baseline.fields.size();
					continue;
				}

				// A file row that goes. It clashes when the table's exe is running
				// and the table says otherwise: on that exe, about the name; from
				// another exe's line, about the same row.
				const auto drop = [&](const auto& r, const auto& rows, const auto& samePlace)
				{
					if (r.gameCL != cl || (sameExe && !names.count(r.key)))
						return false;
					++report.discarded;
					bool paired = false, twin = false;
					for (const auto& o : rows)
						if (o.gameCL == cl && o.key == r.key && samePlace(o, r))
						{
							paired = true;
							twin   = twin || SameRow(o, r);
						}
					if (onRunning && !twin && (sameExe || paired))
						++report.clashes;
					return true;
				};
				eraseRows(feed.functions, [&](const MapFunctionRow& r)
				{
					return drop(r, baseline.functions, [](const MapFunctionRow& a, const MapFunctionRow& b) { return a.fp == b.fp; });
				});
				eraseRows(feed.fields, [&](const MapFieldRow& r)
				{
					return drop(r, baseline.fields, [](const MapFieldRow&, const MapFieldRow&) { return true; });
				});

				if (sameExe && onRunning)
				{
					for (const MapFunctionRow& r : feed.functions)
						if (r.gameCL == cl)
							report.added.push_back(r.key);
					for (const MapFieldRow& r : feed.fields)
						if (r.gameCL == cl)
							report.added.push_back(r.key);
				}
			}
		}

		// One line per build the feed names, after the summary: what the file
		// holds for it and what the merge did with it. Reads the feed and the
		// baseline again; it changes nothing.
		void LogBuilds(const MapData& baseline, const MapData& feed, const MapData& merged, const Exe& running,
			const MergeReport& report, Flavor flavor)
		{
			constexpr size_t kMaxLines = 8;
			using FKey = std::tuple<std::string, uint32_t, uint32_t>;
			std::map<FKey, const MapFunctionRow*> baseFns;
			for (const MapFunctionRow& r : baseline.functions)
				baseFns[{ r.key, r.fp, r.gameCL }] = &r;
			std::map<std::pair<std::string, uint32_t>, const MapFieldRow*> baseFields;
			for (const MapFieldRow& r : baseline.fields)
				baseFields[{ r.key, r.gameCL }] = &r;

			size_t shown = 0;
			for (const MapBuild& fb : feed.builds)
			{
				if (shown == kMaxLines)
				{
					ModLoaderLogger::LogInfo(L"[VersionFeed]   and %zu more build(s) in the file", feed.builds.size() - kMaxLines);
					break;
				}
				++shown;

				const uint32_t cl = fb.gameCL;
				size_t rows = 0, same = 0, differ = 0;
				for (const MapFunctionRow& r : feed.functions)
				{
					if (r.gameCL != cl)
						continue;
					++rows;
					const auto it = baseFns.find({ r.key, r.fp, cl });
					if (it != baseFns.end())
						(SameRow(*it->second, r) ? same : differ)++;
				}
				for (const MapFieldRow& r : feed.fields)
				{
					if (r.gameCL != cl)
						continue;
					++rows;
					const auto it = baseFields.find({ r.key, cl });
					if (it != baseFields.end())
						(SameRow(*it->second, r) ? same : differ)++;
				}
				const size_t added = rows - same - differ;

				wchar_t label[96];
				if (OnRunning(fb, running))
					swprintf_s(label, L"CL %u (this exe)", cl);
				else
					swprintf_s(label, L"CL %u (other exe, stamp 0x%08X, size 0x%08X)", cl, fb.imageStamp, fb.imageSize);

				const MapBuild* own = BuildOf(baseline, cl);
				if (flavor == Flavor::Server && own && BuildOf(merged, cl))
				{
					if (!OnRunning(fb, running))
						ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, not used on this exe", label, rows);
					else if (OnRunning(*own, running))
						ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, %zu added to the loader's own, the rest not used",
							label, rows, report.added.size());
					else
						ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, used in place of the loader's own", label, rows);
				}
				else if (!BuildOf(merged, cl))
					ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, refused (see below), not in the version map", label, rows);
				else if (!own)
					ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, a build the loader does not have, added to the version map", label, rows);
				else if (differ == 0 && added == 0)
					ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, identical to the loader's own, none used", label, rows);
				else
					ModLoaderLogger::LogInfo(L"[VersionFeed]   %ls: %zu rows, %zu differ from the loader's own (%ls rows kept), %zu not in the loader's own and added",
						label, rows, differ, OnRunning(*own, running) ? L"the loader's" : L"the file's", added);
			}
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

	bool VerifySigned(const std::string& file, const std::vector<PublicKey>& keys, VerifiedBody& out, std::string& error)
	{
		out = VerifiedBody{};
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

		// Verified. Now the second line: `seq`.
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

		out.seq     = seq;
		out.keyId   = key->id;
		out.testKey = key->testOnly;
		out.body    = region;
		return true;
	}

	bool ReadSigned(const std::string& file, const std::vector<PublicKey>& keys, SignedFeed& out, std::string& error,
		Flavor flavor)
	{
		out = SignedFeed{};
		VerifiedBody verified;
		if (!VerifySigned(file, keys, verified, error))
			return false;
		const auto fail = [&](const std::string& why)
		{
			error = why;
			return false;
		};

		// The text ParseFeed reads: `format`, then everything after the `seq` line.
		const std::string& region = verified.body;
		const size_t l1 = region.find('\n');
		const size_t l2 = region.find('\n', l1 + 1);
		// Another signed kind is not a version map: ParseFeed would skip its rows and
		// read an empty map, which the cache would then keep ahead of the real one. A
		// version map has no kind line, so any kind line is refused, named or not.
		if (region.compare(l2 + 1, 20, "kind\tplugin-catalog\n") == 0)
			return fail("this is a plugin catalog, not a version map");
		if (region.compare(l2 + 1, 15, "kind\tmod-index\n") == 0)
			return fail("this is a mod index, not a version map");
		if (region.compare(l2 + 1, 5, "kind\t") == 0)
			return fail("this is another kind of file, not a version map");
		MapData map;
		std::string parseError;
		if (!VersionMap::ParseFeed(region.substr(0, l1 + 1) + region.substr(l2 + 1), map, parseError))
			return fail(parseError);

		// The right file for this loader: a server file names only server exes.
		size_t serverBuilds = 0;
		for (const MapBuild& b : map.builds)
			serverBuilds += b.source.compare(0, std::strlen(kServerSourcePrefix), kServerSourcePrefix) == 0;
		if (flavor == Flavor::Server && map.builds.empty())
			return fail("this is not a dedicated server file (it has no build line)");
		if (flavor == Flavor::Server && serverBuilds != map.builds.size())
			return fail("this is not a dedicated server file (a build line is not labelled \"server ...\")");
		if (flavor == Flavor::Client && serverBuilds != 0)
			return fail("this is a dedicated server file, which a client loader does not read");

		out.seq     = verified.seq;
		out.keyId   = verified.keyId;
		out.testKey = verified.testKey;
		out.map     = std::move(map);
		return true;
	}

	MapData Merge(const MapData& builtIn, const MapData& downloaded, const Exe& running, MergeReport& report, Flavor flavor)
	{
		report = MergeReport{};
		MapData baseline = builtIn;
		MapData feed     = downloaded;
		if (flavor == Flavor::Server)
			BindToExe(baseline, feed, running, report);
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

		// Absent fields are not in a feed: the loader's compiled list stands.
		out.absentFields = baseline.absentFields;
		return out;
	}

	Coverage LoaderCoverage(Flavor flavor)
	{
		Coverage cov;
		if (flavor == Flavor::Server)
			return cov;
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

	bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::string& error)
	{
		const std::wstring temp = path + L".tmp";
		HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			error = "cannot create the temp file (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		DWORD wrote = 0;
		const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr)
			&& wrote == data.size() && FlushFileBuffers(h);
		CloseHandle(h);
		if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			error = "cannot write the cache (error " + std::to_string(GetLastError()) + ")";
			DeleteFileW(temp.c_str());
			return false;
		}
		return true;
	}

	bool ReadCache(const std::wstring& path, const std::vector<PublicKey>& keys, SignedFeed& out, Flavor flavor)
	{
		std::string file, error;
		bool missing = false;
		return !keys.empty() && ReadFileCapped(path, file, missing) && ReadSigned(file, keys, out, error, flavor);
	}

	std::shared_ptr<const MapData> BuildFromCache(const std::wstring& path, const std::vector<PublicKey>& keys,
		const Exe& running, const Coverage& cov, uint64_t* seqOut, Flavor flavor)
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
			return nullptr;   // an empty file, as an older loader left after a request that got no feed
		SignedFeed feed;
		std::string error;
		if (!ReadSigned(file, keys, feed, error, flavor))
		{
			ModLoaderLogger::LogWarn(L"[VersionFeed] The feed cache is not usable (%ls); using the loader's own map", Widen(error).c_str());
			return nullptr;
		}
		MergeReport report;
		MapData merged = Merge(VersionMap::BaselineMap(), feed.map, running, report, flavor);
		const auto refused = DropUncovered(merged, VersionMap::BaselineMap(), cov);
		ModLoaderLogger::LogInfo(L"[VersionFeed] Feed seq %llu (key %ls%ls): %zu rows from the feed, %zu discarded (%zu clashed with the loader's map on this exe, %zu absent rows filled, %zu scoped rows without fp dropped)",
			static_cast<unsigned long long>(feed.seq), Widen(feed.keyId).c_str(), feed.testKey ? L", TEST KEY" : L"",
			report.fromFeed, report.discarded, report.clashes, report.absentFilled, report.droppedNoFp);
		LogBuilds(VersionMap::BaselineMap(), feed.map, merged, running, report, flavor);
		for (const std::string& name : report.added)
			ModLoaderLogger::LogInfo(L"[VersionFeed]   added from the file: %ls", Widen(name).c_str());
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
		const auto map = BuildFromCache(path, CompiledKeys(), VersionMap::RunningExe(), LoaderCoverage());
		if (!map)
			return false;
		VersionMap::SetActiveMap(map);
		return true;
	}

	bool StoreCache(const std::wstring& path, const std::string& file, const std::vector<PublicKey>& keys, std::string& error,
		Flavor flavor)
	{
		SignedFeed incoming;
		if (!ReadSigned(file, keys, incoming, error, flavor))
			return false;

		uint64_t cachedSeq = 0;
		std::string old;
		bool missing = false;
		SignedFeed cached;
		std::string ignored;
		if (ReadFileCapped(path, old, missing) && ReadSigned(old, keys, cached, ignored, flavor))
			cachedSeq = cached.seq;
		if (incoming.seq <= cachedSeq)
		{
			error = "seq " + std::to_string(incoming.seq) + " is not above the cache's " + std::to_string(cachedSeq);
			return false;
		}

		return WriteFileAtomic(path, file, error);
	}
}
