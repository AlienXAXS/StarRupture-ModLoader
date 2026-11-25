#pragma once
#include <string>
#include <vector>

// The DLL file names the plugin updater would check in this Plugins folder.
std::vector<std::string> UpdaterSidecars(const std::wstring& pluginsDir);

// Copies the test and its plugin into a folder under root and runs the manager
// steps there, one process per game start. Returns the number of failed starts.
int RunManagerTests(const std::wstring& root);

// One game start, in the copied exe. Returns 0 when every check passed.
int RunManagerStart(const std::string& step);
