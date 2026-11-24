#pragma once

#include "memory_scanner/version_feed.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// plugin_catalog.h -- the signed plugin catalog (plugin-catalog.v1.txt): its
// cache file and the copy the loader keeps in memory.
//
// The file is signed like the version map (see version_feed.h; same keys,
// own `seq`) and its body follows plugin_catalog_text.h. A fetched file is
// verified, parsed, and replaces the cache only with a higher `seq`; a lower
// one (rollback) or the same `seq` with other bytes (conflict) is refused, and
// any failure leaves the last good file as it was. The cache file is the
// signed file itself, verified again when it is loaded.
//
// The copy in memory is read once at start (LoadCache) and swapped when a
// fetch stores a newer file, so AccessorText never touches the disk.
// ---------------------------------------------------------------------------

namespace PluginCatalog
{
	enum class Outcome
	{
		Stored,            // verified, newer, and now the cache
		Unchanged,         // the cache already holds exactly this file's body
		RefusedRollback,   // lower seq than the cache's
		RefusedConflict,   // the cache's seq with a different body
		Rejected,          // failed the signature or the grammar, or could not be written
	};

	// Verifies, parses and decides `file` against the cache at `path`; on
	// Stored, writes the cache and replaces the copy in memory. `error` says
	// why for Rejected, and names the seqs for a refusal.
	Outcome Store(const std::wstring& path, const std::string& file,
		const std::vector<VersionFeed::PublicKey>& keys, std::string& error);

	// Reads the cache at `path` into memory. A missing cache is quiet; one that
	// fails to verify or parse is logged and leaves the copy empty.
	void LoadCache(const std::wstring& path, const std::vector<VersionFeed::PublicKey>& keys);

	// The catalog in plugin_catalog_text.h's grammar, or null when the loader
	// holds none. The text is never changed once published, so the caller may
	// keep the pointer. Takes a lock for the length of a pointer copy; no I/O.
	std::shared_ptr<const std::string> AccessorText();

	// IPluginRegistry::GetFeatureCatalog: AccessorText copied into buf the way
	// snprintf does (the full length, NUL-terminated and cut to size; a null buf
	// and a size of 0 only measure), or -1 when the loader holds no catalog.
	int CopyAccessorText(char* buf, size_t size);
}
