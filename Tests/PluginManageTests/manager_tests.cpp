// The manager-level operations the Browse tab calls (RequestPluginChange and
// GetManageSnapshot), on the loader's own plugin_manager.cpp, with real plugin
// DLLs. Each game start is a process of its own, so a restart is a real one.
#include "plugins/plugin_manager.h"
#include "plugins/plugin_manage.h"
#include "plugins/plugin_call_gate.h"
#include "auto_update/auto_updater.h"
#include "auto_update/mod_install.h"
#include "utils/sha256.h"
#include "manager_tests.h"

#undef MoveFileExW

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace PluginManager;
using PluginManage::CardState;

namespace
{
	int g_checks = 0;
	int g_failed = 0;
	std::string g_step;

	void Expect(bool ok, const std::string& what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL [%s] %s\n", g_step.c_str(), what.c_str());
		}
	}

	std::wstring ExeFolder()
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const std::wstring path = exe;
		return path.substr(0, path.find_last_of(L'\\') + 1);
	}

	std::wstring PluginsDir() { return ExeFolder() + L"ModLoader\\Plugins"; }

	bool Installed(const wchar_t* file)
	{
		return GetFileAttributesW((PluginsDir() + L"\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	// What the plugin DLLs wrote since this start began.
	std::vector<std::string> Events()
	{
		std::ifstream in(ExeFolder() + L"events.log");
		std::vector<std::string> lines;
		for (std::string line; std::getline(in, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(line);
		}
		return lines;
	}

	int Count(const std::string& line)
	{
		const std::vector<std::string> events = Events();
		return static_cast<int>(std::count(events.begin(), events.end(), line));
	}

	ManagedPlugin Card(const std::string& file)
	{
		for (const ManagedPlugin& plugin : GetManageSnapshot().plugins)
			if (plugin.fileName == file)
				return plugin;
		return {};
	}

	void ExpectCard(const std::string& file, CardState state, const char* when)
	{
		const ManagedPlugin plugin = Card(file);
		const CardState got = PluginManage::DeriveCard(plugin.facts).state;
		Expect(!plugin.fileName.empty() && got == state, std::string(when) + ": card of " + file + " is state " +
			std::to_string(static_cast<int>(got)) + (plugin.fileName.empty() ? " (no card)" : "") +
			", wanted " + std::to_string(static_cast<int>(state)));
	}

	unsigned Buttons(const std::string& file) { return PluginManage::DeriveCard(Card(file).facts).buttons; }

	void Ask(ManageOp op, const std::string& file, ManageResult wanted, const char* when)
	{
		RequestPluginChange(op, file);
		const ManagedPlugin plugin = Card(file);
		printf("  [%s] %s: op %d on %s -> result %d %s\n", g_step.c_str(), when, static_cast<int>(op), file.c_str(),
			static_cast<int>(plugin.lastResult), plugin.message.c_str());
		Expect(plugin.lastResult == wanted, std::string(when) + ": " + file + " result " +
			std::to_string(static_cast<int>(plugin.lastResult)) + ", wanted " + std::to_string(static_cast<int>(wanted)));
	}

	bool UpdaterChecks(const char* file)
	{
		const std::vector<std::string> names = UpdaterSidecars(PluginsDir());
		return std::find(names.begin(), names.end(), file) != names.end();
	}

	bool MarkedRemoved(const std::string& name)
	{
		PluginStatus statuses[16]{};
		const int count = GetAllPluginStatuses(statuses, 16);
		for (int i = 0; i < count; ++i)
			if (name == statuses[i].name)
				return statuses[i].isRemoved;
		return false;
	}

	const ScanResult* ResultFor(const std::vector<ScanResult>& results, const char* file)
	{
		for (const ScanResult& result : results)
			if (result.fileName == file)
				return &result;
		return nullptr;
	}

	void ExpectNotLoadedByRescan(const char* file, const std::string& name, const char* when)
	{
		const int before = Count("attach " + name);
		std::vector<ScanResult> results;
		ScanForNewPlugins(&results);
		Expect(Count("attach " + name) == before && !ResultFor(results, file),
			std::string(when) + ": a rescan does not load or report " + file);
	}

	std::string Bytes(const std::wstring& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	// One folder of ModLoader\Removed\<stem> holds the three files as they were.
	bool BackupIntact(const std::wstring& stem)
	{
		const std::wstring root = ExeFolder() + L"ModLoader\\Removed\\" + stem;
		const std::string dll = Bytes(ExeFolder() + L"manage_test_plugin.dll");
		WIN32_FIND_DATAW found{};
		const HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			return false;
		bool intact = false;
		do
		{
			const std::wstring base = root + L"\\" + found.cFileName + L"\\" + stem;
			intact = !dll.empty() && Bytes(base + L".dll") == dll && Bytes(base + L".pdb") == "symbols" &&
				Bytes(base + L".json") == "{\"manifest_url\":\"https://example.invalid/m.json\"}";
		} while (!intact && FindNextFileW(search, &found));
		FindClose(search);
		return intact;
	}

	bool WriteText(const std::wstring& path, const char* text)
	{
		std::ofstream out(path, std::ios::binary);
		out << text;
		return static_cast<bool>(out);
	}

	// ---- disable, restarts, enable, uninstall and restore -------------------

	void Disabling()
	{
		for (const char* file : { "A.dll", "B.dll", "StuckC.dll" })
		{
			ExpectCard(file, CardState::Running, "first start");
			Expect(UpdaterChecks(file), std::string("the updater checks ") + file);
		}

		Ask(ManageOp::Disable, "A.dll", ManageResult::Done, "disable");
		ExpectCard("A.dll", CardState::Disabled, "disable");
		Expect(Count("shutdown-file-there A") == 1 && Count("detach A") == 1, "disable unloads A");
		ExpectNotLoadedByRescan("A.dll", "A", "disabled");

		Ask(ManageOp::Disable, "StuckC.dll", ManageResult::AfterRestart, "disable a restart-only plugin");
		ExpectCard("StuckC.dll", CardState::DisablePending, "disable a restart-only plugin");
		Expect(GetModuleHandleW((PluginsDir() + L"\\StuckC.dll").c_str()) != nullptr, "StuckC stays loaded");

		Expect(!UpdaterChecks("A.dll") && !UpdaterChecks("StuckC.dll"), "the updater skips disabled plugins");
		Expect(UpdaterChecks("B.dll"), "the updater still checks B");
	}

	void StillDisabled()
	{
		Expect(Count("attach A") == 0 && Count("attach StuckC") == 0, "no disabled plugin's code ran at start");
		Expect(Count("init B") == 1, "B started");
		ExpectCard("A.dll", CardState::Disabled, "after restart");
		ExpectCard("StuckC.dll", CardState::Disabled, "after restart");
		Expect((Buttons("A.dll") & PluginManage::kEnable) != 0, "A offers Enable");
		ExpectNotLoadedByRescan("A.dll", "A", "after restart");
		const int index = FindPluginIndex("A.dll");
		if (index >= 0)
		{
			ReloadPlugin(index);
			Expect(Count("attach A") == 0, "a reload does not load disabled A");
		}
		Expect(!UpdaterChecks("A.dll") && !UpdaterChecks("StuckC.dll"), "the updater skips them after a restart");

		Ask(ManageOp::Enable, "A.dll", ManageResult::Done, "enable");
		ExpectCard("A.dll", CardState::Running, "enable");
		Expect(Count("init A") == 1, "A started");
		Expect(UpdaterChecks("A.dll"), "the updater checks enabled A");
		Ask(ManageOp::Enable, "StuckC.dll", ManageResult::Done, "enable");
		ExpectCard("StuckC.dll", CardState::Running, "enable");
	}

	void Uninstalling()
	{
		for (const char* file : { "A.dll", "B.dll", "StuckC.dll" })
			ExpectCard(file, CardState::Running, "enabled stays enabled");

		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Done, "uninstall");
		Expect(Count("shutdown-file-gone A") == 1 && Count("detach A") == 1, "files moved first, then A unloaded");
		Expect(!Installed(L"A.dll") && !Installed(L"A.pdb") && !Installed(L"A.json"), "A's three files are gone");
		ExpectCard("A.dll", CardState::Uninstalled, "uninstall");
		Expect((Buttons("A.dll") & PluginManage::kRestore) != 0, "A offers Restore");
		Expect(!UpdaterChecks("A.dll"), "the updater skips uninstalled A");
		ExpectNotLoadedByRescan("A.dll", "A", "uninstalled");
		Expect(!Installed(L"A.dll") && BackupIntact(L"A"), "the rescan leaves A's backup as it was");
		// A sidecar left or put back by itself must not make the updater fetch the DLL.
		Expect(CopyFileW((ExeFolder() + L"ModLoader\\Plugins\\B.json").c_str(),
			(PluginsDir() + L"\\A.json").c_str(), TRUE) != 0 && !UpdaterChecks("A.dll"),
			"the updater passes over a sidecar without its DLL");
		DeleteFileW((PluginsDir() + L"\\A.json").c_str());

		Ask(ManageOp::Uninstall, "StuckC.dll", ManageResult::AfterRestart, "uninstall a restart-only plugin");
		ExpectCard("StuckC.dll", CardState::UninstalledRunning, "uninstall a restart-only plugin");
		Expect(!Installed(L"StuckC.dll") && Count("detach StuckC") == 0, "StuckC's files moved, its code stays");
		Expect(MarkedRemoved("StuckC"), "StuckC is marked removed");

		Ask(ManageOp::Enable, "StuckC.dll", ManageResult::Failed, "enable with the DLL missing");
		Expect(MarkedRemoved("StuckC"), "a failed enable keeps the removed mark");
		ExpectCard("StuckC.dll", CardState::UninstalledRunning, "enable with the DLL missing");

		Ask(ManageOp::Restore, "StuckC.dll", ManageResult::Done, "undo while still loaded");
		ExpectCard("StuckC.dll", CardState::Running, "undo while still loaded");
		Expect(Installed(L"StuckC.dll") && !MarkedRemoved("StuckC"), "StuckC is installed again");
		Ask(ManageOp::Uninstall, "StuckC.dll", ManageResult::AfterRestart, "uninstall again");
	}

	void Restoring()
	{
		Expect(Count("attach A") == 0 && Count("attach StuckC") == 0, "uninstalled plugins stay out after restart");
		ExpectNotLoadedByRescan("A.dll", "A", "uninstalled, after restart");
		ExpectNotLoadedByRescan("StuckC.dll", "StuckC", "uninstalled, after restart");
		Expect(!Installed(L"A.dll") && !Installed(L"StuckC.dll") && BackupIntact(L"A") && BackupIntact(L"StuckC"),
			"the start leaves both backups as they were");
		ExpectCard("A.dll", CardState::Uninstalled, "after restart");
		ExpectCard("StuckC.dll", CardState::Uninstalled, "after restart");
		for (const char* file : { "A.dll", "StuckC.dll" })
		{
			Ask(ManageOp::Restore, file, ManageResult::Done, "restore after restart");
			ExpectCard(file, CardState::Running, "restore after restart");
		}
		Expect(Installed(L"A.pdb") && Installed(L"A.json") && Count("init A") == 1, "A is back, files and all");
	}

	// ---- an uninstall whose rollback fails too ------------------------------

	// The files move dll, pdb, json. Failing the 3rd move starts the rollback,
	// which is the 4th (pdb back) and the 5th (dll back).
	void RollbackLeavesDllOut()
	{
		for (const char* file : { "A.dll", "StuckC.dll" })
		{
			FailMoves(3, 5);
			Ask(ManageOp::Uninstall, file, ManageResult::RollbackFailed, "DLL cannot be put back");
			FailMoves(0, 0);
		}
		Expect(!Installed(L"A.dll") && Installed(L"A.pdb") && Installed(L"A.json"), "only A's DLL stayed out");
		ExpectCard("A.dll", CardState::UninstalledRunning, "DLL cannot be put back");
		Expect((Buttons("A.dll") & PluginManage::kRestore) != 0, "A offers Restore");
		Expect(Count("detach A") == 0, "A is still loaded");
		Expect(!UpdaterChecks("A.dll"), "the updater does not fetch A's DLL back");
	}

	void RestoreAfterRollback()
	{
		Expect(Count("attach A") == 0 && Count("attach StuckC") == 0, "neither loads without its DLL");
		ExpectCard("A.dll", CardState::Uninstalled, "restart after a failed rollback");

		Ask(ManageOp::Restore, "A.dll", ManageResult::Done, "Restore after a failed rollback");
		ExpectCard("A.dll", CardState::Running, "Restore after a failed rollback");

		// The other way out: put the DLL back by hand, then uninstall.
		Expect(CopyFileW((ExeFolder() + L"manage_test_plugin.dll").c_str(),
			(PluginsDir() + L"\\StuckC.dll").c_str(), TRUE) != 0, "copy StuckC back by hand");
		ScanForNewPlugins();
		RefreshManagedFiles();
		ExpectCard("StuckC.dll", CardState::Running, "reinstall by copying the DLL");
		Ask(ManageOp::Uninstall, "StuckC.dll", ManageResult::AfterRestart, "uninstall after reinstalling");
		Expect(!Installed(L"StuckC.dll") && !Installed(L"StuckC.json"), "StuckC uninstalled");
	}

	void RollbackPutsDllBack()
	{
		FailMoves(3, 4);
		Ask(ManageOp::Uninstall, "A.dll", ManageResult::RollbackFailed, "PDB cannot be put back");
		FailMoves(0, 0);
		Expect(Installed(L"A.dll") && !Installed(L"A.pdb") && Installed(L"A.json"), "only A's PDB stayed out");
		ExpectCard("A.dll", CardState::Running, "PDB cannot be put back");

		// The path is not blocked: off and on again works without a restart.
		Ask(ManageOp::Disable, "A.dll", ManageResult::Done, "disable after a failed rollback");
		Ask(ManageOp::Enable, "A.dll", ManageResult::Done, "enable after a failed rollback");
		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Done, "uninstall after a failed rollback");
		ExpectCard("A.dll", CardState::Uninstalled, "uninstall after a failed rollback");
	}

	void RestoreAfterSecondUninstall()
	{
		ExpectCard("A.dll", CardState::Uninstalled, "restart");
		Ask(ManageOp::Restore, "A.dll", ManageResult::Done, "restore the newest backup");
		ExpectCard("A.dll", CardState::Running, "restore the newest backup");
	}

	// ---- a restore leaves a plugin as it was --------------------------------

	void RestoringDisabled()
	{
		Ask(ManageOp::Disable, "A.dll", ManageResult::Done, "disable");
		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Done, "uninstall a disabled plugin");
		const int attaches = Count("attach A");
		Ask(ManageOp::Restore, "A.dll", ManageResult::Done, "restore a disabled plugin");
		ExpectCard("A.dll", CardState::Disabled, "restore a disabled plugin");
		Expect(Installed(L"A.dll"), "A's DLL is back");
		Expect(Count("attach A") == attaches, "the restore runs none of A's code");
		Expect(!UpdaterChecks("A.dll"), "the updater skips restored, disabled A");
		ExpectNotLoadedByRescan("A.dll", "A", "restored disabled");
	}

	void RestoredStaysDisabled()
	{
		Expect(Count("attach A") == 0, "restored, disabled A does not run at the next start");
		ExpectCard("A.dll", CardState::Disabled, "start after the restore");
		Ask(ManageOp::Enable, "A.dll", ManageResult::Done, "enable the restored plugin");
		ExpectCard("A.dll", CardState::Running, "enable the restored plugin");
	}

	// ---- a plugin call that does not return ---------------------------------

	int IndexOf(const std::string& name)
	{
		PluginStatus statuses[16]{};
		const int count = GetAllPluginStatuses(statuses, 16);
		for (int i = 0; i < count; ++i)
			if (name == statuses[i].name)
				return i;
		return -1;
	}

	void BusyCall()
	{
		std::atomic<bool> inside{ false }, leave{ false };
		std::thread call([&]
		{
			const PluginCallGate::Scope gate;
			inside = true;
			while (!leave) Sleep(1);
		});
		while (!inside) Sleep(1);

		std::string why;
		Expect(!UnloadPlugin(IndexOf("A"), &why) && why.find("nothing was unloaded") != std::string::npos &&
			why.find("RELOAD") == std::string::npos, "a blocked unload does not send anyone to RELOAD: " + why);
		Expect(!ReloadPlugin(IndexOf("A"), &why) && why.find("nothing was reloaded") != std::string::npos,
			"a blocked reload says so: " + why);
		Ask(ManageOp::Disable, "A.dll", ManageResult::AfterRestart, "disable during the call");
		const std::string message = Card("A.dll").message;
		Expect(message.find("nothing was unloaded") != std::string::npos &&
			message.find("RELOAD") == std::string::npos, "Disable gives the unload note: " + message);
		// A is now disabled but still running: nothing may send anyone to RELOAD.
		Expect(!ReloadPlugin(IndexOf("A"), &why) && why.find("disabled") != std::string::npos &&
			why.find("RELOAD") == std::string::npos, "no RELOAD retry for a disabled plugin: " + why);
		PluginStatus rows[16]{};
		const int row = IndexOf("A");
		Expect(row >= 0 && row < GetAllPluginStatuses(rows, 16) &&
			std::string(rows[row].reloadBlock).find("disabled") != std::string::npos,
			"the plugin list shows why in place of the RELOAD button");
		Expect(Count("shutdown-file-there A") == 0 && Count("detach A") == 0, "A is left as it was");
		Expect(!UnloadPlugin(IndexOf("StuckC"), &why) && why.find("requires a game restart") != std::string::npos &&
			why.find("again") == std::string::npos,
			"no retry is offered where the unload is refused anyway: " + why);

		leave = true;
		call.join();
		Expect(UnloadPlugin(IndexOf("A"), &why) && Count("detach A") == 1, "UNLOAD finishes it afterwards: " + why);
	}

	// ---- files put into Plugins while the game runs -------------------------

	bool PutPlugin(const wchar_t* file)
	{
		return CopyFileW((ExeFolder() + L"manage_test_plugin.dll").c_str(),
			(PluginsDir() + L"\\" + file).c_str(), TRUE) != 0;
	}

	void CopiedBack()
	{
		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Done, "uninstall");
		Ask(ManageOp::Uninstall, "StuckC.dll", ManageResult::AfterRestart, "uninstall a restart-only plugin");
		Expect(Count("init A") == 1 && Count("detach A") == 1, "A is unloaded");
		Expect(PutPlugin(L"A.dll") && PutPlugin(L"StuckC.dll"), "copy both back by hand");

		std::vector<ScanResult> results;
		const int live = ScanForNewPlugins(&results);
		const ScanResult* a = ResultFor(results, "A.dll");
		const ScanResult* stuck = ResultFor(results, "StuckC.dll");
		Expect(live == 1 && Count("init A") == 2, "Load New starts the A that was put back");
		Expect(a && a->status == ScanStatus::Live, "Load New reports A as loaded");
		Expect(stuck && stuck->status == ScanStatus::NeedsRestart, "StuckC's old code is mapped: restart");
		Expect(Count("attach StuckC") == 1, "StuckC is not loaded a second time");
		ExpectCard("A.dll", CardState::Running, "put back by hand");
	}

	void RestoreBesideACopy()
	{
		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Done, "uninstall");
		Expect(PutPlugin(L"A~new.dll"), "the same plugin under another file name");
		const int attaches = Count("attach A");

		// Not loaded yet: the loader reads the name without running the DLL.
		Ask(ManageOp::Restore, "A.dll", ManageResult::AlreadyInstalled, "restore beside an unloaded copy");
		Expect(!Installed(L"A.dll"), "the restore moved nothing back");
		Expect(Card("A.dll").message.find("A~new.dll") != std::string::npos, "the refusal names the other file");
		Expect(Count("attach A") == attaches, "reading the copy's name runs none of its code");

		ScanForNewPlugins();
		Expect(Count("attach A") == attaches + 1, "Load New starts the copy");
		Ask(ManageOp::Restore, "A.dll", ManageResult::AlreadyInstalled, "restore beside a running copy");
		Expect(!Installed(L"A.dll") && Installed(L"A~new.dll"), "only the copy is installed");
	}

	// ---- a plugin Vortex deployed -------------------------------------------

	// The manifest is in the game folder, and the loader's own search finds it.
	// A's entry is under a "target" folder.
	void VortexDeployed()
	{
		Expect(WriteText(ExeFolder() + L"vortex.deployment.json",
			R"({"files":[{"relPath":"Plugins\\A.dll","target":"ModLoader","time":1700000000000}]})"),
			"write the manifest");
		RefreshManagedFiles();

		Expect(Card("A.dll").facts.vortexListed, "A is listed by Vortex");
		Expect(PluginManage::DeriveCard(Card("A.dll").facts).uninstallInVortex &&
			(Buttons("A.dll") & PluginManage::kUninstall) == 0, "A's card sends Uninstall to Vortex");
		Expect(!Card("B.dll").facts.vortexListed && (Buttons("B.dll") & PluginManage::kUninstall) != 0,
			"B offers Uninstall");

		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Vortex, "uninstall a Vortex plugin");
		Expect(Installed(L"A.dll") && Installed(L"A.pdb") && Installed(L"A.json") && Count("detach A") == 0,
			"A is untouched");
		Ask(ManageOp::Uninstall, "B.dll", ManageResult::Done, "uninstall a plugin Vortex does not list");
	}

	// ---- Browse install -------------------------------------------------------

	// What the installer's one GET gets, and how often it was asked.
	ModInstall::Hop g_answer;
	std::atomic<int> g_hopCalls{ 0 };

	ModIndex::Entry InstallEntry(const char* stem, const char* version, const std::string& bytes)
	{
		ModIndex::Entry entry;
		entry.id = entry.name = stem;
		entry.version = version;
		entry.install.dllFile = std::string(stem) + ".dll";
		entry.install.url = "https://github.com/owner/repo/releases/download/v1/" + entry.install.dllFile;
		entry.install.sha256 = Sha256::OfBytes(bytes);
		entry.install.size = bytes.size();
		return entry;
	}

	// The card's own result, once the install has ended.
	ManagedPlugin InstallAndWait(const ModIndex::Entry& entry, const std::vector<ModIndex::Entry>& list,
		bool listSigned, ModInstall::Installer& installer)
	{
		RequestPluginInstall(entry, list, listSigned, installer);
		for (int i = 0; i < 500 && Card(entry.install.dllFile).facts.working; ++i)
			Sleep(20);
		const ManagedPlugin card = Card(entry.install.dllFile);
		printf("  [%s] install %s -> result %d %s\n", g_step.c_str(), entry.install.dllFile.c_str(),
			static_cast<int>(card.lastResult), card.message.c_str());
		Expect(!card.facts.working && !GetManageSnapshot().installing, "the install has ended");
		Expect(card.lastOp == ManageOp::Install, "the card shows an install");
		return card;
	}

	// Files in Plugins whose name starts with `stem`, and any install temp file.
	int Leftovers(const wchar_t* stem)
	{
		int found = 0;
		WIN32_FIND_DATAW data{};
		const HANDLE search = FindFirstFileW((PluginsDir() + L"\\*").c_str(), &data);
		if (search == INVALID_HANDLE_VALUE)
			return 0;
		do
		{
			const std::wstring name = data.cFileName;
			if (name.rfind(stem, 0) == 0 || name.find(L".install.tmp") != std::wstring::npos)
				++found;
		} while (FindNextFileW(search, &data));
		FindClose(search);
		return found;
	}

	std::string StoredVersion(const wchar_t* file)
	{
		wchar_t value[64]{};
		GetPrivateProfileStringW(L"PluginVersions", file, L"", value, 64,
			(ExeFolder() + L"ModLoader\\update_state.ini").c_str());
		return std::string(value, value + wcslen(value));
	}

	void Installing()
	{
		const std::string bytes = Bytes(ExeFolder() + L"manage_test_plugin.dll");
		g_answer = { 200, "", bytes };
		ModInstall::Installer installer([](const std::string&, size_t, unsigned, const std::atomic<bool>&)
		{
			++g_hopCalls;
			return g_answer;
		});
		const std::wstring ini = ExeFolder() + L"ModLoader\\modloader.ini";

		// A list that is not signed installs nothing.
		const ModIndex::Entry fresh = InstallEntry("Fresh", "1.0.0", bytes);
		ManagedPlugin card = InstallAndWait(fresh, { fresh }, false, installer);
		Expect(card.lastResult == ManageResult::Failed && card.message.find("isn't signed") != std::string::npos &&
			g_hopCalls == 0 && !Installed(L"Fresh.dll"), "an unsigned list from the default address is refused");

		// A plugin that is already there.
		const ModIndex::Entry same = InstallEntry("A", "9.9.9", bytes);
		const std::string aBefore = Bytes(PluginsDir() + L"\\A.dll");
		card = InstallAndWait(same, { same }, true, installer);
		Expect(card.lastResult == ManageResult::Failed && card.message.find("A.dll is already in") != std::string::npos &&
			g_hopCalls == 0 && Bytes(PluginsDir() + L"\\A.dll") == aBefore && !aBefore.empty(),
			"an existing A.dll is refused before any download and is left as it was");
		ExpectCard("A.dll", CardState::Running, "an existing A.dll is refused");

		// A server_only entry is refused even when the list gives it install fields.
		ModIndex::Entry serverEntry = InstallEntry("Srv", "1.0.0", bytes);
		serverEntry.serverOnly = true;
		Expect(!InstallBlockedReason(serverEntry, { serverEntry }, GetManageSnapshot()).empty(),
			"a server_only entry has no install, so it is blocked");
		card = InstallAndWait(serverEntry, { serverEntry }, true, installer);
		Expect(card.lastResult == ManageResult::Failed && g_hopCalls == 0 && !Installed(L"Srv.dll") &&
			Leftovers(L"Srv") == 0, "a server_only entry is refused at the gate, nothing fetched or written");

		// Requirements.
		ModIndex::Entry needs = InstallEntry("Needs", "1.0.0", bytes);
		needs.requiredIds = { "ghost" };
		card = InstallAndWait(needs, { needs }, true, installer);
		Expect(card.lastResult == ManageResult::Failed && card.message.find("ghost") != std::string::npos &&
			g_hopCalls == 0 && !Installed(L"Needs.dll"), "a missing requirement is named and nothing is fetched");

		Ask(ManageOp::Disable, "B.dll", ManageResult::Done, "disable B");
		needs.requiredIds = { "B" };
		const ModIndex::Entry b = InstallEntry("B", "1.0.0", bytes);
		card = InstallAndWait(needs, { needs, b }, true, installer);
		Expect(card.lastResult == ManageResult::Failed && card.message.find("Enable B first") != std::string::npos &&
			g_hopCalls == 0 && !Installed(L"Needs.dll"), "a disabled requirement is refused");
		Ask(ManageOp::Enable, "B.dll", ManageResult::Done, "enable B");

		// A download that fails leaves nothing.
		ModIndex::Entry wrong = InstallEntry("Wrong", "1.0.0", bytes);
		wrong.install.sha256 = Sha256::OfBytes("something else");
		card = InstallAndWait(wrong, { wrong }, true, installer);
		Expect(card.lastResult == ManageResult::Failed && !card.message.empty() && g_hopCalls == 1 &&
			Leftovers(L"Wrong") == 0, "a bad hash leaves nothing and says why");

		g_answer = {};
		g_hopCalls = 0;
		const ModIndex::Entry silent = InstallEntry("Silent", "1.0.0", bytes);
		card = InstallAndWait(silent, { silent }, true, installer);
		Expect(card.lastResult == ManageResult::Failed && !card.message.empty() && g_hopCalls == 1 &&
			Leftovers(L"Silent") == 0, "a hop with no answer leaves nothing and says why");
		g_answer = { 200, "", bytes };

		// Live load: A was uninstalled and is on the disabled list, with a newer version on record.
		Ask(ManageOp::Uninstall, "A.dll", ManageResult::Done, "uninstall A");
		Expect(PluginManage::SaveDisabled(ini, L"A.dll", true) == PluginManage::SaveResult::Ok, "A is disabled");
		SetInstalledPluginVersion(L"A.dll", "99.0.0");
		const int inits = Count("init A");
		card = InstallAndWait(same, { same }, true, installer);
		Expect(card.lastResult == ManageResult::Done && card.listSigned, "install A: loaded now");
		ExpectCard("A.dll", CardState::Running, "install A");
		Expect(Installed(L"A.dll") && Count("init A") == inits + 1, "install A: its code started");
		Expect(StoredVersion(L"A.dll") == "9.9.9", "install A: the stored version is the entry's, not 99.0.0");
		Expect(!Card("A.dll").facts.listedDisabled, "install A: no longer disabled");

		// Restart: StuckC's old code is still mapped. A signed list from the player's own address installs.
		Ask(ManageOp::Uninstall, "StuckC.dll", ManageResult::AfterRestart, "uninstall StuckC");
		Expect(WritePrivateProfileStringW(L"AutoUpdate", L"ModIndexUrl", L"https://example.test/index.json",
			ini.c_str()) != 0, "the player sets ModIndexUrl");
		const ModIndex::Entry stuck = InstallEntry("StuckC", "1.0.0", bytes);
		const int attaches = Count("attach StuckC");
		// The same address serving a list that is not signed is refused.
		g_hopCalls = 0;
		card = InstallAndWait(stuck, { stuck }, false, installer);
		Expect(card.lastResult == ManageResult::Failed && card.message.find("isn't signed") != std::string::npos &&
			g_hopCalls == 0 && !Installed(L"StuckC.dll"), "an unsigned list from the player's own address is refused");
		card = InstallAndWait(stuck, { stuck }, true, installer);
		Expect(card.lastResult == ManageResult::AfterRestart && card.listSigned, "install StuckC: waits for a restart");
		Expect(Installed(L"StuckC.dll") && Count("attach StuckC") == attaches, "install StuckC: files in, code not loaded twice");

		// One install at a time: while a slow one runs, another entry is refused
		// and never reaches the network. The slow one ends on a wrong hash.
		{
			g_hopCalls = 0;
			std::atomic<bool> release{ false };
			ModInstall::Installer slow([&](const std::string&, size_t, unsigned, const std::atomic<bool>&)
			{
				++g_hopCalls;
				while (!release)
					Sleep(2);
				return g_answer;
			});
			ModIndex::Entry slowEntry = InstallEntry("SlowOne", "1.0.0", bytes);
			slowEntry.install.sha256 = Sha256::OfBytes("something else");
			const ModIndex::Entry other = InstallEntry("Other", "1.0.0", bytes);
			RequestPluginInstall(slowEntry, { slowEntry, other }, true, slow);
			Expect(GetManageSnapshot().installing && Card("SlowOne.dll").facts.working, "a slow install is running");
			RequestPluginInstall(other, { slowEntry, other }, true, slow);
			const ManagedPlugin refused = Card("Other.dll");
			Expect(refused.lastResult == ManageResult::Failed && !refused.facts.working &&
				refused.message.find("Another mod is installing") != std::string::npos && !Installed(L"Other.dll"),
				"a second install is refused while one runs");
			Expect(GetManageSnapshot().installing && Card("SlowOne.dll").facts.working,
				"and the first is still running");
			release = true;
			for (int i = 0; i < 500 && Card("SlowOne.dll").facts.working; ++i)
				Sleep(20);
			Expect(!Card("SlowOne.dll").facts.working && !GetManageSnapshot().installing &&
				Card("SlowOne.dll").lastResult == ManageResult::Failed && g_hopCalls == 1 &&
				Leftovers(L"SlowOne") == 0 && Leftovers(L"Other") == 0,
				"the slow install ends, and the refused one never reached the network");
			slow.Shutdown(2000);
		}
		installer.Shutdown(2000);
	}

	struct Step { const char* name; void (*run)(); };
	const Step kSteps[] = {
		{ "disable", Disabling }, { "still-disabled", StillDisabled }, { "uninstall", Uninstalling },
		{ "restore", Restoring },
		{ "rollback-dll-out", RollbackLeavesDllOut }, { "rollback-dll-out-restart", RestoreAfterRollback },
		{ "rollback-dll-back", RollbackPutsDllBack }, { "rollback-dll-back-restart", RestoreAfterSecondUninstall },
		{ "restore-disabled", RestoringDisabled }, { "restored-stays-disabled", RestoredStaysDisabled },
		{ "copied-back", CopiedBack }, { "duplicate-name", RestoreBesideACopy }, { "vortex", VortexDeployed },
		{ "install", Installing },
		{ "busy-call", BusyCall },
	};

	struct Game
	{
		std::vector<const wchar_t*> plugins;
		std::vector<const wchar_t*> starts;
	};

	// A start leaves its counts beside its exe: the exit code alone cannot carry them.
	void Start(const std::wstring& exe, const std::wstring& step, const std::wstring& countsFile,
		int& checks, int& failures)
	{
		DeleteFileW(countsFile.c_str());
		std::wstring command = L"\"" + exe + L"\" --manager " + step;
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		fflush(stdout);
		if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
		{
			++checks;
			++failures;
			printf("FAIL start %ls could not be run\n", step.c_str());
			return;
		}
		DWORD code = 1;
		if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0)
			TerminateProcess(process.hProcess, 1);
		else
			GetExitCodeProcess(process.hProcess, &code);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		int ran = 0;
		int bad = 0;
		std::ifstream in(countsFile);
		const bool reported = static_cast<bool>(in >> ran >> bad) && ran > 0;
		checks += reported ? ran : 0;
		failures += reported ? bad : 0;
		if (!reported || (code != 0 && bad == 0))
		{
			++checks;
			++failures;
			printf("FAIL start %ls ended with code %lu%s\n", step.c_str(), code, reported ? "" : " and no counts");
		}
	}
}

