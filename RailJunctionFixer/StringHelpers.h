#pragma once

#include <string>
#include "../src/UtfN.hpp"

namespace RailJunctionFixer::StringHelpers
{
    // Force template instantiation for commonly used conversions
    // This ensures the templates are compiled into this plugin, not across DLL boundaries
    
    inline std::wstring Utf8ToWide(const std::string& utf8Str)
    {
        return UtfN::Utf8StringToUtf16String<std::wstring>(utf8Str);
    }

    inline std::string WideToUtf8(const std::wstring& wideStr)
    {
        return UtfN::Utf16StringToUtf8String<std::string>(wideStr);
    }

    inline std::wstring AnsiToWide(const std::string& ansiStr)
    {
  // For ANSI strings, treat them as UTF-8
        return UtfN::Utf8StringToUtf16String<std::wstring>(ansiStr);
    }
}
