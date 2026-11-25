// The plugin the manager steps load. Copies of this one DLL stand for several
// plugins: each takes its name from its file name, and one whose file name
// starts with "Stuck" can only be unloaded by restarting. What happens to it is
// appended to events.log in the folder of the running exe.
#include "plugins/plugin_interface.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	char g_name[MAX_PATH]{};
	wchar_t g_path[MAX_PATH]{};
	PluginInfo g_info{};

	void Note(const char* what)
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring log = exe;
		log = log.substr(0, log.find_last_of(L'\\') + 1) + L"events.log";
		FILE* file = nullptr;
		if (_wfopen_s(&file, log.c_str(), L"ab") == 0 && file)
		{
			fprintf(file, "%s %s\n", what, g_name);
			fclose(file);
		}
	}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		GetModuleFileNameW(module, g_path, MAX_PATH);
		const std::wstring path = g_path;
		std::wstring stem = path.substr(path.find_last_of(L'\\') + 1);
		stem = stem.substr(0, stem.find_last_of(L'.'));
		WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), -1, g_name, MAX_PATH, nullptr, nullptr);
		Note("attach");
	}
	else if (reason == DLL_PROCESS_DETACH)
		Note("detach");
	return TRUE;
}

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo()
{
	g_info.name = g_name;
	g_info.version = "1.0";
	g_info.author = "test";
	g_info.description = "test plugin";
	g_info.interfaceVersion = PLUGIN_INTERFACE_VERSION;
	g_info.pluginTarget = PLUGIN_TARGET_CLIENT;
	g_info.mapFlags = strncmp(g_name, "Stuck", 5) == 0 ? PluginUnloadSetting(PluginReload::RESTART) : 0;
	return &g_info;
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf*)
{
	Note("init");
	return true;
}

// Says whether its DLL was still at the path it was loaded from.
extern "C" __declspec(dllexport) void PluginShutdown()
{
	Note(GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES ? "shutdown-file-gone" : "shutdown-file-there");
}
