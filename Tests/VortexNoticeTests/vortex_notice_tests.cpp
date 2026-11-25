// VortexNoticeTests: did an update replace a file the Vortex mod manager placed
// (auto_update/vortex_notice.h). Each case writes a small game folder under the
// temp directory, with its own vortex.deployment*.json files.
#include "auto_update/vortex_notice.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL: %s\n", what);
		}
	}

	// A deploy time, in milliseconds since 1970 (past 32 bits, as real ones are).
	constexpr long long kDeployedMs = 1763985600000LL;
	constexpr long long kUpdatedMs = kDeployedMs + 3600000LL;

	constexpr const char* kCore = "ModLoader\\StarRupture-ModLoader-Core.dll";
	constexpr const char* kPlugin = "ModLoader\\Plugins\\Compass.dll";

	std::wstring g_dir;

	void Write(const wchar_t* relPath, const std::string& text)
	{
		HANDLE file = CreateFileW((g_dir + L"\\" + relPath).c_str(), GENERIC_WRITE, 0, nullptr,
		                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		DWORD written = 0;
		const bool ok = file != INVALID_HANDLE_VALUE &&
			WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
		Check(ok, "fixture file written");
	}

	void SetModified(const wchar_t* relPath, long long ms)
	{
		const long long ticks = (ms + 11644473600000LL) * 10000;
		FILETIME time{ static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32) };
		HANDLE file = CreateFileW((g_dir + L"\\" + relPath).c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
		                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		const bool ok = file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &time);
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
		Check(ok, "fixture time set");
	}

	void Delete(const wchar_t* relPath)
	{
		DeleteFileW((g_dir + L"\\" + relPath).c_str());
	}

	// One files[] entry as Vortex writes it; relPath is given JSON-escaped.
	std::string Entry(const std::string& relPath, long long time)
	{
		return "{\"relPath\":\"" + relPath + "\",\"source\":\"Some Mod-1-0\",\"target\":\"\",\"time\":" +
			std::to_string(time) + "}";
	}

	std::string Manifest(const std::string& entries)
	{
		return "{\n  \"instance\": \"abc\",\n  \"version\": 1,\n  \"deploymentMethod\": \"hardlink_activator\",\n"
		       "  \"gameId\": \"starrupture\",\n  \"deploymentTime\": 1763985600123,\n"
		       "  \"stagingPath\": \"D:\\\\Vortex\\\\files\",\n  \"targetPath\": \"C:\\\\Game\\\\Win64\",\n"
		       "  \"files\": [" + entries + "]\n}\n";
	}

	const std::string kCoreEntry = Entry("ModLoader\\\\StarRupture-ModLoader-Core.dll", kDeployedMs);
	const std::string kPluginEntry = Entry("ModLoader\\\\Plugins\\\\Compass.dll", kDeployedMs);

	constexpr const wchar_t* kLoaderManifest = L"vortex.deployment.starrupture-modloader.json";
	constexpr const wchar_t* kBinariesManifest = L"vortex.deployment.starrupture-binaries.json";

	bool Replaced(const std::vector<std::string>& paths)
	{
		return VortexNotice::ReplacedVortexFiles(g_dir, paths);
	}

	// Both files on disk, newer than the deploy; no manifests.
	void Reset()
	{
		Delete(kLoaderManifest);
		Delete(kBinariesManifest);
		Delete(L"vortex.deployment.json");
		Write(L"ModLoader\\StarRupture-ModLoader-Core.dll", "new core");
		SetModified(L"ModLoader\\StarRupture-ModLoader-Core.dll", kUpdatedMs);
		Write(L"ModLoader\\Plugins\\Compass.dll", "new plugin");
		SetModified(L"ModLoader\\Plugins\\Compass.dll", kUpdatedMs);
	}

	void TestNotManaged()
	{
		Reset();
		Check(!Replaced({ kCore, kPlugin }), "no manifest: not managed");

		Write(kLoaderManifest, Manifest(Entry("dwmapi.dll", kDeployedMs)));
		Check(!Replaced({ kCore, kPlugin }), "files the manifest does not list: not managed");

		Write(kLoaderManifest, Manifest(""));
		Check(!Replaced({ kCore }), "an empty files list: not managed");

		Write(L"vortex.deployment.json.bak", Manifest(kCoreEntry));
		Check(!Replaced({ kCore }), "a file that is not a .json manifest is not read");
		Delete(L"vortex.deployment.json.bak");

		Write(kLoaderManifest, Manifest(kCoreEntry));
		Check(!Replaced({}), "no replaced files: nothing to say");
	}

	void TestManaged()
	{
		Reset();
		Write(kLoaderManifest, Manifest(Entry("dwmapi.dll", kDeployedMs) + "," + kCoreEntry));
		Check(Replaced({ kCore }), "a listed file with another time was replaced");
		Check(!Replaced({ kPlugin }), "a plugin the loader manifest does not list");
		Check(Replaced({ kPlugin, kCore }), "one listed file among several is enough");

		Reset();
		Write(L"vortex.deployment.json", Manifest(kCoreEntry));
		Check(Replaced({ kCore }), "the default mod type's manifest name is read");
	}

	void TestPluginsOnly()
	{
		Reset();
		Write(kBinariesManifest, Manifest(kPluginEntry));
		Check(Replaced({ kPlugin }), "plugins-only install: the plugin is managed");
		Check(!Replaced({ kCore }), "plugins-only install: the loader is not");

		Write(kLoaderManifest, "{ not finished");
		Check(Replaced({ kPlugin }), "a bad manifest does not hide a good one");
	}

	void TestModifiedTime()
	{
		Reset();
		Write(kLoaderManifest, Manifest(kCoreEntry));
		const wchar_t* core = L"ModLoader\\StarRupture-ModLoader-Core.dll";

		SetModified(core, kDeployedMs);
		Check(!Replaced({ kCore }), "the deployed file (same time) was not replaced");
		SetModified(core, kDeployedMs + 999);
		Check(!Replaced({ kCore }), "Vortex's whole-second time still matches");
		SetModified(core, kDeployedMs - VortexNotice::kTimeToleranceMs - 1000);
		Check(Replaced({ kCore }), "an older file differs too");
		Check(VortexNotice::ModifiedTimeMs(g_dir + L"\\" + core) == kDeployedMs - VortexNotice::kTimeToleranceMs - 1000,
		      "the disk time is read in milliseconds since 1970");

		Delete(core);
		Check(!Replaced({ kCore }), "a listed file that is not on disk");

		Reset();
		Write(kLoaderManifest, Manifest("{\"relPath\":\"ModLoader\\\\StarRupture-ModLoader-Core.dll\",\"source\":\"x\"}"));
		Check(!Replaced({ kCore }), "an entry with no time cannot be compared");
		Write(kLoaderManifest, Manifest("{\"relPath\":\"ModLoader\\\\StarRupture-ModLoader-Core.dll\",\"time\":\"soon\"}"));
		Check(!Replaced({ kCore }), "a time that is not a number");
	}

	void TestPathVariants()
	{
		Reset();
		Write(kLoaderManifest, Manifest(Entry("modloader/plugins/COMPASS.DLL", kDeployedMs)));
		Check(Replaced({ kPlugin }), "case and forward slashes in the manifest");

		Write(kLoaderManifest, Manifest(kPluginEntry));
		Check(Replaced({ "modloader/Plugins/compass.dll" }), "case and forward slashes in the replaced path");

		Write(kLoaderManifest, Manifest(Entry("Compass.dll", kDeployedMs)));
		Check(!Replaced({ kPlugin }), "a file name alone does not match a longer path");

		Write(kLoaderManifest, "\xEF\xBB\xBF" + Manifest(kPluginEntry));
		Check(Replaced({ kPlugin }), "a byte order mark");
	}

	void TestBadManifests()
	{
		Reset();
		const std::string good = Manifest(kCoreEntry);
		const std::string bad[] = {
			"",
			"   \r\n",
			"not json",
			"[" + kCoreEntry + "]",
			good.substr(0, good.size() / 2),
			good.substr(0, good.find_last_of(']')),
			"{}",
			"{\"files\":5}",
			"{\"files\":\"[" "}",
			"{\"files\":[{\"relPath\":\"ModLoader",
			"{\"files\":[}",
			"{\"files\":[{\"relPath\":12,\"time\":1}]}",
		};
		for (const std::string& text : bad)
		{
			Write(kLoaderManifest, text);
			Check(!Replaced({ kCore }), "a malformed manifest means no message");
		}

		std::string huge = good;
		huge.insert(1, static_cast<size_t>(VortexNotice::kMaxManifestBytes), ' ');
		Write(kLoaderManifest, huge);
		Check(!Replaced({ kCore }), "a manifest past the size cap is not read");
	}
}

int main()
{
	wchar_t temp[MAX_PATH]{};
	GetTempPathW(MAX_PATH, temp);
	g_dir = std::wstring(temp) + L"vortex_notice_tests_" + std::to_wstring(GetCurrentProcessId());
	CreateDirectoryW(g_dir.c_str(), nullptr);
	CreateDirectoryW((g_dir + L"\\ModLoader").c_str(), nullptr);
	CreateDirectoryW((g_dir + L"\\ModLoader\\Plugins").c_str(), nullptr);

	TestNotManaged();
	TestManaged();
	TestPluginsOnly();
	TestModifiedTime();
	TestPathVariants();
	TestBadManifests();

	Reset();
	Delete(L"ModLoader\\StarRupture-ModLoader-Core.dll");
	Delete(L"ModLoader\\Plugins\\Compass.dll");
	RemoveDirectoryW((g_dir + L"\\ModLoader\\Plugins").c_str());
	RemoveDirectoryW((g_dir + L"\\ModLoader").c_str());
	RemoveDirectoryW(g_dir.c_str());

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
