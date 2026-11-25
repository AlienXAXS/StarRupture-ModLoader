#pragma once

#include <cctype>
#include <string>
#include <vector>

// The loader's minimal JSON readers: no external library, only the flat
// manifest schema. Shared by the update passes (auto_updater.cpp),
// loader_manifest.h and the mod index (mod_index.h).

// Find the value of "key": the first string token equal to key that sits
// outside any other string and is followed by a colon. Returns the position of
// the value's first character, or npos. A string value that merely spells the
// key (such as "name": "id") is not a key, and escaped quotes are skipped over.
inline size_t JsonFindValue(const std::string& json, const char* key)
{
	const size_t keyLen = std::char_traits<char>::length(key);
	size_t i = 0;
	while (i < json.size())
	{
		if (json[i] != '"')
		{
			++i;
			continue;
		}

		const size_t start = ++i;
		while (i < json.size() && json[i] != '"')
			i += (json[i] == '\\') ? 2 : 1;
		if (i >= json.size())
			return std::string::npos;
		const size_t end = i++;

		size_t next = i;
		while (next < json.size() && isspace(static_cast<unsigned char>(json[next])))
			++next;
		if (next >= json.size() || json[next] != ':')
			continue;

		if (end - start == keyLen && json.compare(start, keyLen, key) == 0)
		{
			++next;
			while (next < json.size() && isspace(static_cast<unsigned char>(json[next])))
				++next;
			return next < json.size() ? next : std::string::npos;
		}
	}
	return std::string::npos;
}

// Extract the value of a top-level string field: "key": "value"
// Handles basic \" escapes inside the value. Empty when the key is missing or
// its value is not a string.
inline std::string JsonExtractString(const std::string& json, const char* key)
{
	size_t vstart = JsonFindValue(json, key);
	if (vstart == std::string::npos || json[vstart] != '"')
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
	const size_t vpos = JsonFindValue(json, key);
	if (vpos == std::string::npos || !isdigit(static_cast<unsigned char>(json[vpos])))
		return fallback;

	return std::stoi(json.substr(vpos));
}

// Decode the string whose first character (after the opening quote) is json[i].
// Leaves i on the closing quote, or at json.size() when there is none.
inline std::string JsonDecodeStringAt(const std::string& json, size_t& i)
{
	std::string value;
	for (; i < json.size(); ++i)
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

// Extract a top-level string field and decode its JSON escapes. Used for the
// manifest's "notes": JsonExtractString (json_extract.h) drops the backslash, so it
// cannot tell \n from the letter n. Handles \n \t \" \\ \/ and \uXXXX; \r is
// dropped, and a \u escape outside printable ASCII becomes '?' so the text
// always draws in the overlay font.
inline std::string JsonExtractDecodedString(const std::string& json, const char* key)
{
	size_t i = JsonFindValue(json, key);
	if (i == std::string::npos || json[i] != '"')
		return {};
	return JsonDecodeStringAt(json, ++i);
}

// The value of "key": ["a", "b"], decoded. Empty when the key is missing, the
// value is not an array, it is cut off, or any item is not a string. Items
// must be strings: JsonFindValue finds a key at any depth, so the keys of an
// object inside the array would be read as the enclosing object's.
inline std::vector<std::string> JsonExtractStringArray(const std::string& json, const char* key)
{
	size_t i = JsonFindValue(json, key);
	if (i == std::string::npos || json[i] != '[')
		return {};

	std::vector<std::string> items;
	for (++i; i < json.size(); ++i)
	{
		const char c = json[i];
		if (c == ']')
			return items;
		if (c == '"')
			items.push_back(JsonDecodeStringAt(json, ++i));
		else if (c != ',' && !isspace(static_cast<unsigned char>(c)))
			break;
	}
	return {};
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
	const size_t apos = JsonFindValue(json, arrayKey);
	if (apos == std::string::npos || json[apos] != '[')
		return {};

	return JsonSplitObjectArray(json, apos);
}
