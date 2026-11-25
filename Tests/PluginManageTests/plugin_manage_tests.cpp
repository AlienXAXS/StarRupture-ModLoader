// PluginManageTests: the rules behind disabling and uninstalling a plugin
// (plugins/plugin_manage.h) and the "did Vortex place this file" question. Every
// case works in its own folder under the temp directory. The last part is a
// scripted run (disable, enable, uninstall, restore) that prints each step and
// the files after it.
#include "plugins/plugin_manage.h"
#include "auto_update/mod_index.h"
#include "auto_update/vortex_notice.h"
#include "manager_tests.h"

#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace PluginManage;

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

	std::wstring g_root; // the temp folder of this run
	std::wstring g_dir;  // the ModLoader folder of the current case
	Folders g_folders;
	std::wstring g_ini;
	const std::wstring kStamp = L"20260101-120000";

	std::string Utf8(const std::wstring& text)
	{
		const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string out(length > 0 ? length - 1 : 0, '\0');
		if (length > 1)
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
		return out;
	}

	bool Exists(const std::wstring& rel)
	{
		return GetFileAttributesW((g_dir + L"\\" + rel).c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	void Write(const std::wstring& rel, const std::string& text)
	{
		for (size_t slash = rel.find(L'\\'); slash != std::wstring::npos; slash = rel.find(L'\\', slash + 1))
			CreateDirectoryW((g_dir + L"\\" + rel.substr(0, slash)).c_str(), nullptr);
		HANDLE file = CreateFileW((g_dir + L"\\" + rel).c_str(), GENERIC_WRITE, 0, nullptr,
		                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		DWORD written = 0;
		const bool ok = file != INVALID_HANDLE_VALUE &&
			WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
		Check(ok, "fixture file written");
	}

	// FNV-1a of a file's bytes: two files of one size still differ.
	std::wstring ContentHash(const std::wstring& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return L"unread";
		unsigned long long hash = 14695981039346656037ull;
		for (char c; in.get(c);)
			hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
		return std::to_wstring(hash);
	}

	// Every file and folder below dir, sorted: "relative path:size:content hash"
	// for a file. A link is listed and not followed.
	void Walk(const std::wstring& dir, const std::wstring& prefix, std::vector<std::wstring>& out)
	{
		WIN32_FIND_DATAW found{};
		const HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			return;
		do
		{
			const std::wstring name = found.cFileName;
			if (name == L"." || name == L"..")
				continue;
			const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			const bool link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
			out.push_back(prefix + name + (folder ? L"\\" : L":" + std::to_wstring(found.nFileSizeLow) +
				(link ? std::wstring() : L":" + ContentHash(dir + L"\\" + name))));
			if (folder && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				Walk(dir + L"\\" + name, prefix + name + L"\\", out);
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}

	std::vector<std::wstring> Tree()
	{
		std::vector<std::wstring> out;
		Walk(g_dir, L"", out);
		std::sort(out.begin(), out.end());
		return out;
	}

	// Delete a folder and everything in it; a link is removed, never entered.
	void Wipe(const std::wstring& dir)
	{
		const DWORD own = GetFileAttributesW(dir.c_str());
		if (own == INVALID_FILE_ATTRIBUTES)
			return;
		WIN32_FIND_DATAW found{};
		const HANDLE search = (own & FILE_ATTRIBUTE_REPARSE_POINT)
			? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"\\*").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = found.cFileName;
				if (name == L"." || name == L"..")
					continue;
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					Wipe(dir + L"\\" + name);
				else
					DeleteFileW((dir + L"\\" + name).c_str());
			} while (FindNextFileW(search, &found));
			FindClose(search);
		}
		RemoveDirectoryW(dir.c_str());
	}

	// A new ModLoader folder with an empty Plugins folder.
	void Fresh(const wchar_t* name)
	{
		g_dir = g_root + L"\\" + name;
		Wipe(g_dir);
		CreateDirectoryW(g_dir.c_str(), nullptr);
		CreateDirectoryW((g_dir + L"\\Plugins").c_str(), nullptr);
		g_folders = { g_dir + L"\\Plugins", g_dir + L"\\Removed" };
		g_ini = g_dir + L"\\modloader.ini";
	}

	// X.dll, X.pdb and X.json, beside everything that must stay.
	void PluginWithNeighbours()
	{
		Write(L"Plugins\\Compass.dll", "dll");
		Write(L"Plugins\\Compass.pdb", "pdb!");
		Write(L"Plugins\\Compass.json", "json!!");
		Write(L"Plugins\\Compass.ini", "settings");
		Write(L"Plugins\\Compass.dll.bak", "lookalike");
		Write(L"Plugins\\Compass.json.old", "lookalike");
		Write(L"Plugins\\Compass2.dll", "lookalike");
		Write(L"Plugins\\Compass-data.json", "lookalike");
		Write(L"Plugins\\config\\Compass.ini", "settings");
		Write(L"Plugins\\Compass\\save.dat", "save data");
		Write(L"Plugins\\Other.dll", "other plugin");
	}

	// Succeeds as the real move does, except for the calls named (counted from 1).
	MoveFn FailingMove(std::vector<int> failures)
	{
		auto calls = std::make_shared<int>(0);
		return [calls, failures](const std::wstring& from, const std::wstring& to) -> DWORD
		{
			++*calls;
			if (std::find(failures.begin(), failures.end(), *calls) != failures.end())
				return ERROR_SHARING_VIOLATION;
			return MoveOnSameVolume(from, to);
		};
	}

	// A directory junction: a reparse point any user may make.
	bool MakeJunction(const std::wstring& link, const std::wstring& target)
	{
		if (!CreateDirectoryW(link.c_str(), nullptr))
			return false;
		const std::wstring substitute = L"\\??\\" + target;
		struct MountPoint
		{
			DWORD tag;
			WORD dataLength, reserved, substituteOffset, substituteLength, printOffset, printLength;
			wchar_t path[1024];
		} data{};
		const WORD bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
		data.tag = IO_REPARSE_TAG_MOUNT_POINT;
		data.substituteLength = bytes;
		data.printOffset = bytes + sizeof(wchar_t);
		wmemcpy(data.path, substitute.c_str(), substitute.size());
		data.dataLength = static_cast<WORD>(8 + bytes + 2 * sizeof(wchar_t));

		const HANDLE dir = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (dir == INVALID_HANDLE_VALUE)
			return false;
		DWORD returned = 0;
		const bool ok = DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, &data, data.dataLength + 8,
		                                nullptr, 0, &returned, nullptr) != 0;
		CloseHandle(dir);
		return ok;
	}

	// --- The disabled list ------------------------------------------------------

	void TestSplit()
	{
		// std::string is what the preload manager splits.
		const std::vector<std::string> narrow = SplitNameList(std::string(" a.dll ,\tb c.dll\t,, ,d.dll,"));
		Check(narrow == std::vector<std::string>({ "a.dll", "b c.dll", "d.dll" }), "split: spaces and tabs trimmed, empty items dropped");
		Check(SplitNameList(std::string()).empty() && SplitNameList(std::string(" , ,")).empty(), "split: nothing to split");
		Check(SplitNameList(std::string("\ra\n")) == std::vector<std::string>({ "\ra\n" }), "split: only spaces and tabs are trimmed");
		Check(SplitNameList(std::wstring(L"x.dll")) == std::vector<std::wstring>({ L"x.dll" }), "split: wide");
	}

	void TestEditList()
	{
		std::wstring out = L"untouched";
		Check(EditDisabledList(L"", L"A.dll", true, out) == ListEdit::Ok && out == L"A.dll", "edit: first name");
		Check(EditDisabledList(L" A.dll , B.dll", L"a.DLL", true, out) == ListEdit::Ok && out == L"B.dll,a.DLL", "edit: no duplicate, case aside");
		Check(EditDisabledList(L"A.dll,B.dll,a.dll", L"A.DLL", false, out) == ListEdit::Ok && out == L"B.dll", "edit: every spelling removed");
		Check(EditDisabledList(L"B.dll", L"A.dll", false, out) == ListEdit::Ok && out == L"B.dll", "edit: removing an absent name");

		out = L"untouched";
		Check(EditDisabledList(L"", L"A,B.dll", true, out) == ListEdit::BadName && out == L"untouched", "edit: comma refused");
		Check(!CanBeDisabled(L"A,B.dll") && CanBeDisabled(L"A B.dll"), "edit: which names can be disabled");
		Check(EditDisabledList(L"", L"..\\A.dll", true, out) == ListEdit::BadName, "edit: a path is refused");

		const std::wstring full(kMaxDisabledValueChars, L'x');
		Check(EditDisabledList(full, L"A.dll", true, out) == ListEdit::TooLong && out == L"untouched", "edit: overflow refused");
		Check(EditDisabledList(full + L"x", L"A.dll", false, out) == ListEdit::TooLong, "edit: a value that may be cut is not rewritten");
		const std::wstring room(kMaxDisabledValueChars - 6, L'x');
		Check(EditDisabledList(room, L"A.dll", true, out) == ListEdit::Ok && out.size() == kMaxDisabledValueChars, "edit: exactly full fits");
	}

	void TestIni()
	{
		Fresh(L"ini");
		std::vector<std::wstring> names;
		Check(ReadDisabledList(g_ini, names) && names.empty(), "ini: no file means nothing disabled");

		Write(L"modloader.ini", "[Preload]\r\nDisabled=P.dll\r\n[Plugins]\r\nDisabled= A.dll ,\tB.dll\r\nOther=1\r\n");
		Check(ReadDisabledList(g_ini, names) && names == std::vector<std::wstring>({ L"A.dll", L"B.dll" }), "ini: a hand-edited list reads");
		Check(SaveDisabled(g_ini, L"C.dll", true) == SaveResult::Ok, "ini: disable saves");
		Check(SaveDisabled(g_ini, L"a.dll", false) == SaveResult::Ok, "ini: enable saves");
		Check(ReadDisabledList(g_ini, names) && names == std::vector<std::wstring>({ L"B.dll", L"C.dll" }), "ini: round trip");
		Check(ListHas(names, L"b.DLL") && !ListHas(names, L"A.dll"), "ini: names compare case aside");
		wchar_t value[64]{};
		GetPrivateProfileStringW(L"Preload", L"Disabled", L"", value, 64, g_ini.c_str());
		Check(std::wstring(value) == L"P.dll", "ini: the preload list is left alone");
		GetPrivateProfileStringW(L"Plugins", L"Other", L"", value, 64, g_ini.c_str());
		Check(std::wstring(value) == L"1", "ini: another key is left alone");
		Check(SaveDisabled(g_ini, L"A,B.dll", true) == SaveResult::BadName, "ini: comma refused");

		const std::string longList(kMaxDisabledValueChars - 2, 'x');
		Write(L"modloader.ini", "[Plugins]\r\nDisabled=" + longList + "\r\n");
		const std::vector<std::wstring> before = Tree();
		Check(SaveDisabled(g_ini, L"A.dll", true) == SaveResult::TooLong && Tree() == before, "ini: a full list is refused untouched");

		Write(L"modloader.ini", "[Plugins]\r\nDisabled=" + std::string(kDisabledBufferChars + 50, 'x') + "\r\n");
		Check(!ReadDisabledList(g_ini, names), "ini: a list too long to read whole says so");

		Write(L"modloader.ini", "[Plugins]\r\nDisabled=B.dll\r\n");
		if (GetACP() != CP_UTF8)
		{
			Check(SaveDisabled(g_ini, L"\x4E2D\x6587.dll", true) == SaveResult::WriteFailed, "ini: a name the file cannot hold fails");
			Check(ReadDisabledList(g_ini, names) && names == std::vector<std::wstring>({ L"B.dll" }), "ini: and the list is as it was");
		}
		else
			printf("SKIPPED: unstorable ini name (the ANSI code page is UTF-8)\n");

		g_ini = g_dir + L"\\missing\\modloader.ini";
		Check(SaveDisabled(g_ini, L"A.dll", true) == SaveResult::WriteFailed, "ini: an unwritable file fails");
	}

	// --- Names and folders ------------------------------------------------------

	void TestNames()
	{
		for (const wchar_t* good : { L"Compass.dll", L"My.Mod v2.DLL", L"Mini_Map-Client.dll", L"M\x00F6d.dll" })
			Check(IsPluginDllName(good), "names: a plain DLL name");
		for (const wchar_t* bad : { L"", L".dll", L"Compass", L"Compass.exe", L"Compass.pdb", L"Compass.dll.bak",
		                            L"..\\Compass.dll", L"../Compass.dll", L"sub\\Compass.dll", L"sub/Compass.dll",
		                            L"C:Compass.dll", L"C:\\Compass.dll", L"\\\\host\\share\\a.dll", L"Compass:stream.dll",
		                            L"Compass.dll:stream", L"Compass.dll::$DATA", L"a..dll", L"..dll", L"a?.dll", L"a*.dll",
		                            L"a<.dll", L"a\".dll", L"a|.dll", L"a\n.dll", L" a.dll", L"a .dll", L"a.dll ", L"a.dll." })
		{
			const bool refused = !IsPluginDllName(bad);
			if (!refused)
				printf("  accepted: %s\n", Utf8(bad).c_str());
			Check(refused, "names: a bad name is refused");
		}
		Check(!IsPluginDllName(std::wstring(MAX_PATH, L'a') + L".dll"), "names: too long");
		Check(!IsPluginDllName(std::wstring(L"a\0b.dll", 7)), "names: an embedded NUL");
		Check(IsFileOf(L"compass.PDB", L"Compass", L".pdb"), "names: the stem's own file, case aside");
		Check(!IsFileOf(L"Other.pdb", L"Compass", L".pdb") && !IsFileOf(L"Compass.json", L"Compass", L".pdb"),
		      "names: wrong stem and wrong extension");
	}

	void TestBadInput()
	{
		Fresh(L"badinput");
		PluginWithNeighbours();
		const std::vector<std::wstring> before = Tree();
		for (const wchar_t* bad : { L"..\\Plugins\\Compass.dll", L"Compass.dll:x", L"C:\\Windows\\System32\\version.dll", L"Compass.ini" })
		{
			Check(Uninstall(g_folders, bad, L"n", L"1", kStamp).result == FileResult::BadName, "input: uninstall refuses the name");
			Check(Restore(g_folders, bad).result == FileResult::BadName, "input: restore refuses the name");
		}
		for (const wchar_t* stamp : { L"", L"..\\..\\x", L"20260101-120000-2", L"20260101_120000", L"20260101-12000a" })
			Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", stamp).result == FileResult::BadName, "input: a bad stamp");
		Check(Uninstall(g_folders, L"Missing.dll", L"n", L"1", kStamp).result == FileResult::NotFound, "input: no such DLL");
		Write(L"Plugins\\Lonely.pdb", "pdb");
		Check(Uninstall(g_folders, L"Lonely.dll", L"n", L"1", kStamp).result == FileResult::NotFound, "input: a pdb without its DLL");
		DeleteFileW((g_folders.plugins + L"\\Lonely.pdb").c_str());
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::NotFound, "input: nothing to restore");
		Check(Tree() == before, "input: nothing changed");

		SYSTEMTIME time{};
		time.wYear = 2026; time.wMonth = 3; time.wDay = 4; time.wHour = 5; time.wMinute = 6; time.wSecond = 7;
		Check(BackupStamp(time) == L"20260304-050607", "stamp: format");
		std::pair<std::wstring, int> order;
		Check(ParseBackupFolder(L"20260304-050607", order) && order.second == 1, "stamp: a first folder");
		Check(ParseBackupFolder(L"20260304-050607-10", order) && order.second == 10, "stamp: a suffix");
		for (const wchar_t* bad : { L"20260304-050607-", L"20260304-050607x2", L"20260304-050607-2a", L"20260304-050607-1000", L"notes" })
			Check(!ParseBackupFolder(bad, order), "stamp: another folder name");
	}

	void TestReparsePoints()
	{
		Fresh(L"links");
		PluginWithNeighbours();
		CreateDirectoryW((g_dir + L"\\Elsewhere").c_str(), nullptr);
		const std::wstring elsewhere = g_dir + L"\\Elsewhere";

		// Removed is a link
		const bool made = MakeJunction(g_folders.removed, elsewhere);
		Check(made && IsReparsePoint(g_folders.removed), "links: junction fixture");
		std::vector<std::wstring> before = Tree();
		Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp).result == FileResult::ReparsePoint, "links: Removed");
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::ReparsePoint, "links: restore through Removed");
		Check(ListBackups(g_folders).empty() && Tree() == before, "links: nothing moved through Removed");
		RemoveDirectoryW(g_folders.removed.c_str());

		// the stem folder is a link
		CreateDirectoryW(g_folders.removed.c_str(), nullptr);
		Check(MakeJunction(g_folders.removed + L"\\Compass", elsewhere), "links: stem fixture");
		before = Tree();
		Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp).result == FileResult::ReparsePoint && Tree() == before, "links: stem folder");
		RemoveDirectoryW((g_folders.removed + L"\\Compass").c_str());

		// the backup folder is a link
		Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp).result == FileResult::Ok, "links: a real backup");
		const std::wstring backup = g_folders.removed + L"\\Compass\\" + kStamp;
		const std::wstring moved = elsewhere + L"\\backup";
		Check(MoveFileW(backup.c_str(), moved.c_str()) && MakeJunction(backup, moved), "links: backup fixture");
		before = Tree();
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::ReparsePoint && Tree() == before, "links: backup folder");

		// Plugins is a link
		Fresh(L"links2");
		CreateDirectoryW((g_dir + L"\\Real").c_str(), nullptr);
		Write(L"Real\\Compass.dll", "dll");
		RemoveDirectoryW(g_folders.plugins.c_str());
		Check(MakeJunction(g_folders.plugins, g_dir + L"\\Real"), "links: Plugins fixture");
		before = Tree();
		Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp).result == FileResult::ReparsePoint, "links: Plugins, uninstall");
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::ReparsePoint && Tree() == before, "links: Plugins, restore");

		// a plugin file that is a link (making one needs a privilege)
		Fresh(L"links3");
		Write(L"target.dll", "outside");
		if (CreateSymbolicLinkW((g_folders.plugins + L"\\Compass.dll").c_str(), (g_dir + L"\\target.dll").c_str(),
		                        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
		{
			before = Tree();
			Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp).result == FileResult::ReparsePoint && Tree() == before, "links: a linked DLL");
		}
		else
			printf("SKIPPED: linked plugin file (cannot make a file link here, error %lu)\n", GetLastError());
	}

	// --- Uninstall and restore --------------------------------------------------

	void TestMovesOnlyThree()
	{
		Fresh(L"three");
		PluginWithNeighbours();
		const Outcome gone = Uninstall(g_folders, L"compass.DLL", L"Comp\x00E4ss \x2713\r\nDll=..\\evil.dll", L"1.2", kStamp);
		const std::wstring backup = L"Removed\\compass\\" + kStamp;
		Check(gone.result == FileResult::Ok && gone.backupDir == g_dir + L"\\" + backup, "three: uninstalled into the stamp folder");
		for (const wchar_t* file : { L"\\Compass.dll", L"\\Compass.pdb", L"\\Compass.json", L"\\removed.ini" })
			Check(Exists(backup + file), "three: in the backup");
		for (const wchar_t* file : { L"Plugins\\Compass.dll", L"Plugins\\Compass.pdb", L"Plugins\\Compass.json" })
			Check(!Exists(file), "three: gone from Plugins");
		for (const wchar_t* file : { L"Plugins\\Compass.ini", L"Plugins\\Compass.dll.bak", L"Plugins\\Compass.json.old",
		                             L"Plugins\\Compass2.dll", L"Plugins\\Compass-data.json", L"Plugins\\config\\Compass.ini",
		                             L"Plugins\\Compass\\save.dat", L"Plugins\\Other.dll" })
			Check(Exists(file), "three: left alone");

		Backup found;
		Check(FindNewestBackup(g_folders, L"Compass.dll", found) == FileResult::Ok, "three: the backup is found");
		Check(found.dllName == L"Compass.dll" && found.files.size() == 3 && found.files[0] == L"Compass.dll", "three: disk spelling, DLL first");
		Check(found.name == L"Comp\x00E4ss \x2713  Dll=..\\evil.dll" && found.version == L"1.2", "three: name kept on one line, as Unicode");
		const std::vector<Backup> all = ListBackups(g_folders);
		// the folder keeps whichever spelling asked first, so the paths match without case
		Check(all.size() == 1 && PluginManage::SameName(all[0].dir, found.dir) && all[0].dllName == L"Compass.dll", "three: listed");

		// a DLL alone, and a folder named like one of the files
		Fresh(L"alone");
		Write(L"Plugins\\Solo.dll", "dll");
		Write(L"Plugins\\Solo.json\\data.txt", "a folder, not the file");
		Check(Uninstall(g_folders, L"Solo.dll", L"", L"", kStamp).result == FileResult::Ok, "alone: uninstalled");
		Check(Exists(L"Plugins\\Solo.json\\data.txt"), "alone: the folder stays");
		Check(Restore(g_folders, L"Solo.dll").result == FileResult::Ok && Exists(L"Plugins\\Solo.dll"), "alone: restored");
	}

	void TestUninstallRollback()
	{
		const char* what[] = { "rollback: uninstall, file 1", "rollback: uninstall, file 2", "rollback: uninstall, file 3" };
		const wchar_t* failed[] = { L"Compass.dll", L"Compass.pdb", L"Compass.json" };
		for (int at = 1; at <= 3; ++at)
		{
			Fresh(L"rollback");
			PluginWithNeighbours();
			CreateDirectoryW(g_folders.removed.c_str(), nullptr);
			const std::vector<std::wstring> before = Tree();
			const Outcome out = Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp, FailingMove({ at }));
			Check(out.result == FileResult::MoveFailed && out.error == ERROR_SHARING_VIOLATION &&
			      out.failedFile == failed[at - 1] && out.backupDir.empty(), what[at - 1]);
			Check(Tree() == before, what[at - 1]);
		}

		// a real failure: another process holds the PDB without delete sharing
		Fresh(L"held");
		PluginWithNeighbours();
		CreateDirectoryW(g_folders.removed.c_str(), nullptr);
		const std::vector<std::wstring> before = Tree();
		const HANDLE held = CreateFileW((g_folders.plugins + L"\\Compass.pdb").c_str(), GENERIC_READ, FILE_SHARE_READ,
		                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		const Outcome out = Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp);
		CloseHandle(held);
		Check(out.result == FileResult::MoveFailed && out.error == ERROR_SHARING_VIOLATION && Tree() == before, "rollback: a held PDB");

		// a failed uninstall leaves an older backup alone
		Check(Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp).result == FileResult::Ok, "rollback: first backup");
		Write(L"Plugins\\Compass.dll", "newer");
		const std::vector<std::wstring> withBackup = Tree();
		Check(Uninstall(g_folders, L"Compass.dll", L"n", L"2", kStamp, FailingMove({ 1 })).result == FileResult::MoveFailed &&
		      Tree() == withBackup, "rollback: the older backup is untouched");
	}

	void TestUninstallRollbackFails()
	{
		// File 3 fails, then putting back file 1 (the DLL) fails: calls 3 and 5.
		Fresh(L"stuck-dll");
		PluginWithNeighbours();
		Outcome out = Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp, FailingMove({ 3, 5 }));
		const std::wstring backup = L"Removed\\Compass\\" + kStamp;
		Check(out.result == FileResult::RollbackFailed && out.failedFile == L"Compass.json" && out.backupDir == g_dir + L"\\" + backup,
		      "stuck: reported, with the backup folder");
		Check(Exists(backup + L"\\Compass.dll") && Exists(backup + L"\\removed.ini") && Exists(L"Plugins\\Compass.pdb") &&
		      Exists(L"Plugins\\Compass.json"), "stuck: nothing is lost");
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::Ok && Exists(L"Plugins\\Compass.dll"), "stuck: restore brings the DLL back");

		// File 3 fails, then putting back file 2 fails: the PDB stays in the backup.
		Fresh(L"stuck-pdb");
		PluginWithNeighbours();
		out = Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp, FailingMove({ 3, 4 }));
		Check(out.result == FileResult::RollbackFailed && Exists(L"Plugins\\Compass.dll") && Exists(backup + L"\\Compass.pdb") &&
		      Exists(backup + L"\\removed.ini"), "stuck: the PDB is kept in the backup");
	}

	void TestRestoreRollback()
	{
		const char* what[] = { "rollback: restore, file 1", "rollback: restore, file 2", "rollback: restore, file 3" };
		const wchar_t* failed[] = { L"Compass.json", L"Compass.pdb", L"Compass.dll" }; // the DLL goes last
		for (int at = 1; at <= 3; ++at)
		{
			Fresh(L"restore-rollback");
			PluginWithNeighbours();
			Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp);
			const std::vector<std::wstring> before = Tree();
			const Outcome out = Restore(g_folders, L"Compass.dll", FailingMove({ at }));
			Check(out.result == FileResult::MoveFailed && out.failedFile == failed[at - 1], what[at - 1]);
			Check(Tree() == before, what[at - 1]);
		}

		// The DLL fails, then putting back the JSON fails: calls 3 and 5.
		Fresh(L"restore-stuck");
		PluginWithNeighbours();
		Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp);
		const Outcome out = Restore(g_folders, L"Compass.dll", FailingMove({ 3, 5 }));
		Check(out.result == FileResult::RollbackFailed && !out.backupDir.empty() && Exists(L"Plugins\\Compass.json") &&
		      !Exists(L"Plugins\\Compass.dll"), "stuck restore: reported, no DLL in Plugins");
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::Ok && Exists(L"Plugins\\Compass.dll") &&
		      Exists(L"Plugins\\Compass.pdb"), "stuck restore: a second restore finishes");
	}

	void TestRestoreCollision()
	{
		for (const wchar_t* taken : { L"Plugins\\Compass.dll", L"Plugins\\compass.PDB", L"Plugins\\Compass.json",
		                              L"Plugins\\Compass.json\\x" })
		{
			Fresh(L"collision");
			PluginWithNeighbours();
			Uninstall(g_folders, L"Compass.dll", L"n", L"1", kStamp);
			Write(taken, "a newer copy");
			const std::vector<std::wstring> before = Tree();
			Check(Restore(g_folders, L"Compass.dll").result == FileResult::AlreadyInstalled && Tree() == before,
			      "collision: refused with nothing moved");
		}
		Check(MoveOnSameVolume(g_folders.plugins + L"\\Compass.ini", g_folders.plugins + L"\\Other.dll") != ERROR_SUCCESS &&
		      Exists(L"Plugins\\Compass.ini"), "collision: the move itself never overwrites");
	}

	void TestBackups()
	{
		Fresh(L"backups");
		const auto install = [](const char* content, const std::wstring& version, const std::wstring& stamp)
		{
			Write(L"Plugins\\Compass.dll", content);
			return Uninstall(g_folders, L"Compass.dll", L"Compass", version, stamp);
		};
		Backup found;
		install("1", L"1", L"20260101-120000");
		install("22", L"2", L"20260102-120000");
		Check(FindNewestBackup(g_folders, L"Compass.dll", found) == FileResult::Ok && found.version == L"2", "backups: the newer stamp");
		for (int i = 3; i <= 11; ++i) // the same second: -2 ... -10
			install("333", std::to_wstring(i), L"20260102-120000");
		Check(FindNewestBackup(g_folders, L"Compass.dll", found) == FileResult::Ok && found.version == L"11" &&
		      found.dir.ends_with(L"20260102-120000-10"), "backups: -10 is newer than -9");
		Write(L"Removed\\Compass\\20990101-000000\\removed.ini", "no DLL here");
		Write(L"Removed\\Compass\\notes\\Compass.dll", "not a backup");
		Check(FindNewestBackup(g_folders, L"Compass.dll", found) == FileResult::Ok && found.version == L"11", "backups: other folders are skipped");
		Check(Restore(g_folders, L"Compass.dll").result == FileResult::Ok, "backups: the newest is restored");
		Check(Exists(L"Removed\\Compass\\20260102-120000-10\\removed.ini") && Exists(L"Removed\\Compass\\20260101-120000\\Compass.dll"),
		      "backups: none is deleted");
		Check(FindNewestBackup(g_folders, L"Compass.dll", found) == FileResult::Ok && found.version == L"10", "backups: then the one before");
	}

	void TestTamperedRecord()
	{
		Fresh(L"record");
		Write(L"Plugins\\Other.dll", "other");
		const std::wstring backup = L"Removed\\Compass\\" + kStamp;
		Write(backup + L"\\Compass.dll", "dll");
		Write(L"evil.dll", "outside");
		Backup found;
		Check(FindNewestBackup(g_folders, L"Compass.dll", found) == FileResult::BadRecord, "record: missing");

		const char* bad[] = {
			"Dll=..\\..\\..\\evil.dll", "Dll=C:\\Windows\\evil.dll", "Dll=Other.dll", "Dll=Compass.dll:stream",
			"Dll=Compass.exe", "Pdb=Compass.pdb",
			"Dll=Compass.dll\r\nPdb=..\\..\\..\\evil.dll", "Dll=Compass.dll\r\nPdb=Compass.json",
			"Dll=Compass.dll\r\nJson=Other.json", "Dll=Compass.dll\r\nJson=sub\\Compass.json",
		};
		for (const char* line : bad)
		{
			Write(backup + L"\\removed.ini", std::string("[Removed]\r\n") + line + "\r\n");
			const std::vector<std::wstring> before = Tree();
			const bool refused = Restore(g_folders, L"Compass.dll").result == FileResult::BadRecord && Tree() == before;
			if (!refused)
				printf("  record: %s\n", line);
			Check(refused, "record: a bad name is refused and nothing moves");
		}
		Check(ListBackups(g_folders).empty(), "record: a bad backup is not listed");
	}

	void TestLoadedDll()
	{
		Fresh(L"loaded");
		wchar_t system[MAX_PATH]{};
		GetSystemDirectoryW(system, MAX_PATH);
		const std::wstring path = g_folders.plugins + L"\\PluginManageLoaded.dll";
		Check(CopyFileW((std::wstring(system) + L"\\version.dll").c_str(), path.c_str(), TRUE) != 0, "loaded: fixture DLL");
		const HMODULE module = LoadLibraryW(path.c_str());
		Check(module != nullptr, "loaded: the copy loads");
		if (!module)
			return;

		const Outcome gone = Uninstall(g_folders, L"PluginManageLoaded.dll", L"n", L"1", kStamp);
		Check(gone.result == FileResult::Ok && !Exists(L"Plugins\\PluginManageLoaded.dll"), "loaded: a loaded DLL is moved to another folder");
		const auto size = reinterpret_cast<DWORD(WINAPI*)(LPCWSTR, LPDWORD)>(GetProcAddress(module, "GetFileVersionInfoSizeW"));
		DWORD ignored = 0;
		Check(GetModuleHandleW(L"PluginManageLoaded.dll") == module && size && size((std::wstring(system) + L"\\version.dll").c_str(), &ignored) > 0,
		      "loaded: and its code still runs");
		Check(Restore(g_folders, L"PluginManageLoaded.dll").result == FileResult::Ok && Exists(L"Plugins\\PluginManageLoaded.dll"),
		      "loaded: and moved back while loaded");
		FreeLibrary(module);
		printf("loaded DLL: moved out of Plugins and back while mapped\n");
	}

	// --- Preload names and cards ------------------------------------------------

	void TestPreloadNames()
	{
		std::vector<std::string> names;
		AddPreloadNames("StarRupture-Preload-IniOverrides-Client.dll", "IniOverrides", names);
		Check(names == std::vector<std::string>({ "IniOverrides", "StarRupture-Preload-IniOverrides-Client", "StarRupture-Preload-IniOverrides" }),
		      "preload: reported name, stem, and the stem without -Client");
		names.clear();
		AddPreloadNames("Fix-SERVER.dll", "", names);
		Check(names == std::vector<std::string>({ "Fix-SERVER", "Fix" }), "preload: -Server in any case, no reported name");
		names.clear();
		AddPreloadNames("-Client.dll", "", names);
		Check(names == std::vector<std::string>({ "-Client" }), "preload: a bare suffix is not cut to nothing");

		ModIndex::Entry entry;
		entry.id = "fix";
		entry.name = "The Fix";
		names.clear();
		AddPreloadNames("Fix-Client.dll", "", names);
		Check(ModIndex::IsInstalled(entry, names), "preload: an index entry matches");
	}

	void TestCards()
	{
		const auto card = [](CardFacts facts, CardState state, unsigned buttons, const char* what)
		{
			const Card c = DeriveCard(facts);
			Check(c.state == state && c.buttons == buttons && !c.disableGreyed && !c.uninstallInVortex, what);
		};
		CardFacts f;
		card(f, CardState::NotInstalled, kOpenPage | kReadme, "card: not installed");
		f = {}; f.inPlugins = f.running = true;
		card(f, CardState::Running, kDisable | kUninstall, "card: running");
		f.running = false;
		card(f, CardState::NotRunning, kDisable | kUninstall, "card: installed, not running");
		f.enableWaitsForRestart = true;
		card(f, CardState::EnablePending, kDisable, "card: enable pending");
		f = {}; f.inPlugins = f.listedDisabled = true;
		card(f, CardState::Disabled, kEnable | kUninstall, "card: disabled");
		f.running = true;
		card(f, CardState::DisablePending, kEnable | kUninstall, "card: disable pending");
		f = {}; f.hasBackup = true;
		card(f, CardState::Uninstalled, kRestore | kOpenPage, "card: uninstalled");
		f.running = true;
		card(f, CardState::UninstalledRunning, kRestore, "card: gone after a restart");
		f.hasBackup = false;
		card(f, CardState::UninstalledRunning, kNoButtons, "card: running with no file and no backup");
		f = {}; f.asPreload = true;
		card(f, CardState::Preload, kNoButtons, "card: preload");
		f.hasBackup = true;
		card(f, CardState::Preload, kNoButtons, "card: preload wins over a backup");
		f.working = f.inPlugins = true;
		card(f, CardState::Working, kNoButtons, "card: working");

		f = {}; f.inPlugins = f.running = f.vortexListed = true;
		Card c = DeriveCard(f);
		Check(c.buttons == kDisable && c.uninstallInVortex, "card: Vortex files cannot be uninstalled here");
		f = {}; f.inPlugins = f.running = true; f.canBeDisabled = false;
		c = DeriveCard(f);
		Check(c.buttons == (kDisable | kUninstall) && c.disableGreyed, "card: Disable greyed for a name the list cannot hold");
		f = {}; f.vortexListed = true;
		Check(!DeriveCard(f).uninstallInVortex, "card: no Vortex line without an Uninstall button");
	}

	void TestVortexListing()
	{
		using VortexNotice::Listing;
		Fresh(L"vortex");
		const std::vector<std::string> paths = { "ModLoader\\Plugins\\Compass.dll", "ModLoader\\Plugins\\Compass.pdb" };
		const auto listing = [&paths] { return VortexNotice::ReadDeployment(g_dir).Lists(paths); };
		const auto manifest = [](const std::string& relPath)
		{
			return "{\"files\":[{\"relPath\":\"" + relPath + "\",\"time\":1}]}\n";
		};
		Check(listing() == Listing::NotListed, "vortex: no manifest");
		Write(L"vortex.deployment.json", manifest("dwmapi.dll"));
		Check(listing() == Listing::NotListed, "vortex: not listed");
		Write(L"vortex.deployment.json", manifest("modloader/plugins/COMPASS.PDB"));
		Check(listing() == Listing::Listed, "vortex: listed, any case and slash, no file time needed");
		Write(L"vortex.deployment.json",
		      "{\"files\":[{\"relPath\":\"Compass.dll\",\"target\":\"ModLoader/Plugins\",\"time\":1}]}\n");
		Check(listing() == Listing::Listed, "vortex: listed under the entry's target");
		Write(L"vortex.deployment.json",
		      "{\"files\":[{\"relPath\":\"ModLoader/Plugins/Compass.dll\",\"target\":\"Other\"}]}\n");
		Check(listing() == Listing::NotListed, "vortex: another target is another file");
		Write(L"vortex.deployment.json", manifest("ModLoader/Plugins/Compass.dll"));
		Write(L"vortex.deployment.a.json", "{ cut");
		Check(listing() == Listing::Listed, "vortex: listed beside a bad manifest");
		Write(L"vortex.deployment.json", manifest("dwmapi.dll"));
		Check(listing() == Listing::Unsure, "vortex: a bad manifest is unsure, not listed");
		Check(VortexNotice::ReadDeployment(g_dir + L"\\missing").Lists(paths) == Listing::Unsure, "vortex: an unreadable folder is unsure");
	}

	// --- The scripted run -------------------------------------------------------

	void Show(const char* step)
	{
		printf("\n== %s\n", step);
		std::vector<std::wstring> names;
		ReadDisabledList(g_ini, names);
		std::string list;
		for (const std::wstring& name : names)
			list += (list.empty() ? "" : ",") + Utf8(name);
		printf("[Plugins] Disabled=%s\n", list.c_str());
		for (const std::wstring& entry : Tree())
		{
			// "name:size:hash" for a file, "name\" for a folder
			printf("  %s\n", Utf8(entry).c_str());
		}
	}

	void ScriptedRun()
	{
		Fresh(L"ModLoader");
		Write(L"modloader.ini", "[Plugins]\r\nDisabled=\r\n");
		Write(L"Plugins\\MiniMap.dll", "minimap code");
		Write(L"Plugins\\MiniMap.pdb", "minimap symbols");
		Write(L"Plugins\\MiniMap.json", "{\"version\":\"1.0\"}");
		Write(L"Plugins\\config\\MiniMap.ini", "[General]\r\nEnabled=1\r\n");
		Write(L"Plugins\\MiniMap\\waypoints.dat", "player data");
		Write(L"Plugins\\Compass.dll", "compass code");
		printf("\nScripted run (name:bytes:hash is a file, name\\ a folder)\n");
		Show("Start");
		const std::vector<std::wstring> start = Tree();
		std::vector<std::wstring> names;

		Check(SaveDisabled(g_ini, L"MiniMap.dll", true) == SaveResult::Ok, "run: disable");
		Show("Disable MiniMap.dll");
		Check(ReadDisabledList(g_ini, names) && ListHas(names, L"minimap.dll"), "run: it is on the list");

		Check(SaveDisabled(g_ini, L"MiniMap.dll", false) == SaveResult::Ok, "run: enable");
		Show("Enable MiniMap.dll");
		Check(ReadDisabledList(g_ini, names) && names.empty(), "run: the list is empty again");

		const Outcome gone = Uninstall(g_folders, L"MiniMap.dll", L"MiniMap", L"1.0", kStamp);
		Check(gone.result == FileResult::Ok, "run: uninstall");
		Show("Uninstall MiniMap.dll");
		Check(!Exists(L"Plugins\\MiniMap.dll") && Exists(L"Plugins\\config\\MiniMap.ini") &&
		      Exists(L"Plugins\\MiniMap\\waypoints.dat") && Exists(L"Plugins\\Compass.dll"), "run: settings, data and other plugins stay");

		Check(Restore(g_folders, L"MiniMap.dll").result == FileResult::Ok, "run: restore");
		Show("Restore MiniMap.dll");
		std::vector<std::wstring> after = Tree();
		after.erase(std::remove_if(after.begin(), after.end(),
		                           [](const std::wstring& e) { return e.rfind(L"Removed\\", 0) == 0; }), after.end());
		std::vector<std::wstring> startFiles = start;
		const auto notIni = [](const std::wstring& e) { return e.rfind(L"modloader.ini", 0) == 0; };
		after.erase(std::remove_if(after.begin(), after.end(), notIni), after.end());
		startFiles.erase(std::remove_if(startFiles.begin(), startFiles.end(), notIni), startFiles.end());
		Check(after == startFiles, "run: Plugins is as it started");
		printf("\n");
	}

	void TestIdentity()
	{
		const std::wstring ini = g_root + L"\\identity.ini";
		Check(ReadIdentity(ini, L"A.dll").name.empty(), "identity: nothing cached");
		Check(SaveIdentity(ini, L"A.dll", { L"Café 日", L"1.2", L"me\nName=x" }), "identity: saved");
		const Identity who = ReadIdentity(ini, L"a.dll");
		Check(who.name == L"Café 日" && who.version == L"1.2", "identity: Unicode round trip, name case aside");
		Check(who.author == L"me Name=x", "identity: a line break cannot start a key");
		Check(!SaveIdentity(ini, L"B[1].dll", { L"B", L"1", L"" }), "identity: brackets refused");
		Check(!SaveIdentity(ini, L"C.dll", {}), "identity: no name, nothing saved");
		DeleteFileW(ini.c_str());
	}
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::string(argv[1]) == "--manager")
		return RunManagerStart(argv[2]);

	wchar_t temp[MAX_PATH]{};
	GetTempPathW(MAX_PATH, temp);
	wchar_t full[MAX_PATH]{};
	GetLongPathNameW(temp, full, MAX_PATH); // a junction target needs the long form
	g_root = std::wstring(full) + L"plugin_manage_tests_" + std::to_wstring(GetCurrentProcessId());
	CreateDirectoryW(g_root.c_str(), nullptr);

	TestSplit();
	TestEditList();
	TestIni();
	TestNames();
	TestBadInput();
	TestReparsePoints();
	TestMovesOnlyThree();
	TestUninstallRollback();
	TestUninstallRollbackFails();
	TestRestoreRollback();
	TestRestoreCollision();
	TestBackups();
	TestTamperedRecord();
	TestLoadedDll();
	TestPreloadNames();
	TestCards();
	TestIdentity();
	TestVortexListing();
	ScriptedRun();
	RunManagerTests(g_root, g_checks, g_failed);

	Wipe(g_root);

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
