#pragma once

// ---------------------------------------------------------------------------
// plugin_manage.h -- the rules behind disabling and uninstalling a plugin: the
// disabled list, which names and folders may be touched, moving a plugin's
// files to ModLoader\Removed and back, and what a mod's card shows. Header
// only, with no loader state, so a test can use it without the loader.
//
// A plugin's files are exactly X.dll, X.pdb and X.json in ModLoader\Plugins.
// Folders and config ini files hold settings and save data and are never
// touched. A file name given to these functions must come from the loader's own
// directory listing or plugin record, never from the mod index or from text a
// plugin supplied.
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace PluginManage
{
	// --- Name lists -------------------------------------------------------------

	// A comma-separated ini value as its items, spaces and tabs trimmed (a
	// hand-edited ini will have spaces after the commas), empty items dropped.
	// [Preload] Disabled and [Plugins] Disabled are both read with it.
	template <class Str>
	std::vector<Str> SplitNameList(const Str& list)
	{
		using Char = typename Str::value_type;
		const auto blank = [](Char c) { return c == Char(' ') || c == Char('\t'); };

		std::vector<Str> out;
		size_t start = 0;
		while (start < list.size())
		{
			const size_t comma = list.find(Char(','), start);
			Str item = list.substr(start, comma == Str::npos ? Str::npos : comma - start);

			while (!item.empty() && blank(item.front())) item.erase(item.begin());
			while (!item.empty() && blank(item.back()))  item.pop_back();

			if (!item.empty())
				out.push_back(item);

			if (comma == Str::npos)
				break;
			start = comma + 1;
		}
		return out;
	}

	// File names compare as Windows compares them.
	inline bool SameName(const std::wstring& a, const std::wstring& b)
	{
		return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
		                            b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
	}

	inline bool ListHas(const std::vector<std::wstring>& list, const std::wstring& name)
	{
		for (const std::wstring& item : list)
			if (SameName(item, name))
				return true;
		return false;
	}

	// --- Names ------------------------------------------------------------------

	// A name that can only mean a file in the folder it is joined to: no path,
	// drive or stream separator, no "..", no wildcard or control character.
	inline bool IsBareFileName(const std::wstring& name)
	{
		if (name.empty() || name.size() >= MAX_PATH || name.find(L"..") != std::wstring::npos)
			return false;
		for (wchar_t c : name)
			if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c))
				return false;
		// Windows drops a trailing space or dot, so such a name means another file.
		return name.front() != L' ' && name.back() != L' ' && name.back() != L'.';
	}

	// "X.dll" -> "X".
	inline std::wstring Stem(const std::wstring& fileName)
	{
		return fileName.substr(0, fileName.rfind(L'.'));
	}

	// Whether `name` is exactly the file `stem` + `extension` (".dll", ".pdb" or
	// ".json"), case aside.
	inline bool IsFileOf(const std::wstring& name, const std::wstring& stem, const wchar_t* extension)
	{
		return IsBareFileName(name) && SameName(name, stem + extension);
	}

	inline bool IsPluginDllName(const std::wstring& name)
	{
		if (!IsBareFileName(name) || name.size() <= 4)
			return false;
		const std::wstring stem = Stem(name);
		// The stem names a folder under Removed, so it follows the same rule.
		return IsBareFileName(stem) && IsFileOf(name, stem, L".dll");
	}

	// --- The disabled list: [Plugins] Disabled in modloader.ini -----------------

	constexpr const wchar_t* kDisabledSection = L"Plugins";
	constexpr const wchar_t* kDisabledKey     = L"Disabled";
	constexpr DWORD kDisabledBufferChars = 8192;
	// A value of kDisabledBufferChars - 1 reads the same as a cut one.
	constexpr size_t kMaxDisabledValueChars = kDisabledBufferChars - 2;

	enum class ListEdit { Ok, BadName, TooLong };

	// A comma splits the list, so such a name cannot be put on it.
	inline bool CanBeDisabled(const std::wstring& dllName)
	{
		return IsPluginDllName(dllName) && dllName.find(L',') == std::wstring::npos;
	}

	// The list value `current` with `dllName` on it or off it. The other items
	// are kept. `updated` is set only when the result is Ok.
	inline ListEdit EditDisabledList(const std::wstring& current, const std::wstring& dllName,
	                                 bool disabled, std::wstring& updated)
	{
		if (!CanBeDisabled(dllName))
			return ListEdit::BadName;
		if (current.size() > kMaxDisabledValueChars)
			return ListEdit::TooLong; // read cut off: writing it back would lose names

		std::wstring value;
		const auto add = [&value](const std::wstring& item)
		{
			if (!value.empty())
				value += L',';
			value += item;
		};
		for (const std::wstring& item : SplitNameList(current))
			if (!SameName(item, dllName))
				add(item);
		if (disabled)
			add(dllName);

		if (value.size() > kMaxDisabledValueChars)
			return ListEdit::TooLong;
		updated = std::move(value);
		return ListEdit::Ok;
	}

	namespace detail
	{
		inline std::wstring ReadIni(const std::wstring& ini, const wchar_t* section, const wchar_t* key)
		{
			std::vector<wchar_t> buffer(kDisabledBufferChars);
			const DWORD length = GetPrivateProfileStringW(section, key, L"", buffer.data(),
			                                              kDisabledBufferChars, ini.c_str());
			return std::wstring(buffer.data(), length);
		}
	}

	// The names on the list. False when the value is too long to read whole; the
	// names that were read are still returned.
	inline bool ReadDisabledList(const std::wstring& ini, std::vector<std::wstring>& names)
	{
		const std::wstring value = detail::ReadIni(ini, kDisabledSection, kDisabledKey);
		names = SplitNameList(value);
		return value.size() <= kMaxDisabledValueChars;
	}

	enum class SaveResult { Ok, BadName, TooLong, WriteFailed };

	// Put `dllName` on the list or take it off. Anything but Ok leaves the list
	// as it was. The value is read back, so a name the ini's code page cannot
	// hold counts as a failed write.
	inline SaveResult SaveDisabled(const std::wstring& ini, const std::wstring& dllName, bool disabled)
	{
		const std::wstring current = detail::ReadIni(ini, kDisabledSection, kDisabledKey);
		std::wstring updated;
		switch (EditDisabledList(current, dllName, disabled, updated))
		{
		case ListEdit::BadName: return SaveResult::BadName;
		case ListEdit::TooLong: return SaveResult::TooLong;
		case ListEdit::Ok: break;
		}

		if (!WritePrivateProfileStringW(kDisabledSection, kDisabledKey, updated.c_str(), ini.c_str()))
			return SaveResult::WriteFailed;
		if (detail::ReadIni(ini, kDisabledSection, kDisabledKey) != updated)
		{
			WritePrivateProfileStringW(kDisabledSection, kDisabledKey, current.c_str(), ini.c_str());
			return SaveResult::WriteFailed;
		}
		return SaveResult::Ok;
	}

	// --- Uninstall and restore --------------------------------------------------

	struct Folders
	{
		std::wstring plugins; // ...\ModLoader\Plugins
		std::wstring removed; // ...\ModLoader\Removed (same volume)
	};

	// Moves one file; returns ERROR_SUCCESS or the Windows error. A test passes
	// its own to make a chosen move fail.
	using MoveFn = std::function<DWORD(const std::wstring& from, const std::wstring& to)>;

	// A rename only: it never overwrites and never falls back to copy and delete.
	// Windows allows it for a DLL that is still loaded.
	inline DWORD MoveOnSameVolume(const std::wstring& from, const std::wstring& to)
	{
		return MoveFileExW(from.c_str(), to.c_str(), 0) ? ERROR_SUCCESS : GetLastError();
	}

	enum class FileResult
	{
		Ok,
		BadName,          // not a bare plugin DLL name
		ReparsePoint,     // a link where a plain file or folder belongs
		NotFound,         // no DLL to uninstall, or no backup to restore
		AlreadyInstalled, // restore: a file of that name is in Plugins
		BadRecord,        // restore: removed.ini names something it must not
		SetupFailed,      // the backup folder or its record could not be made
		MoveFailed,       // a move failed and everything was put back
		RollbackFailed,   // a move failed and so did putting back: files are in both folders
	};

	struct Outcome
	{
		FileResult result = FileResult::Ok;
		std::wstring backupDir;  // the backup used; empty when nothing was left in one
		std::wstring failedFile; // the file the failed or refused step was about
		DWORD error = ERROR_SUCCESS;
	};

	struct Backup
	{
		std::wstring dir;
		std::wstring dllName;            // as it was in Plugins
		std::wstring name;               // the plugin's own name and version, when known
		std::wstring version;
		bool disabled = false;           // it was disabled when it was uninstalled
		std::vector<std::wstring> files; // those in the backup, the DLL first
	};

	constexpr const wchar_t* kRecordFile    = L"removed.ini";
	constexpr const wchar_t* kRecordSection = L"Removed";

	// The three files of a plugin, the DLL first, and their keys in removed.ini.
	constexpr const wchar_t* kExtensions[] = { L".dll", L".pdb", L".json" };
	constexpr const wchar_t* kRecordKeys[] = { L"Dll", L"Pdb", L"Json" };

	inline bool IsReparsePoint(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
	}

	// The name of a backup folder for a time: yyyyMMdd-HHmmss.
	inline std::wstring BackupStamp(const SYSTEMTIME& time)
	{
		wchar_t text[32]{};
		swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay,
		           time.wHour, time.wMinute, time.wSecond);
		return text;
	}

	// A backup folder's name: a stamp, then "-2", "-3" ... for a second one in
	// the same second. `order` sorts newer after older. False for other names.
	inline bool ParseBackupFolder(const std::wstring& folder, std::pair<std::wstring, int>& order)
	{
		constexpr size_t kStampLen = 15;
		if (folder.size() < kStampLen)
			return false;
		for (size_t i = 0; i < kStampLen; ++i)
			if (i == 8 ? folder[i] != L'-' : !iswdigit(folder[i]))
				return false;

		int sequence = 1;
		if (folder.size() > kStampLen)
		{
			if (folder[kStampLen] != L'-' || folder.size() == kStampLen + 1 || folder.size() > kStampLen + 4)
				return false;
			sequence = 0;
			for (size_t i = kStampLen + 1; i < folder.size(); ++i)
			{
				if (!iswdigit(folder[i]))
					return false;
				sequence = sequence * 10 + (folder[i] - L'0');
			}
		}
		order = { folder.substr(0, kStampLen), sequence };
		return true;
	}

	namespace detail
	{
		// The name of the file at dir\name as it is spelled on disk, or empty
		// when no file (a folder does not count) has exactly that name.
		inline std::wstring FileOnDisk(const std::wstring& dir, const std::wstring& name)
		{
			WIN32_FIND_DATAW found{};
			const HANDLE search = FindFirstFileW((dir + L"\\" + name).c_str(), &found);
			if (search == INVALID_HANDLE_VALUE)
				return {};
			FindClose(search);
			// The search also matches short 8.3 names.
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !SameName(found.cFileName, name))
				return {};
			return found.cFileName;
		}

		inline std::vector<std::wstring> SubFolders(const std::wstring& dir)
		{
			std::vector<std::wstring> out;
			WIN32_FIND_DATAW found{};
			const HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
			if (search == INVALID_HANDLE_VALUE)
				return out;
			do
			{
				if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				    wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0)
					out.push_back(found.cFileName);
			} while (FindNextFileW(search, &found));
			FindClose(search);
			return out;
		}

		inline bool MakeFolder(const std::wstring& dir)
		{
			if (CreateDirectoryW(dir.c_str(), nullptr))
				return true;
			const DWORD attributes = GetFileAttributesW(dir.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
		}

		// Plugin text goes into the record, where a line break would start a new key.
		inline std::wstring OneLine(std::wstring text)
		{
			for (wchar_t& c : text)
				if (c < 0x20)
					c = L' ';
			return text;
		}

		inline bool WriteRecord(const std::wstring& dir, const std::vector<std::wstring>& files,
		                        const std::wstring& name, const std::wstring& version, bool disabled)
		{
			const std::wstring path = dir + L"\\" + kRecordFile;

			// The profile functions write Unicode only into a file that already
			// starts with a UTF-16 byte order mark.
			const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			                                FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			const wchar_t mark = 0xFEFF;
			DWORD written = 0;
			const bool marked = WriteFile(file, &mark, sizeof(mark), &written, nullptr) != 0;
			CloseHandle(file);
			if (!marked)
				return false;

			const auto put = [&path](const wchar_t* key, const std::wstring& value)
			{
				return WritePrivateProfileStringW(kRecordSection, key, value.c_str(), path.c_str()) != 0;
			};
			bool ok = put(L"Name", OneLine(name)) && put(L"Version", OneLine(version)) &&
				put(L"Disabled", disabled ? L"1" : L"0");
			for (const std::wstring& entry : files)
				for (size_t i = 0; ok && i < 3; ++i)
					if (SameName(entry, Stem(entry) + kExtensions[i]))
						ok = put(kRecordKeys[i], entry);
			return ok;
		}

		// Move `files` from one folder to the other in order. When one fails, the
		// ones already moved go back, last first.
		inline Outcome MoveAll(const std::wstring& from, const std::wstring& to,
		                       const std::vector<std::wstring>& files, const MoveFn& move)
		{
			Outcome out;
			for (size_t i = 0; i < files.size(); ++i)
			{
				const DWORD error = move(from + L"\\" + files[i], to + L"\\" + files[i]);
				if (error == ERROR_SUCCESS)
					continue;

				out.result = FileResult::MoveFailed;
				out.failedFile = files[i];
				out.error = error;
				while (i-- > 0)
					if (move(to + L"\\" + files[i], from + L"\\" + files[i]) != ERROR_SUCCESS)
						out.result = FileResult::RollbackFailed;
				break;
			}
			return out;
		}
	}

	// Move the plugin's X.dll, X.pdb and X.json (those that exist) to a new
	// folder Removed\X\<stamp>, the DLL first, with a removed.ini beside them.
	// Nothing else is touched. `name` and `version` are kept for the card, and
	// `disabled` so that a restore can leave the plugin as it was.
	inline Outcome Uninstall(const Folders& folders, const std::wstring& dllName,
	                         const std::wstring& name, const std::wstring& version,
	                         const std::wstring& stamp, const MoveFn& move = MoveOnSameVolume,
	                         bool disabled = false)
	{
		Outcome out;
		const auto refuse = [&out](FileResult result, const std::wstring& what)
		{
			out.result = result;
			out.failedFile = what;
			return out;
		};

		// A bare stamp only: a second backup in the same second gets its suffix here.
		std::pair<std::wstring, int> order;
		if (!IsPluginDllName(dllName) || !ParseBackupFolder(stamp, order) || order.first != stamp)
			return refuse(FileResult::BadName, dllName);

		const std::wstring stem = Stem(dllName);
		const std::wstring stemDir = folders.removed + L"\\" + stem;
		for (const std::wstring& dir : { folders.plugins, folders.removed, stemDir })
			if (IsReparsePoint(dir))
				return refuse(FileResult::ReparsePoint, dir);

		std::vector<std::wstring> files;
		for (const wchar_t* extension : kExtensions)
		{
			const std::wstring found = detail::FileOnDisk(folders.plugins, stem + extension);
			if (found.empty())
				continue;
			if (IsReparsePoint(folders.plugins + L"\\" + found))
				return refuse(FileResult::ReparsePoint, found);
			files.push_back(found);
		}
		if (files.empty() || !SameName(files.front(), dllName))
			return refuse(FileResult::NotFound, dllName);

		if (!detail::MakeFolder(folders.removed) || !detail::MakeFolder(stemDir))
		{
			out.error = GetLastError();
			return refuse(FileResult::SetupFailed, stemDir);
		}
		std::wstring backupDir = stemDir + L"\\" + stamp;
		for (int sequence = 2; !CreateDirectoryW(backupDir.c_str(), nullptr); ++sequence)
		{
			out.error = GetLastError();
			if (out.error != ERROR_ALREADY_EXISTS || sequence > 999)
				return refuse(FileResult::SetupFailed, backupDir);
			backupDir = stemDir + L"\\" + stamp + L"-" + std::to_wstring(sequence);
		}
		out.error = ERROR_SUCCESS;

		// For a folder this call made and put no file in: not a backup yet.
		const auto discard = [&]()
		{
			DeleteFileW((backupDir + L"\\" + kRecordFile).c_str());
			RemoveDirectoryW(backupDir.c_str()); // only removes an empty folder
			RemoveDirectoryW(stemDir.c_str());
		};

		// The record goes first, so a process that dies during the moves leaves
		// a backup that can be restored.
		if (!detail::WriteRecord(backupDir, files, name, version, disabled))
		{
			out.error = GetLastError();
			discard();
			return refuse(FileResult::SetupFailed, backupDir);
		}

		out = detail::MoveAll(folders.plugins, backupDir, files, move);
		if (out.result == FileResult::MoveFailed)
			discard();
		else
			out.backupDir = backupDir;
		return out;
	}

	// The newest backup of a plugin: the latest folder under Removed\X that
	// holds X.dll. Returns Ok, NotFound, BadName, ReparsePoint or BadRecord.
	// Every name in removed.ini is checked again here, since the file is on disk
	// where anything can have changed it.
	inline FileResult FindNewestBackup(const Folders& folders, const std::wstring& dllName, Backup& backup)
	{
		if (!IsPluginDllName(dllName))
			return FileResult::BadName;
		const std::wstring stem = Stem(dllName);
		const std::wstring stemDir = folders.removed + L"\\" + stem;
		if (IsReparsePoint(folders.removed) || IsReparsePoint(stemDir))
			return FileResult::ReparsePoint;

		std::wstring newest;
		std::pair<std::wstring, int> newestOrder, order;
		for (const std::wstring& folder : detail::SubFolders(stemDir))
		{
			if (!ParseBackupFolder(folder, order) || (!newest.empty() && order <= newestOrder))
				continue;
			// A restored or abandoned folder has no DLL left in it.
			if (detail::FileOnDisk(stemDir + L"\\" + folder, stem + L".dll").empty())
				continue;
			newest = folder;
			newestOrder = order;
		}
		if (newest.empty())
			return FileResult::NotFound;

		Backup found;
		found.dir = stemDir + L"\\" + newest;
		if (IsReparsePoint(found.dir))
			return FileResult::ReparsePoint;

		const std::wstring record = found.dir + L"\\" + kRecordFile;
		for (size_t i = 0; i < 3; ++i)
		{
			const std::wstring name = detail::ReadIni(record, kRecordSection, kRecordKeys[i]);
			if (name.empty())
			{
				if (i == 0)
					return FileResult::BadRecord;
				continue;
			}
			if (!IsFileOf(name, stem, kExtensions[i]))
				return FileResult::BadRecord;
			if (i == 0)
				found.dllName = name;
			// Not there: putting it back failed to move it out of Plugins.
			if (!detail::FileOnDisk(found.dir, name).empty())
				found.files.push_back(name);
		}
		found.name = detail::ReadIni(record, kRecordSection, L"Name");
		found.version = detail::ReadIni(record, kRecordSection, L"Version");
		found.disabled = detail::ReadIni(record, kRecordSection, L"Disabled") == L"1";
		backup = std::move(found);
		return FileResult::Ok;
	}

	// The newest backup of every plugin that has one that can be restored.
	inline std::vector<Backup> ListBackups(const Folders& folders)
	{
		std::vector<Backup> out;
		if (IsReparsePoint(folders.removed))
			return out;
		for (const std::wstring& stem : detail::SubFolders(folders.removed))
		{
			Backup backup;
			if (FindNewestBackup(folders, stem + L".dll", backup) == FileResult::Ok)
				out.push_back(std::move(backup));
		}
		return out;
	}

	// Move the newest backup's files back to Plugins, the DLL last, so a process
	// that dies during the moves leaves nothing to load and the backup findable.
	// Refused, with nothing moved, when any of those files is already there.
	inline Outcome Restore(const Folders& folders, const std::wstring& dllName,
	                       const MoveFn& move = MoveOnSameVolume)
	{
		Outcome out;
		const auto refuse = [&out](FileResult result, const std::wstring& what)
		{
			out.result = result;
			out.failedFile = what;
			return out;
		};

		if (IsReparsePoint(folders.plugins))
			return refuse(FileResult::ReparsePoint, folders.plugins);
		Backup backup;
		const FileResult found = FindNewestBackup(folders, dllName, backup);
		if (found != FileResult::Ok)
			return refuse(found, dllName);

		for (const std::wstring& file : backup.files)
		{
			if (IsReparsePoint(backup.dir + L"\\" + file))
				return refuse(FileResult::ReparsePoint, file);
			if (GetFileAttributesW((folders.plugins + L"\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES)
				return refuse(FileResult::AlreadyInstalled, file);
		}

		const std::vector<std::wstring> dllLast(backup.files.rbegin(), backup.files.rend());
		out = detail::MoveAll(backup.dir, folders.plugins, dllLast, move);
		out.backupDir = backup.dir;
		return out;
	}

	// --- Identity cache: ModLoader\plugin_manage.ini ----------------------------

	// What a plugin called itself, kept by file name so a disabled plugin still
	// has a name: nothing of it is loaded to ask.
	constexpr const wchar_t* kIdentityFile = L"plugin_manage.ini";

	struct Identity
	{
		std::wstring name, version, author;
	};

	// False when nothing was written: no name, a file name that cannot head an
	// ini section, or a failed write.
	inline bool SaveIdentity(const std::wstring& ini, const std::wstring& dllName, const Identity& who)
	{
		if (!IsPluginDllName(dllName) || dllName.find_first_of(L"[]") != std::wstring::npos || who.name.empty())
			return false;

		// The profile functions write Unicode only into a file that starts with
		// a UTF-16 byte order mark. CREATE_NEW leaves an existing file alone.
		const HANDLE file = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		                                FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			const wchar_t mark = 0xFEFF;
			DWORD written = 0;
			WriteFile(file, &mark, sizeof(mark), &written, nullptr);
			CloseHandle(file);
		}

		const auto put = [&](const wchar_t* key, const std::wstring& value)
		{
			return WritePrivateProfileStringW(dllName.c_str(), key, detail::OneLine(value).c_str(), ini.c_str()) != 0;
		};
		return put(L"Name", who.name) && put(L"Version", who.version) && put(L"Author", who.author);
	}

	inline Identity ReadIdentity(const std::wstring& ini, const std::wstring& dllName)
	{
		return { detail::ReadIni(ini, dllName.c_str(), L"Name"),
		         detail::ReadIni(ini, dllName.c_str(), L"Version"),
		         detail::ReadIni(ini, dllName.c_str(), L"Author") };
	}

	// --- Preload DLLs -----------------------------------------------------------

	// The names a DLL in ModLoader\Preload answers to: the one it reports (empty
	// when the loader did not ask it), its file name without ".dll", and that
	// without "-Client" or "-Server". Add them for every DLL in the folder and
	// match a mod index entry with ModIndex::IsInstalled.
	inline void AddPreloadNames(const std::string& fileName, const std::string& reportedName,
	                            std::vector<std::string>& names)
	{
		if (!reportedName.empty())
			names.push_back(reportedName);

		const std::string stem = fileName.substr(0, fileName.rfind('.'));
		if (stem.empty())
			return;
		names.push_back(stem);

		for (const char* suffix : { "-Client", "-Server" })
		{
			const size_t length = strlen(suffix);
			if (stem.size() > length && _stricmp(stem.c_str() + stem.size() - length, suffix) == 0)
				names.push_back(stem.substr(0, stem.size() - length));
		}
	}

	// --- Cards ------------------------------------------------------------------

	enum class CardState
	{
		NotInstalled,
		Running,
		NotRunning,         // installed: unloaded, failed, too old or crashed
		Disabled,
		DisablePending,     // off after a restart
		EnablePending,      // on after a restart
		Uninstalled,        // there is a backup to restore
		UninstalledRunning, // gone after a restart
		Preload,            // installed as a preload DLL, which has its own switch
		Working,            // an action is in flight
	};

	enum CardButton : unsigned
	{
		kNoButtons = 0,
		kDisable   = 1u << 0,
		kEnable    = 1u << 1,
		kUninstall = 1u << 2,
		kRestore   = 1u << 3,
	};

	struct CardFacts
	{
		bool working = false;
		bool inPlugins = false;             // the DLL is in ModLoader\Plugins
		bool running = false;               // its code is loaded and running
		bool listedDisabled = false;        // on [Plugins] Disabled
		bool enableWaitsForRestart = false; // enabled, but it cannot load until a restart
		bool hasBackup = false;
		bool asPreload = false;             // installed in ModLoader\Preload
		bool canBeDisabled = true;          // CanBeDisabled(file name)
		bool vortexListed = false;          // Vortex placed one of its files
	};

	struct Card
	{
		CardState state = CardState::NotInstalled;
		unsigned buttons = kNoButtons;
		bool disableGreyed = false;     // Disable is shown but cannot be used
		bool uninstallInVortex = false; // a line saying so replaces Uninstall
	};

	inline Card DeriveCard(const CardFacts& facts)
	{
		Card card;
		const auto is = [&card](CardState state, unsigned buttons)
		{
			card.state = state;
			card.buttons = buttons;
		};

		if (facts.working)
			is(CardState::Working, kNoButtons);
		else if (facts.inPlugins)
		{
			if (facts.listedDisabled)
				is(facts.running ? CardState::DisablePending : CardState::Disabled, kEnable | kUninstall);
			else if (facts.running)
				is(CardState::Running, kDisable | kUninstall);
			else
				is(facts.enableWaitsForRestart ? CardState::EnablePending : CardState::NotRunning,
				   facts.enableWaitsForRestart ? kDisable : kDisable | kUninstall);
		}
		else if (facts.running)
			is(CardState::UninstalledRunning, facts.hasBackup ? kRestore : kNoButtons);
		else if (facts.asPreload) // before the backup: it is installed, so not "Uninstalled"
			is(CardState::Preload, kNoButtons);
		else if (facts.hasBackup)
			is(CardState::Uninstalled, kRestore);
		else
			is(CardState::NotInstalled, kNoButtons);

		if (facts.vortexListed && (card.buttons & kUninstall))
		{
			card.buttons &= ~static_cast<unsigned>(kUninstall);
			card.uninstallInVortex = true;
		}
		card.disableGreyed = !facts.canBeDisabled && (card.buttons & kDisable);
		return card;
	}
}
