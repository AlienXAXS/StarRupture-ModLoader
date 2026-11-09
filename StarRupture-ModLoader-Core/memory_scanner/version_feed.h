#pragma once

#include "memory_scanner/version_map.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// VersionFeed -- the signed map data a loader reads between releases.
//
// The feed is version-map-client.v1.txt (see VersionMap::ParseFeed for the
// lines) plus two lines of its own:
//
//   format<TAB>1
//   seq<TAB><n>                    the UTC second of signing, never 0
//   build ... function ... field ...
//   sig<TAB><keyId><TAB><base64>   the last line
//
// `sig` is an Ed25519 signature (RFC 8032) by the key in slot <keyId> over
// every byte before the `sig` line, `seq` line included. Verify first, then
// parse: nothing in a file is read until its signature has passed. A file over
// 1 MiB, with a `sig` line that is not the last, with an unknown key ID or
// with any malformed line is rejected whole (a line of a kind the loader
// does not know is not malformed: it is skipped).
//
// What a loader does with a verified feed:
//   1. Merge it with the map compiled into the loader (Merge), one row per key
//      and CL, and for a scoped key one per (key, fp, CL). The feed can add
//      builds and keys and fill a shipped `absent`; it never changes a
//      locator the loader shipped with for the running exe.
//   2. Drop every CL only the feed knows that lacks a locator for a required
//      loader entry or a loader field (DropUncovered).
//   3. Make the result the map every lookup reads (VersionMap::SetActiveMap),
//      once per process (ActivateCached).
// A feed's cache file is the signed file itself, verified again on every
// launch; the cache only ever moves to a higher `seq` (StoreCache).
// ---------------------------------------------------------------------------

namespace VersionFeed
{
	constexpr size_t kMaxFileBytes = 1u << 20;

	struct PublicKey
	{
		std::string           id;         // "k1", "k2"
		std::array<uint8_t, 32> key{};
		bool                  testOnly = false;
	};

	// The key slots compiled into this loader, the ones that hold a key. With
	// none, the loader reads no feed and fetches none.
	const std::vector<PublicKey>& CompiledKeys();

	struct SignedFeed
	{
		uint64_t              seq = 0;
		std::string           keyId;
		bool                  testKey = false;
		VersionMap::MapData   map;
	};

	// Verifies `file` against `keys`, then parses it. On failure `out` is left
	// empty and `error` says why.
	bool ReadSigned(const std::string& file, const std::vector<PublicKey>& keys, SignedFeed& out, std::string& error);

	// What a merge did, for its one log line.
	struct MergeReport
	{
		size_t fromFeed        = 0;   // rows (builds included) the merge took from the feed
		size_t discarded       = 0;   // rows from either side the merge did not keep
		size_t clashes         = 0;   // of those, feed rows that disagreed with the baseline on the running exe
		size_t absentFilled    = 0;   // baseline `absent` rows replaced by a feed locator
		size_t droppedNoFp     = 0;   // baseline scoped rows with fp 0, which match no request
	};

	// The baseline and the feed as one map. See the file comment.
	VersionMap::MapData Merge(const VersionMap::MapData& baseline, const VersionMap::MapData& feed,
		const VersionMap::Exe& running, MergeReport& report);

	// What a CL needs before the loader runs on it: a locator (not `absent`)
	// for each function entry, under that fp, and a row for each field key.
	struct Need
	{
		std::string key;
		uint32_t    fp = 0;   // 0 for a global key; the running loader's own fp for a ModLoader/ entry
	};
	struct Coverage
	{
		std::vector<Need>        functions;
		std::vector<std::string> fields;
	};

	// The loader's own: its required registry entries (LoaderEntryKey and
	// LoaderEntryFingerprint) and kLoaderFieldKeys.
	Coverage LoaderCoverage();

	// How many of `cov` the map lacks on `cl`.
	size_t MissingKeys(const VersionMap::MapData& map, uint32_t cl, const Coverage& cov);

	// Removes from `merged` every build, with its rows, that `baseline` does
	// not hold and that lacks something in `cov`. Returns each CL removed with
	// the number of keys it lacked.
	std::vector<std::pair<uint32_t, size_t>> DropUncovered(VersionMap::MapData& merged,
		const VersionMap::MapData& baseline, const Coverage& cov);

	// The verified feed in the cache file at `path`; false, quietly, when
	// there is none or it does not verify.
	bool ReadCache(const std::wstring& path, const std::vector<PublicKey>& keys, SignedFeed& out);

	// Reads the cache file at `path` and builds the map to run on: the
	// baseline merged with the verified feed. Returns null, and leaves the
	// baseline in force, when there is no cache or it fails (logged). Logs one
	// line for the merge and one for each CL refused.
	std::shared_ptr<const VersionMap::MapData> BuildFromCache(const std::wstring& path,
		const std::vector<PublicKey>& keys, const VersionMap::Exe& running, const Coverage& cov, uint64_t* seq = nullptr);

	// Stage 1's read: BuildFromCache with the compiled keys and the loader's
	// coverage, then VersionMap::SetActiveMap. Once per process; a second
	// call does nothing. Call it before anything reads the map. Returns
	// whether a feed is now in force.
	bool ActivateCached(const std::wstring& path);

	// Verifies `file` and, if its `seq` is higher than the cache's (a cache
	// that is missing or fails verification counts as seq 0), replaces the
	// cache file with it through a temp file and MoveFileExW. A file that
	// fails, or has an equal or lower `seq`, leaves the cache as it was.
	bool StoreCache(const std::wstring& path, const std::string& file,
		const std::vector<PublicKey>& keys, std::string& error);
}
