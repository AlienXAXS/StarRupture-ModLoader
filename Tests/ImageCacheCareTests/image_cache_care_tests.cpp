// Image cache care test: the housekeeping of the image library's cache folder
// (UI/image_cache_care.cpp) on a real temporary folder.
//
// Old folders (the current one and the newest other are kept, the rest goes with its set
// list), the size cap (the least recently used files go first, the older folder before the
// current one, the set list never), the leftovers of game instances that are gone (a file
// another instance holds open is skipped), the clear, and the two jobs on the real writer.
//
// A console program: no game, no GPU.
#include "UI/image_cache_care.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

using namespace ImageCache;

// ---------------------------------------------------------------------------
// A cache root in the temp folder

// A process id no process has: ids are multiples of four.
static constexpr unsigned long kGonePid = 4000000001ul;

struct Root
{
    std::wstring path;

    Root()
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"image_cache_care_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        CreateDirectoryW(path.c_str(), nullptr);
    }

    ~Root() { Remove(path); }

    static void Remove(const std::wstring& dir)
    {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    Remove(dir + L"\\" + name);
                else
                    DeleteFileW((dir + L"\\" + name).c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(dir.c_str());
    }

    std::wstring Full(const std::wstring& rel) const { return path + L"\\" + rel; }

    void Folder(const std::wstring& rel) const { CreateDirectoryW(Full(rel).c_str(), nullptr); }

    // A file of `size` bytes, last written `ageSeconds` ago.
    void File(const std::wstring& rel, size_t size = 100, int ageSeconds = 0) const
    {
        HANDLE h = CreateFileW(Full(rel).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        const std::vector<char> bytes(size, 'x');
        DWORD wrote = 0;
        WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
        CloseHandle(h);
        SetAge(rel, ageSeconds);
    }

    // A folder is as old as its newest file, so this ages every file directly inside it.
    void AgeFolder(const std::wstring& rel, int ageSeconds) const
    {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((Full(rel) + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return;
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                SetAge(rel + L"\\" + data.cFileName, ageSeconds);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    // Works on files and folders.
    void SetAge(const std::wstring& rel, int ageSeconds) const
    {
        HANDLE h = CreateFileW(Full(rel).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER t;
        t.LowPart  = now.dwLowDateTime;
        t.HighPart = now.dwHighDateTime;
        t.QuadPart -= static_cast<unsigned long long>(ageSeconds) * 10000000ull;
        FILETIME then;
        then.dwLowDateTime  = t.LowPart;
        then.dwHighDateTime = t.HighPart;
        SetFileTime(h, nullptr, nullptr, &then);
        CloseHandle(h);
    }

    bool Exists(const std::wstring& rel) const { return GetFileAttributesW(Full(rel).c_str()) != INVALID_FILE_ATTRIBUTES; }

    size_t Count(const std::wstring& rel = L"") const
    {
        size_t n = 0;
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(((rel.empty() ? path : Full(rel)) + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return 0;
        do
        {
            const std::wstring name = data.cFileName;
            if (name != L"." && name != L"..")
                ++n;
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return n;
    }
};

static std::wstring Tmp(const wchar_t* stem, unsigned long pid)
{
    return std::wstring(stem) + L"." + std::to_wstring(pid) + L".tmp";
}

static bool WaitFor(const std::function<bool()>& done, int ms = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Old folders

static void TestKeepsCurrentAndNewestOther()
{
    Root root;
    // Newest first: other1, current, other2, other3 (the current one is not the newest).
    for (const wchar_t* name : { L"cur", L"other1", L"other2", L"other3" })
    {
        root.Folder(name);
        root.File(std::wstring(name) + L"\\a.img");
        root.File(std::wstring(name) + L"\\sets.lst", 20);
    }
    root.AgeFolder(L"other1", 10);
    root.AgeFolder(L"cur", 20);
    root.AgeFolder(L"other2", 30);
    root.AgeFolder(L"other3", 40);

    const Care::Result result = Care::Tidy(root.path, root.path + L"\\cur");
    CHECK(root.Exists(L"cur\\a.img") && root.Exists(L"cur\\sets.lst"));
    CHECK(root.Exists(L"other1\\a.img") && root.Exists(L"other1\\sets.lst"));
    // A folder that goes takes its set list with it.
    CHECK(!root.Exists(L"other2") && !root.Exists(L"other3"));
    CHECK(result.foldersRemoved == 2);
    CHECK(result.filesRemoved == 4);
    CHECK(result.bytesFreed == 2 * 120);
}

static void TestCurrentFolderNotMadeYet()
{
    Root root;
    for (const wchar_t* name : { L"old1", L"old2", L"old3" })
    {
        root.Folder(name);
        root.File(std::wstring(name) + L"\\a.img");
    }
    root.AgeFolder(L"old1", 10);
    root.AgeFolder(L"old2", 20);
    root.AgeFolder(L"old3", 30);

    // The current folder is made later by the writer: it counts as the kept one, so only the
    // newest other is kept beside it.
    Care::Tidy(root.path + L"\\", root.path + L"\\cur\\");
    CHECK(root.Exists(L"old1\\a.img"));
    CHECK(!root.Exists(L"old2") && !root.Exists(L"old3"));
    CHECK(!root.Exists(L"cur"));
}

static void TestFolderNameMatchIgnoresCase()
{
    Root root;
    root.Folder(L"Cur");
    root.File(L"Cur\\a.img");
    root.Folder(L"x1");
    root.Folder(L"x2");
    root.File(L"x1\\a.img");
    root.File(L"x2\\a.img");
    root.AgeFolder(L"Cur", 50);
    root.AgeFolder(L"x1", 10);
    root.AgeFolder(L"x2", 20);
    Care::Tidy(root.path, root.path + L"\\cUR");
    CHECK(root.Exists(L"Cur\\a.img") && root.Exists(L"x1") && !root.Exists(L"x2"));
}

static void TestOldFolderWithAFileHeldOpenStays()
{
    Root root;
    root.Folder(L"cur");
    root.Folder(L"new");
    root.Folder(L"old");
    root.File(L"cur\\a.img");
    root.File(L"new\\a.img");
    root.File(L"old\\held.img");
    root.File(L"old\\free.img");
    root.AgeFolder(L"cur", 10);
    root.AgeFolder(L"new", 20);
    root.AgeFolder(L"old", 30);

    // Another instance has a file of the old folder open; the delete is refused and skipped.
    HANDLE held = CreateFileW(root.Full(L"old\\held.img").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const Care::Result result = Care::Tidy(root.path, root.path + L"\\cur");
    CloseHandle(held);

    CHECK(root.Exists(L"old\\held.img"));
    CHECK(!root.Exists(L"old\\free.img"));
    CHECK(result.foldersRemoved == 0);

    // The next start finishes the job.
    Care::Tidy(root.path, root.path + L"\\cur");
    CHECK(!root.Exists(L"old"));
}

static void TestMissingRootIsQuiet()
{
    Root root;
    const std::wstring gone = root.path + L"\\nothing-here";
    const Care::Result tidy = Care::Tidy(gone, gone + L"\\cur");
    const Care::Result clear = Care::Clear(gone);
    CHECK(tidy.filesRemoved == 0 && tidy.foldersRemoved == 0);
    CHECK(clear.filesRemoved == 0 && clear.foldersRemoved == 0);
    CHECK(Care::Tidy(L"", L"").filesRemoved == 0);
    CHECK(Care::Clear(L"").filesRemoved == 0);
}

// ---------------------------------------------------------------------------
// The size cap

static void TestCapDeletesOlderFolderFirstThenOldest()
{
    Root root;
    root.Folder(L"cur");
    root.Folder(L"prev");
    // prev: p1 oldest .. p3; cur: c1 oldest .. c3. 100 bytes each, plus a set list of 20 in each.
    root.File(L"prev\\p1.img", 100, 900);
    root.File(L"prev\\p2.img", 100, 800);
    root.File(L"prev\\p3.img", 100, 700);
    root.File(L"prev\\sets.lst", 20, 1000);
    root.File(L"cur\\c1.img", 100, 600);
    root.File(L"cur\\c2.img", 100, 500);
    root.File(L"cur\\c3.img", 100, 400);
    root.File(L"cur\\sets.lst", 20, 1000);

    // 640 bytes in all. Cap 500: two files go, from prev, oldest first.
    Care::Result result = Care::Tidy(root.path, root.path + L"\\cur", 500);
    CHECK(!root.Exists(L"prev\\p1.img") && !root.Exists(L"prev\\p2.img"));
    CHECK(root.Exists(L"prev\\p3.img") && root.Exists(L"cur\\c1.img") && root.Exists(L"cur\\c2.img") && root.Exists(L"cur\\c3.img"));
    CHECK(result.filesRemoved == 2 && result.bytesFreed == 200);

    // Cap 200: the rest of prev, then the oldest of cur, until it fits (c1 and c2 gone, c3 and both set lists left).
    result = Care::Tidy(root.path, root.path + L"\\cur", 200);
    CHECK(!root.Exists(L"prev\\p3.img"));
    CHECK(!root.Exists(L"cur\\c1.img") && !root.Exists(L"cur\\c2.img"));
    CHECK(root.Exists(L"cur\\c3.img"));
    // The set lists stay however small the cap is.
    CHECK(root.Exists(L"prev\\sets.lst") && root.Exists(L"cur\\sets.lst"));
    Care::Tidy(root.path, root.path + L"\\cur", 0);
    CHECK(!root.Exists(L"cur\\c3.img"));
    CHECK(root.Exists(L"prev\\sets.lst") && root.Exists(L"cur\\sets.lst"));
}

static void TestUnderTheCapNothingGoes()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img", 100);
    root.File(L"cur\\b.img", 100);
    const Care::Result result = Care::Tidy(root.path, root.path + L"\\cur", 200);
    CHECK(result.filesRemoved == 0);
    CHECK(root.Exists(L"cur\\a.img") && root.Exists(L"cur\\b.img"));
    CHECK(Care::kCapBytes == 256ull * 1024 * 1024);
}

static void TestTouchedFileOutlivesOlderOnes()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img", 100, 900);   // the oldest by write time
    root.File(L"cur\\b.img", 100, 800);
    root.File(L"cur\\c.img", 100, 700);

    Care::Touch(root.Full(L"cur\\a.img"));   // used again: now the newest
    Care::Tidy(root.path, root.path + L"\\cur", 200);
    CHECK(root.Exists(L"cur\\a.img"));
    CHECK(!root.Exists(L"cur\\b.img"));
    CHECK(root.Exists(L"cur\\c.img"));

    Care::Touch(root.Full(L"cur\\missing.img"));   // a file that is not there is not an error
    CHECK(!root.Exists(L"cur\\missing.img"));
}

static void TestCapSkipsAFileHeldOpen()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img", 100, 900);
    root.File(L"cur\\b.img", 100, 800);
    root.File(L"cur\\c.img", 100, 700);
    HANDLE held = CreateFileW(root.Full(L"cur\\a.img").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    Care::Tidy(root.path, root.path + L"\\cur", 100);
    CloseHandle(held);
    // The oldest is open elsewhere: it stays and the next ones go instead.
    CHECK(root.Exists(L"cur\\a.img"));
    CHECK(!root.Exists(L"cur\\b.img") && !root.Exists(L"cur\\c.img"));
}

// ---------------------------------------------------------------------------
// Leftovers and two instances

static void TestProcessAlive()
{
    CHECK(Care::ProcessAlive(GetCurrentProcessId()));
    CHECK(!Care::ProcessAlive(kGonePid));
}

static void TestLeftoversOfGoneInstancesAreRemoved()
{
    Root root;
    root.Folder(L"cur");
    root.Folder(L"prev");
    root.File(L"cur\\a.img");
    root.File(L"cur\\" + Tmp(L"b.img", kGonePid), 40);
    root.File(L"prev\\" + Tmp(L"sets.lst", kGonePid), 30);
    root.File(L"cur\\" + Tmp(L"c.img", GetCurrentProcessId()), 50);   // a running instance's
    root.File(L"cur\\notes.tmp", 10);                                 // not the writer's name
    root.File(L"cur\\x.abc.tmp", 10);
    root.AgeFolder(L"cur", 10);
    root.AgeFolder(L"prev", 20);

    const Care::Result result = Care::Tidy(root.path, root.path + L"\\cur");
    CHECK(result.leftoversRemoved == 2);
    CHECK(result.filesRemoved == 2 && result.bytesFreed == 70);
    CHECK(!root.Exists(L"cur\\" + Tmp(L"b.img", kGonePid)));
    CHECK(!root.Exists(L"prev\\" + Tmp(L"sets.lst", kGonePid)));
    CHECK(root.Exists(L"cur\\" + Tmp(L"c.img", GetCurrentProcessId())));
    CHECK(root.Exists(L"cur\\notes.tmp") && root.Exists(L"cur\\x.abc.tmp"));
    CHECK(root.Exists(L"cur\\a.img"));
}

static void TestLeftoverHeldOpenIsSkipped()
{
    Root root;
    root.Folder(L"cur");
    const std::wstring name = L"cur\\" + Tmp(L"b.img", kGonePid);
    root.File(name, 40);

    // A file the writer of another instance still has open (share mode 0, as the writer opens it).
    HANDLE held = CreateFileW(root.Full(name).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const Care::Result result = Care::Tidy(root.path, root.path + L"\\cur");
    CHECK(result.leftoversRemoved == 0 && result.filesRemoved == 0);
    CHECK(root.Exists(name));
    CloseHandle(held);

    CHECK(Care::Tidy(root.path, root.path + L"\\cur").leftoversRemoved == 1);
    CHECK(!root.Exists(name));
}

// ---------------------------------------------------------------------------
// Clear

static void TestClearEmptiesTheRoot()
{
    Root root;
    root.Folder(L"cur");
    root.Folder(L"prev");
    root.Folder(L"prev\\deeper");
    root.File(L"cur\\a.img");
    root.File(L"cur\\sets.lst", 20);
    root.File(L"cur\\" + Tmp(L"b.img", GetCurrentProcessId()), 30);
    root.File(L"prev\\deeper\\c.img");
    root.File(L"loose.txt", 5);

    const Care::Result result = Care::Clear(root.path + L"\\");
    CHECK(root.Exists(L""));          // the root itself stays
    CHECK(root.Count() == 0);
    CHECK(result.filesRemoved == 5);
    CHECK(result.foldersRemoved == 2);
    CHECK(result.bytesFreed == 100 + 20 + 30 + 100 + 5);

    CHECK(Care::Clear(root.path).filesRemoved == 0);   // nothing left, nothing to say
}

static void TestClearSkipsAFileHeldOpen()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img");
    root.File(L"cur\\b.img");
    HANDLE held = CreateFileW(root.Full(L"cur\\a.img").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const Care::Result result = Care::Clear(root.path);
    CloseHandle(held);
    CHECK(root.Exists(L"cur\\a.img") && !root.Exists(L"cur\\b.img"));
    CHECK(result.filesRemoved == 1 && result.foldersRemoved == 0);
}

// ---------------------------------------------------------------------------
// The jobs, on the writer's worker

struct Lines
{
    std::mutex lock;
    std::vector<std::string> lines;
    Care::Log Sink()
    {
        return [this](const std::string& line) { std::lock_guard<std::mutex> l(lock); lines.push_back(line); };
    }
    size_t Count() { std::lock_guard<std::mutex> l(lock); return lines.size(); }
};

static void TestClearJobRunsOnTheWorker()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img");
    root.File(L"cur\\sets.lst", 20);
    Lines lines;
    std::thread::id worker;
    std::mutex lock;

    Writer writer([](const std::string&) {});
    const std::wstring rootPath = root.path;
    Job job = Care::ClearJob([&worker, &lock, rootPath]
    {
        std::lock_guard<std::mutex> l(lock);
        worker = std::this_thread::get_id();
        return rootPath;
    }, lines.Sink());

    // Posting is only a queue push: nothing is deleted before the worker runs it.
    CHECK(writer.Submit(std::move(job)));
    CHECK(WaitFor([&] { return root.Count() == 0; }));
    CHECK(WaitFor([&] { return lines.Count() == 1; }));
    {
        std::lock_guard<std::mutex> l(lock);
        CHECK(worker != std::thread::id() && worker != std::this_thread::get_id());
    }
    writer.Stop();
    CHECK(lines.lines[0].find("cleared") != std::string::npos);
    CHECK(root.Exists(L""));
}

static void TestTidyJobRunsAndWriterKeepsWorking()
{
    Root root;
    root.Folder(L"cur");
    root.Folder(L"prev");
    root.Folder(L"old");
    root.File(L"cur\\a.img");
    root.File(L"prev\\a.img");
    root.File(L"old\\b.img");
    root.AgeFolder(L"cur", 10);
    root.AgeFolder(L"prev", 20);
    root.AgeFolder(L"old", 30);
    Lines lines;

    Writer writer([](const std::string&) {});
    const std::wstring rootPath = root.path;
    CHECK(writer.Submit(Care::TidyJob([rootPath] { return rootPath; }, [rootPath] { return rootPath + L"\\cur"; }, lines.Sink())));
    // A write queued behind the job still lands.
    Job write;
    write.folder = [rootPath] { return rootPath + L"\\cur"; };
    write.name   = "new.img";
    write.make   = [] { return std::vector<uint8_t>{ 1, 2, 3 }; };
    CHECK(writer.Submit(std::move(write)));
    CHECK(WaitFor([&] { return root.Exists(L"cur\\new.img"); }));
    writer.Stop();

    CHECK(!root.Exists(L"old"));
    CHECK(root.Exists(L"prev") && root.Exists(L"cur\\a.img"));
    CHECK(lines.Count() == 1);
    CHECK(!writer.Off());
}

static void TestTidyJobSaysNothingWhenThereIsNothingToDo()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img");
    Lines lines;
    Writer writer([](const std::string&) {});
    const std::wstring rootPath = root.path;
    CHECK(writer.Submit(Care::TidyJob([rootPath] { return rootPath; }, [rootPath] { return rootPath + L"\\cur"; }, lines.Sink())));
    // Stop joins; a job in the queue may be dropped, so wait for the worker to be done with it.
    Job probe;
    probe.folder = [rootPath] { return rootPath + L"\\probe"; };
    probe.name   = "p.img";
    probe.make   = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(probe)));
    CHECK(WaitFor([&] { return root.Exists(L"probe\\p.img"); }));
    writer.Stop();
    CHECK(lines.Count() == 0);
    CHECK(root.Exists(L"cur\\a.img"));
}

static void TestTheWriterTouchesAFileItSkips()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img", 4, 900);   // saved a long time ago, 4 bytes
    root.File(L"cur\\b.img", 4, 800);

    Writer writer([](const std::string&) {});
    const std::wstring folder = root.path + L"\\cur";
    Job again;
    again.folder = [folder] { return folder; };
    again.name   = "a.img";
    again.make   = [] { return std::vector<uint8_t>{ 'x', 'x', 'x', 'x' }; };   // same size: left alone, but in use
    CHECK(writer.Submit(std::move(again)));
    Job probe;
    probe.folder = [folder] { return folder; };
    probe.name   = "p.img";
    probe.make   = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(probe)));
    CHECK(WaitFor([&] { return root.Exists(L"cur\\p.img"); }));
    writer.Stop();

    // a.img is now newer than b.img, so a cap of 5 bytes (p.img is 1) drops b.img first.
    Care::Tidy(root.path, folder, 5);
    CHECK(root.Exists(L"cur\\a.img"));
    CHECK(!root.Exists(L"cur\\b.img"));
}

static void TestTidyWithoutACurrentFolderTouchesNothing()
{
    Root root;
    root.Folder(L"cur");
    root.Folder(L"prev");
    root.Folder(L"old");
    root.File(L"cur\\a.img");
    root.File(L"prev\\a.img");
    root.File(L"old\\b.img");
    root.File(L"old\\x.99.tmp");
    root.AgeFolder(L"cur", 10);
    root.AgeFolder(L"prev", 20);
    root.AgeFolder(L"old", 30);

    // The disk part is off, so the session has no folder: neither the pruning nor the cap runs, and
    // the folder the session reads from cannot be among the deleted ones.
    const Care::Result result = Care::Tidy(root.path, L"", 1);
    CHECK(result.foldersRemoved == 0 && result.filesRemoved == 0);
    CHECK(root.Exists(L"cur\\a.img") && root.Exists(L"prev\\a.img") && root.Exists(L"old\\b.img"));

    Lines lines;
    Writer writer([](const std::string&) {});
    const std::wstring rootPath = root.path;
    CHECK(writer.Submit(Care::TidyJob([rootPath] { return rootPath; }, [] { return std::wstring(); }, lines.Sink())));
    Job probe;
    probe.folder = [rootPath] { return rootPath + L"\\probe"; };
    probe.name   = "p.img";
    probe.make   = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(probe)));
    CHECK(WaitFor([&] { return root.Exists(L"probe\\p.img"); }));
    writer.Stop();
    CHECK(root.Exists(L"cur\\a.img") && root.Exists(L"prev\\a.img") && root.Exists(L"old\\b.img"));
    CHECK(lines.Count() == 0);
}

