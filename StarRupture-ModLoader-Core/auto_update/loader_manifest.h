#pragma once

#include <cstdio>
#include <string>

#include "auto_update/json_extract.h"

// What the loader's own release manifest says about the latest release, and
// the sentence a refused plugin gets from it. Header only, so a test can use
// it without the updater.
namespace LoaderManifest
{
	struct Latest
	{
		std::string tag;            // build_tag, e.g. "v1.28.0"
		int         ifaceMin = -1;  // interface_version_min, -1 when the manifest has none
		int         ifaceMax = -1;  // interface_version_max, -1 when the manifest has none
		bool        known    = false;   // a build_tag was read

		bool HasRange() const { return known && ifaceMax >= 0; }
	};

	// A number too large for an int reads as absent.
	inline int ReadInterface(const std::string& json, const char* key)
	{
		try { return JsonExtractInt(json, key, -1); }
		catch (...) { return -1; }
	}

	// Manifests made before the interface range was added carry only build_tag.
	inline Latest Parse(const std::string& manifest)
	{
		Latest latest;
		latest.tag      = JsonExtractString(manifest, "build_tag");
		latest.known    = !latest.tag.empty();
		latest.ifaceMin = ReadInterface(manifest, "interface_version_min");
		latest.ifaceMax = ReadInterface(manifest, "interface_version_max");
		return latest;
	}

	// What to say about a plugin built for plugin interface `declared`, which
	// this loader refused as too new. A phrase to follow "This plugin", cut to
	// cap like snprintf.
	inline const char* Describe(int declared, const Latest& latest, char* buf, size_t cap)
	{
		if (latest.HasRange() && latest.ifaceMax >= declared)
			snprintf(buf, cap, "needs ModLoader %s or newer", latest.tag.c_str());
		else if (latest.HasRange())
			snprintf(buf, cap, "is built for a ModLoader newer than the latest release (%s)", latest.tag.c_str());
		else
			snprintf(buf, cap, "needs a newer ModLoader");
		return buf;
	}
}