int RunManagerStart(const std::string& step)
{
	g_step = step;
	InitializePluginManager();
	LoadAllPlugins();
	InitAllLoadedPlugins();
	MarkStartupComplete();
	RefreshManagedFiles();

	bool known = false;
	for (const Step& s : kSteps)
		if (step == s.name)
		{
			known = true;
			s.run();
		}
	Expect(known, "known step");
	printf("start %s: %d checks, %d failed\n", step.c_str(), g_checks, g_failed);
	fflush(stdout);
	const bool saved = WriteText(ExeFolder() + L"counts.txt",
		(std::to_string(g_checks) + " " + std::to_string(g_failed)).c_str());
	return g_failed || !saved ? 1 : 0;
}

void RunManagerTests(const std::wstring& root, int& checks, int& failures)
{
	const Game games[] = {
		{ { L"A", L"B", L"StuckC" }, { L"disable", L"still-disabled", L"uninstall", L"restore" } },
		{ { L"A", L"StuckC" }, { L"rollback-dll-out", L"rollback-dll-out-restart" } },
		{ { L"A" }, { L"rollback-dll-back", L"rollback-dll-back-restart" } },
		{ { L"A" }, { L"restore-disabled", L"restored-stays-disabled" } },
		{ { L"A", L"StuckC" }, { L"copied-back" } },
		{ { L"A" }, { L"duplicate-name" } },
		{ { L"A", L"B" }, { L"vortex" } },
		{ { L"A", L"B", L"StuckC" }, { L"install" } },
		{ { L"A", L"StuckC" }, { L"busy-call" } },
	};

	wchar_t self[MAX_PATH]{};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	const std::wstring plugin = ExeFolder() + L"manage_test_plugin.dll";

	int number = 0;
	for (const Game& game : games)
	{
		const std::wstring dir = root + L"\\game" + std::to_wstring(++number);
		const std::wstring plugins = dir + L"\\ModLoader\\Plugins";
		CreateDirectoryW(dir.c_str(), nullptr);
		CreateDirectoryW((dir + L"\\ModLoader").c_str(), nullptr);
		CreateDirectoryW(plugins.c_str(), nullptr);

		bool ready = CopyFileW(self, (dir + L"\\game.exe").c_str(), TRUE) != 0 &&
			CopyFileW(plugin.c_str(), (dir + L"\\manage_test_plugin.dll").c_str(), TRUE) != 0;
		for (const wchar_t* name : game.plugins)
		{
			const std::wstring stem = plugins + L"\\" + name;
			ready = ready && CopyFileW(plugin.c_str(), (stem + L".dll").c_str(), TRUE) != 0 &&
				WriteText(stem + L".pdb", "symbols") &&
				WriteText(stem + L".json", "{\"manifest_url\":\"https://example.invalid/m.json\"}");
		}
		if (!ready)
		{
			printf("FAIL could not set up %ls (is manage_test_plugin.dll built?)\n", dir.c_str());
			++checks;
			++failures;
			continue;
		}

		for (const wchar_t* step : game.starts)
		{
			DeleteFileW((dir + L"\\events.log").c_str());
			Start(dir + L"\\game.exe", step, dir + L"\\counts.txt", checks, failures);
		}
	}
}
