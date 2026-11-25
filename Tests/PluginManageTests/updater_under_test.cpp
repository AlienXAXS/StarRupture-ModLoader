// auto_updater.cpp keeps its sidecar scan file-static, so the test compiles the
// file into this one. Only the scan is called: nothing here reaches the network.
#include "auto_update/auto_updater.cpp"

#include "manager_tests.h"

std::vector<std::string> UpdaterSidecars(const std::wstring& pluginsDir)
{
	std::vector<std::string> names;
	for (const PluginSidecar& sidecar : ScanForSidecars(pluginsDir.c_str()))
		names.push_back(std::string(sidecar.dllFilename.begin(), sidecar.dllFilename.end()));
	return names;
}