static void TestClearJobRunsAfterTheWriterWentOff()
{
    Root root;
    root.Folder(L"cur");
    root.File(L"cur\\a.img");
    root.File(L"cur\\sets.lst", 20);
    root.File(L"blocked");   // a file where a folder should be: the folder cannot be made
    Lines lines;

    Writer writer([](const std::string&) {});
    const std::wstring rootPath = root.path;
    Job bad;
    bad.folder = [rootPath] { return rootPath + L"\\blocked\\sub"; };
    bad.name   = "a.img";
    bad.make   = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(bad)));
    CHECK(WaitFor([&] { return writer.Off(); }));

    // An ordinary job is refused now, the clear job is not, and it empties the root.
    Job plain;
    plain.folder = [rootPath] { return rootPath + L"\\fine"; };
    plain.name   = "b.img";
    plain.make   = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(!writer.Submit(std::move(plain)));
    CHECK(writer.Submit(Care::ClearJob([rootPath] { return rootPath; }, lines.Sink())));
    CHECK(WaitFor([&] { return lines.Count() == 1; }));
    writer.Stop();
    CHECK(root.Count() == 0 && root.Exists(L""));
    CHECK(lines.lines[0].find("cleared") != std::string::npos);
    CHECK(!root.Exists(L"fine"));
}

