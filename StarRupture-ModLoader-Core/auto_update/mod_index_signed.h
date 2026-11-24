#pragma once

#include "auto_update/mod_index_fetcher.h"
#include "memory_scanner/version_feed.h"

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// mod_index_signed.h -- the mod index as a signed file (mod-index.v1.txt), and
// the download that reads it for the Browse tab (mod_index_fetcher.h).
//
// The file is signed like the version map feed (version_feed.h: same keys, own
// `seq`), around the index JSON of mod_index.h:
//
//   format<TAB>1
//   seq<TAB><n>                    rises with every publish
//   kind<TAB>mod-index
//   <the index JSON, byte for byte>
//   sig<TAB><keyId><TAB><base64>   over every byte before this line
//
// Entries are only ever read from a body whose signature has passed.
//
// The loader's own address (the file beside the version map feed) accepts only
// this envelope. It keeps the last verified file in a cache (with its ETag, so
// an unchanged file is a 304), refuses a file whose `seq` is below the cache's,
// and shows the cached list when the download itself fails. An address the
// player set in modloader.ini [AutoUpdate] ModIndexUrl may serve the envelope
// or a bare JSON index (for trying an index out): it is read as it comes, an
// envelope still has to verify, and nothing is cached.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	struct Envelope
	{
		uint64_t    seq = 0;
		std::string body;   // the index JSON
	};

	// Whether `file` starts like an envelope rather than a bare JSON index.
	bool IsEnvelope(const std::string& file);

	// Verifies `file` against `keys`, then checks the format and kind lines and
	// takes the body. On failure `out` is left empty and `error` says why in plain
	// words for the player (the technical reason is logged).
	bool ReadEnvelope(const std::string& file, const std::vector<VersionFeed::PublicKey>& keys, Envelope& out,
		std::string& error);

	struct Source
	{
		std::vector<VersionFeed::PublicKey> keys;    // the compiled keys; the tests pass their own
		std::wstring cachePath;                      // the verified file and, beside it, its ETag
		bool signedOnly = true;                      // the loader's own address; false: the player's ModIndexUrl
		bool allowPlainHttp = false;                 // for tests that run a local server
	};

	// The request for the Fetcher: one GET of the URL, verified, and (signedOnly)
	// cached. Runs on the Fetcher's worker thread.
	FetchFn MakeFetch(Source source);
}
