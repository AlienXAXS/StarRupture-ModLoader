#pragma once
#include <string>

std::wstring GetExeDir();
std::wstring GetExeDirPath(const wchar_t* filename);

// Returns <exedir>\ModLoader\, creating the directory if needed.
std::wstring GetModLoaderDir();

// Returns <exedir>\ModLoader\<filename>.
std::wstring GetModLoaderDirPath(const wchar_t* filename);

void LogStartupEnvironment();
void LoadUE4SS();
