#pragma once

#include <Windows.h>
#include <string>

// ---------------------------------------------------------------------------
// Pattern Configuration Helper
// Reads patterns from version-mod.ini [Patterns] section
// Reads hook enable flags from [HookSettings] section
// ---------------------------------------------------------------------------

namespace PatternConfig
{
    // Get INI file path
    std::wstring GetIniPath();
    
    // Read a pattern from [Patterns] section
    // Returns empty string if not found
    std::string GetPattern(const wchar_t* patternName);
    
    // Read a boolean hook setting from [HookSettings] section
    // Returns the provided default value if not found
    bool GetHookEnabled(const wchar_t* hookName, bool defaultValue = false);

    // Read an integer setting from [HookSettings] section
    // Used for vtable slot indices and other numeric configuration
    // Returns the provided default value if not found
    int GetInt(const wchar_t* settingName, int defaultValue = -1);
}
