#include "pattern_config.h"
#include "../plugin_logger.h"

namespace PatternConfig
{
    std::wstring GetIniPath()
    {
      wchar_t iniPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
        
        wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
        if (lastSlash)
        {
 wcscpy_s(lastSlash + 1,
                static_cast<rsize_t>(MAX_PATH - (lastSlash + 1 - iniPath)),
   L"version-mod.ini");
        }
        
      return iniPath;
  }

    std::string GetPattern(const wchar_t* patternName)
    {
        std::wstring iniPath = GetIniPath();
        
        wchar_t buffer[512]{};
        GetPrivateProfileStringW(L"Patterns", patternName, L"", buffer, 512, iniPath.c_str());
   
        // Convert wide string to narrow string
        if (buffer[0] == L'\0')
  {
            // Convert wchar_t pattern name to char for logging
        char narrowName[256]{};
          WideCharToMultiByte(CP_UTF8, 0, patternName, -1, narrowName, 256, nullptr, nullptr);
  PluginLogger::Debug("Pattern '%s' not found in INI", narrowName);
 return "";
        }

        // Convert wchar_t to char
        char narrowBuffer[512]{};
        WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, 512, nullptr, nullptr);
        
        char narrowName[256]{};
        WideCharToMultiByte(CP_UTF8, 0, patternName, -1, narrowName, 256, nullptr, nullptr);
 PluginLogger::Debug("Pattern '%s' loaded from INI: %s", narrowName, narrowBuffer);
        return std::string(narrowBuffer);
    }

    bool GetHookEnabled(const wchar_t* hookName, bool defaultValue)
    {
        std::wstring iniPath = GetIniPath();
        
        // GetPrivateProfileInt returns 0 if not found, so we use a sentinel value
   int result = GetPrivateProfileIntW(L"HookSettings", hookName, -1, iniPath.c_str());
        
    // If not found (-1), return default value
        if (result == -1)
  {
      char narrowName[256]{};
   WideCharToMultiByte(CP_UTF8, 0, hookName, -1, narrowName, 256, nullptr, nullptr);
 PluginLogger::Debug("Hook setting '%s' not found in INI, using default: %s", 
    narrowName, defaultValue ? "true" : "false");
            return defaultValue;
        }
     
    bool enabled = (result != 0);
  char narrowName[256]{};
WideCharToMultiByte(CP_UTF8, 0, hookName, -1, narrowName, 256, nullptr, nullptr);
        PluginLogger::Info("Hook setting '%s' = %s", narrowName, enabled ? "ENABLED" : "DISABLED");
        return enabled;
    }

    int GetInt(const wchar_t* settingName, int defaultValue)
    {
        std::wstring iniPath = GetIniPath();

     // Use a sentinel that's unlikely to be a real value
        int result = GetPrivateProfileIntW(L"HookSettings", settingName, -9999, iniPath.c_str());

        if (result == -9999)
        {
    char narrowName[256]{};
            WideCharToMultiByte(CP_UTF8, 0, settingName, -1, narrowName, 256, nullptr, nullptr);
            PluginLogger::Debug("Setting '%s' not found in INI, using default: %d", narrowName, defaultValue);
            return defaultValue;
        }

        char narrowName[256]{};
        WideCharToMultiByte(CP_UTF8, 0, settingName, -1, narrowName, 256, nullptr, nullptr);
        PluginLogger::Info("Setting '%s' = %d", narrowName, result);
     return result;
    }
}
