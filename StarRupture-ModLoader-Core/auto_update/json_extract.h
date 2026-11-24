#pragma once

#include <cctype>
#include <string>

// The loader's minimal JSON readers: no external library, only the flat
// manifest schema. Shared by the update passes (auto_updater.cpp) and
// loader_manifest.h.

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
