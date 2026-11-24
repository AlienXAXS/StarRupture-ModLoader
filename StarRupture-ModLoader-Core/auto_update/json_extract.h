#pragma once

#include <cctype>
#include <string>
#include <vector>

// The loader's minimal JSON readers: no external library, only the flat
// manifest schema. Shared by the update passes (auto_updater.cpp),
// loader_manifest.h and the mod index (mod_index.h).

// Extract the value of a top-level string field: "key": "value"
// Handles basic \" escapes inside the value.
inline std::string JsonExtractString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return {};

	size_t vstart = json.find('"', kpos + needle.size());
	if (vstart == std::string::npos)
		return {};
	++vstart;

	std::string value;
	for (size_t i = vstart; i < json.size(); ++i)
	{
		if (json[i] == '\\' && i + 1 < json.size())
		{
			value += json[i + 1];
			++i;
		}
		else if (json[i] == '"')
		{
			break;
		}
		else
		{
			value += json[i];
		}
	}
	return value;
}

// Extract the value of a top-level integer field: "key": 12
inline int JsonExtractInt(const std::string& json, const char* key, int fallback = 0)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return fallback;

	size_t vpos = kpos + needle.size();
	while (vpos < json.size() && (json[vpos] == ' ' || json[vpos] == '\t'
		|| json[vpos] == '\r' || json[vpos] == '\n'
		|| json[vpos] == ':'))
		++vpos;

	if (vpos >= json.size() || !isdigit(static_cast<unsigned char>(json[vpos])))
		return fallback;

	return std::stoi(json.substr(vpos));
}

// Extract a top-level string field and decode its JSON escapes. Used for the
// manifest's "notes": JsonExtractString (json_extract.h) drops the backslash, so it
// cannot tell \n from the letter n. Handles \n \t \" \\ \/ and \uXXXX; \r is
// dropped, and a \u escape outside printable ASCII becomes '?' so the text
// always draws in the overlay font.
inline std::string JsonExtractDecodedString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return {};

	size_t vstart = json.find('"', kpos + needle.size());
	if (vstart == std::string::npos)
		return {};
	++vstart;

	std::string value;
	for (size_t i = vstart; i < json.size(); ++i)
	{
		if (json[i] == '"')
			break;

		if (json[i] != '\\' || i + 1 >= json.size())
		{
			value += json[i];
			continue;
		}

		switch (json[++i])
		{
		case 'n': value += '\n'; break;
		case 't': value += '\t'; break;
		case 'r': break;
		case 'u':
		{
			bool ok = i + 4 < json.size();
			for (size_t k = 1; ok && k <= 4; ++k)
				ok = isxdigit(static_cast<unsigned char>(json[i + k])) != 0;
			const unsigned code = ok ? std::stoul(json.substr(i + 1, 4), nullptr, 16) : 0;
			if (ok)
				i += 4;
			value += (ok && (code == '\n' || (code >= 0x20 && code < 0x7F))) ? static_cast<char>(code) : '?';
			break;
		}
		default: value += json[i]; break;
		}
	}
	return value;
}

// Split the array whose '[' is at json[arrayStart] into its { } object strings,
// by brace depth, skipping braces and quotes inside strings. Elements that are
// not objects are skipped. Stops after maxObjects objects (so a caller can cap
// the work and the memory); an unterminated object is dropped.
inline std::vector<std::string> JsonSplitObjectArray(
	const std::string& json, size_t arrayStart, size_t maxObjects = static_cast<size_t>(-1))
{
	std::vector<std::string> results;

	size_t i = arrayStart + 1;
	while (i < json.size() && results.size() < maxObjects)
	{
		if (json[i] == ' ' || json[i] == '\t' ||
			json[i] == '\r' || json[i] == '\n' || json[i] == ',')
		{
			++i;
			continue;
		}

		if (json[i] == ']')
			break;

		if (json[i] == '{')
		{
			size_t start = i;
			int depth = 0;
			bool inString = false;
			while (i < json.size())
			{
				char c = json[i];
				if (inString)
				{
					if (c == '\\') { ++i; }
					else if (c == '"') { inString = false; }
				}
				else
				{
					if (c == '"') { inString = true; }
					else if (c == '{') { ++depth; }
					else if (c == '}')
					{
						--depth;
						if (depth == 0)
						{
							results.push_back(json.substr(start, i - start + 1));
							++i;
							break;
						}
					}
				}
				++i;
			}
			continue;
		}

		++i;
	}

	return results;
}

// Split the "plugins": [ ... ] array into individual { } object strings.
// Works via brace-depth tracking; assumes no nested objects inside entries.
inline std::vector<std::string> JsonExtractObjectArray(
	const std::string& json, const char* arrayKey)
{
	std::string needle = std::string("\"") + arrayKey + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return {};

	size_t apos = json.find('[', kpos + needle.size());
	if (apos == std::string::npos)
		return {};

	return JsonSplitObjectArray(json, apos);
}
