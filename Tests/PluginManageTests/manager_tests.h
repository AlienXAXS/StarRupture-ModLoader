#pragma once
#include <string>
#include <vector>

// The DLL file names the plugin updater would check in this Plugins folder.
std::vector<std::string> UpdaterSidecars(const std::wstring& pluginsDir);

// Copies the test and its plugin into a folder under root and runs the manager
// steps there, one process per game start. Adds every start's checks to the
// totals; a start that fails or reports no checks adds at least one failure.
void RunManagerTests(const std::wstring& root, int& checks, int& failures);

// One game start, in the copied exe. Returns 0 when every check passed.
int RunManagerStart(const std::string& step);
