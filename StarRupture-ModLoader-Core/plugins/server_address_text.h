#pragma once

#include "plugins/declared_features_text.h"

#include <cstddef>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// server_address_text.h -- the text IPluginNetworkChannel::GetServerAddress
// hands out, as plain data.
//
// The engine prints the connection's remote address as UTF-16 text; the plugin
// gets UTF-8, lower-cased, or nothing. This header holds only those text rules,
// with no game, loader or Windows types, so they can be tested on their own.
// The copy into the caller's buffer is HostRulesWire::CopyTextCut, the same
// text convention as GetClientFeatures and GetDeclaredFeatures.
// ---------------------------------------------------------------------------
namespace ServerAddressText
{
    static_assert(sizeof(wchar_t) == 2, "the engine's text is UTF-16");

    // The longest text taken, in UTF-16 units. An address is far shorter; a
    // longer count means the string is not one.
    constexpr int32_t kMaxUnits = 256;

    // Appends one code point as UTF-8.
    inline void AppendUtf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // UTF-16 to UTF-8. A surrogate that has no partner becomes U+FFFD.
    inline std::string WideToUtf8(const wchar_t* data, size_t units)
    {
        std::string out;
        for (size_t i = 0; i < units; ++i)
        {
            const uint32_t u = static_cast<uint16_t>(data[i]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units)
            {
                const uint32_t lo = static_cast<uint16_t>(data[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                    ++i;
                    continue;
                }
            }
            AppendUtf8(out, (u >= 0xD800 && u <= 0xDFFF) ? 0xFFFDu : u);
        }
        return out;
    }

    // The engine's FString is {data, num (with the NUL), max}. Turns it into the
    // lower-cased UTF-8 address in `out` and returns true, or returns false (and
    // leaves `out` empty) when there is no usable address: null data, no text,
    // a count that is not one, or the engine's literal "Invalid" (what it
    // prints for a connection with no address).
    inline bool FromEngineText(const wchar_t* data, int32_t num, std::string& out)
    {
        out.clear();
        if (!data || num <= 1 || num - 1 > kMaxUnits)
            return false;
        size_t units = 0;
        while (units < static_cast<size_t>(num - 1) && data[units] != L'\0')
            ++units;
        const std::string text = DeclaredFeaturesText::Lower(WideToUtf8(data, units));
        if (text.empty() || text == "invalid")
            return false;
        out = text;
        return true;
    }
}