int main()
{
    RunTest("the current folder and the newest other are kept", TestKeepsCurrentAndNewestOther);
    RunTest("a current folder that is not made yet keeps one other", TestCurrentFolderNotMadeYet);
    RunTest("the folder name match ignores case", TestFolderNameMatchIgnoresCase);
    RunTest("an old folder with a file held open stays until the next start", TestOldFolderWithAFileHeldOpenStays);
    RunTest("a missing or empty root is quiet", TestMissingRootIsQuiet);
    RunTest("the cap deletes the older folder first, oldest first, never the set list", TestCapDeletesOlderFolderFirstThenOldest);
    RunTest("under the cap nothing goes", TestUnderTheCapNothingGoes);
    RunTest("a file used again outlives older ones", TestTouchedFileOutlivesOlderOnes);
    RunTest("the cap skips a file another instance has open", TestCapSkipsAFileHeldOpen);
    RunTest("process ids", TestProcessAlive);
    RunTest("temp files of instances that are gone are removed", TestLeftoversOfGoneInstancesAreRemoved);
    RunTest("a temp file held open is skipped quietly", TestLeftoverHeldOpenIsSkipped);
    RunTest("clear empties the root and keeps the root", TestClearEmptiesTheRoot);
    RunTest("clear skips a file held open", TestClearSkipsAFileHeldOpen);
    RunTest("the clear job runs on the worker", TestClearJobRunsOnTheWorker);
    RunTest("the tidy job runs and the writer keeps working", TestTidyJobRunsAndWriterKeepsWorking);
    RunTest("the tidy job says nothing when there is nothing to do", TestTidyJobSaysNothingWhenThereIsNothingToDo);
    RunTest("the writer touches a file it skips", TestTheWriterTouchesAFileItSkips);
    RunTest("tidy without a current folder touches nothing", TestTidyWithoutACurrentFolderTouchesNothing);
    RunTest("the clear job runs after the writer went off", TestClearJobRunsAfterTheWriterWentOff);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
