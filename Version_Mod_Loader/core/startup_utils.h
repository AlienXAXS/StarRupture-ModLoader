#pragma once
#include <string>

std::wstring GetExeDir();
std::wstring GetExeDirPath(const wchar_t* filename);
void         LogStartupEnvironment();
void         LoadUE4SS();
