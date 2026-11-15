// Image disk test: the files the image library saves and the rules around them.
//
// The format (UI/image_cache_file.cpp): a file written and read back, and every way a file can be
// wrong (bad magic, wrong revision, truncated, a flipped byte, too many levels). The folder key
// (a change in any input gives a new folder). The writer (UI/image_cache_writer.cpp) on a real
// temporary folder: tmp then rename, a file of the same size left alone, a folder that cannot be
// written switching the disk part off once, the bounded queue, and the stop. The saver
// (UI/image_disk.cpp) on a fake readback: what a finished copy becomes and when the set list is
// written.
//
// A console program: no game, no GPU.
#include "UI/image_cache_reader.h"
#include "UI/image_disk.h"
#include "UI/image_shrink.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <cstdio>
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
// Helpers

static std::vector<uint8_t> MakePixels(uint32_t width, uint32_t height, uint8_t seed)
{
    const uint32_t levels = ImageShrink::LevelCount(width, height);
    std::vector<uint8_t> pixels(static_cast<size_t>(ImageShrink::Bytes({ width, height }, levels)));
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<uint8_t>(seed + i * 7);
    return pixels;
}

static Info MakeInfo(uint32_t width = 64, uint32_t height = 32)
{
    Info info;
    info.sourceFormat = 77;
    info.sourceWidth  = 512;
    info.sourceHeight = 256;
    info.storedWidth  = width;
    info.storedHeight = height;
    info.levels       = ImageShrink::LevelCount(width, height);
    info.path         = "/Game/UI/T_Icon.T_Icon";
    return info;
}

static void Put32At(std::vector<uint8_t>& file, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        file[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

static std::vector<uint8_t> Valid(Info* outInfo = nullptr, std::vector<uint8_t>* outPixels = nullptr)
{
    const Info info = MakeInfo();
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 3);
    if (outInfo) *outInfo = info;
    if (outPixels) *outPixels = pixels;
    return Serialize(info, pixels.data(), pixels.size());
}

static Parsed ParseOf(const std::vector<uint8_t>& file)
{
    Info info;
    const uint8_t* pixels = nullptr;
    return Parse(file.data(), file.size(), info, pixels);
}

// ---------------------------------------------------------------------------
// The format

static void TestCrc()
{
    const char* text = "123456789";
    CHECK(Crc32(text, 9) == 0xCBF43926u);
    CHECK(Crc32(nullptr, 0) == 0);
    // Chained over two parts it is the crc of the whole.
    CHECK(Crc32(text + 4, 5, Crc32(text, 4)) == Crc32(text, 9));
}

static void TestRoundTrip()
{
    Info info;
    std::vector<uint8_t> pixels;
    const std::vector<uint8_t> file = Valid(&info, &pixels);
    CHECK(!file.empty());
    CHECK(file.size() == kHeaderBytes + info.path.size() + pixels.size());

    Info read;
    const uint8_t* payload = nullptr;
    CHECK(Parse(file.data(), file.size(), read, payload) == Parsed::Ok);
    CHECK(read.sourceFormat == info.sourceFormat);
    CHECK(read.sourceWidth == info.sourceWidth && read.sourceHeight == info.sourceHeight);
    CHECK(read.storedWidth == info.storedWidth && read.storedHeight == info.storedHeight);
    CHECK(read.levels == info.levels);
    CHECK(read.path == info.path);
    CHECK(payload == file.data() + kHeaderBytes + info.path.size());
    CHECK(payload && std::equal(pixels.begin(), pixels.end(), payload));
}

static void TestRoundTripShapes()
{
    // The shapes the shrink makes: square, wide, tall and the smallest.
    const uint32_t sizes[][2] = { { 256, 256 }, { 256, 171 }, { 181, 256 }, { 1, 1 }, { 32, 32 } };
    for (const auto& size : sizes)
    {
        const Info info = MakeInfo(size[0], size[1]);
        const std::vector<uint8_t> pixels = MakePixels(size[0], size[1], 9);
        const std::vector<uint8_t> file = Serialize(info, pixels.data(), pixels.size());
        Info read;
        const uint8_t* payload = nullptr;
        CHECK(Parse(file.data(), file.size(), read, payload) == Parsed::Ok);
        CHECK(payload && std::equal(pixels.begin(), pixels.end(), payload));
    }
}

static void TestSerializeRefusals()
{
    const Info info = MakeInfo();
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 1);

    CHECK(Serialize(info, pixels.data(), pixels.size() - 1).empty());
    CHECK(Serialize(info, pixels.data(), pixels.size() + 4).empty());
    CHECK(Serialize(info, nullptr, pixels.size()).empty());

    Info wrongLevels = info;
    wrongLevels.levels = info.levels + 1;
    CHECK(Serialize(wrongLevels, pixels.data(), pixels.size()).empty());

    Info noPath = info;
    noPath.path.clear();
    CHECK(Serialize(noPath, pixels.data(), pixels.size()).empty());

    Info longPath = info;
    longPath.path.assign(kMaxPathLength + 1, 'a');
    CHECK(Serialize(longPath, pixels.data(), pixels.size()).empty());

    Info empty = info;
    empty.storedWidth = 0;
    CHECK(Serialize(empty, pixels.data(), pixels.size()).empty());
}

static void TestBadMagic()
{
    std::vector<uint8_t> file = Valid();
    file[0] ^= 0xFF;
    CHECK(ParseOf(file) == Parsed::BadMagic);
    // A set list is not an image file, whatever its size.
    SetList sets;
    sets.push_back({ 1, { "/Game/UI/T_A.T_A", "/Game/UI/T_B.T_B", "/Game/UI/T_C.T_C" } });
    CHECK(ParseOf(SerializeSets(sets)) == Parsed::BadMagic);
}

static void TestWrongRevision()
{
    std::vector<uint8_t> file = Valid();
    Put32At(file, 4, kRevision + 1);
    CHECK(ParseOf(file) == Parsed::WrongRevision);
    Put32At(file, 4, 0);
    CHECK(ParseOf(file) == Parsed::WrongRevision);
}

static void TestTruncated()
{
    const std::vector<uint8_t> file = Valid();

    CHECK(ParseOf({}) == Parsed::TooShort);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + 10)) == Parsed::TooShort);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + kHeaderBytes - 1)) == Parsed::TooShort);

    // Header only, header and part of the path, and the payload cut anywhere.
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + kHeaderBytes)) == Parsed::Truncated);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + kHeaderBytes + 5)) == Parsed::Truncated);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.end() - 1)) == Parsed::Truncated);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + file.size() / 2)) == Parsed::Truncated);

    // Bytes after the payload are not part of the file.
    std::vector<uint8_t> longer = file;
    longer.push_back(0);
    CHECK(ParseOf(longer) == Parsed::Truncated);
}

static void TestCrcMismatch()
{
    const std::vector<uint8_t> file = Valid();

    std::vector<uint8_t> pixel = file;
    pixel.back() ^= 1;
    CHECK(ParseOf(pixel) == Parsed::BadCrc);

    std::vector<uint8_t> path = file;
    path[kHeaderBytes + 3] ^= 1;
    CHECK(ParseOf(path) == Parsed::BadCrc);

    // A header field the other checks do not constrain (the source format, the source size).
    std::vector<uint8_t> format = file;
    format[8] ^= 1;
    CHECK(ParseOf(format) == Parsed::BadCrc);
    std::vector<uint8_t> source = file;
    source[12] ^= 1;
    CHECK(ParseOf(source) == Parsed::BadCrc);

    std::vector<uint8_t> stored = file;
    stored[44] ^= 1;   // the crc itself
    CHECK(ParseOf(stored) == Parsed::BadCrc);
}

static void TestBadHeader()
{
    const std::vector<uint8_t> file = Valid();

    // More levels than a chain can have, however well the rest agrees.
    std::vector<uint8_t> levels = file;
    Put32At(levels, 28, kMaxLevels + 1);
    CHECK(ParseOf(levels) == Parsed::BadHeader);
    Put32At(levels, 28, 0);
    CHECK(ParseOf(levels) == Parsed::BadHeader);
    Put32At(levels, 28, 0xFFFFFFFFu);
    CHECK(ParseOf(levels) == Parsed::BadHeader);

    // A level count that is not the chain of the stored size.
    std::vector<uint8_t> chain = file;
    Put32At(chain, 28, ImageShrink::LevelCount(64, 32) - 1);
    CHECK(ParseOf(chain) == Parsed::BadHeader);

    std::vector<uint8_t> width = file;
    Put32At(width, 20, kMaxSide + 1);
    CHECK(ParseOf(width) == Parsed::BadHeader);
    Put32At(width, 20, 0);
    CHECK(ParseOf(width) == Parsed::BadHeader);

    std::vector<uint8_t> path = file;
    Put32At(path, 32, kMaxPathLength + 1);
    CHECK(ParseOf(path) == Parsed::BadHeader);
    Put32At(path, 32, 0);
    CHECK(ParseOf(path) == Parsed::BadHeader);

    // A payload size that is not the size of the levels.
    std::vector<uint8_t> payload = file;
    Put32At(payload, 36, 1);
    CHECK(ParseOf(payload) == Parsed::BadHeader);
    std::vector<uint8_t> huge = file;
    Put32At(huge, 40, 0x7FFFFFFFu);
    CHECK(ParseOf(huge) == Parsed::BadHeader);

    std::vector<uint8_t> source = file;
    Put32At(source, 12, 0);
    CHECK(ParseOf(source) == Parsed::BadHeader);
}

static void TestSetListRoundTrip()
{
    SetList sets;
    sets.push_back({ 1, { "/Game/A.A", "/Game/B.B" } });
    sets.push_back({ 2, { "/Game/C.C" } });

    const std::vector<uint8_t> file = SerializeSets(sets);
    SetList read;
    CHECK(ParseSets(file.data(), file.size(), read) == Parsed::Ok);
    CHECK(read == sets);

    // An empty list is a valid file.
    const std::vector<uint8_t> none = SerializeSets({});
    SetList empty = sets;
    CHECK(ParseSets(none.data(), none.size(), empty) == Parsed::Ok);
    CHECK(empty.empty());
}

static void TestSetListBad()
{
    SetList sets;
    sets.push_back({ 1, { "/Game/A.A", "/Game/B.B" } });
    const std::vector<uint8_t> file = SerializeSets(sets);
    SetList read;

    std::vector<uint8_t> magic = file;
    magic[0] ^= 1;
    CHECK(ParseSets(magic.data(), magic.size(), read) == Parsed::BadMagic);

    std::vector<uint8_t> revision = file;
    Put32At(revision, 4, kRevision + 1);
    CHECK(ParseSets(revision.data(), revision.size(), read) == Parsed::WrongRevision);

    CHECK(ParseSets(file.data(), 8, read) == Parsed::TooShort);
    CHECK(ParseSets(file.data(), file.size() - 1, read) == Parsed::BadCrc);

    std::vector<uint8_t> flipped = file;
    flipped[flipped.size() - 8] ^= 1;
    CHECK(ParseSets(flipped.data(), flipped.size(), read) == Parsed::BadCrc);

    // A count that runs past the data, with a crc that is right for the bytes.
    std::vector<uint8_t> count = file;
    Put32At(count, 16, 1000);
    Put32At(count, count.size() - 4, Crc32(count.data(), count.size() - 4));
    CHECK(ParseSets(count.data(), count.size(), read) == Parsed::Truncated);

    std::vector<uint8_t> setCount = file;
    Put32At(setCount, 8, kMaxSets + 1);
    Put32At(setCount, setCount.size() - 4, Crc32(setCount.data(), setCount.size() - 4));
    CHECK(ParseSets(setCount.data(), setCount.size(), read) == Parsed::BadHeader);
}

// ---------------------------------------------------------------------------
// Names and the folder key

static void TestFileName()
{
    const std::string a = FileName("/game/ui/t_a.t_a");
    CHECK(a == FileName("/game/ui/t_a.t_a"));
    CHECK(a != FileName("/game/ui/t_b.t_b"));
    CHECK(a.size() == 16 + 4);
    CHECK(a.substr(16) == ".img");
    CHECK(a.find_first_not_of("0123456789abcdef.img") == std::string::npos);
}

static void TestFolderKey()
{
    FolderKey base;
    base.section    = L"1.0.0-CL-127004|65A1B2C3|3F40000";
    base.pakHash    = 0x1234;
    base.revision   = kRevision;
    base.storedSize = 256;
    const std::wstring name = FolderName(base);

    CHECK(name == FolderName(base));
    CHECK(name.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos);

    FolderKey changed = base;
    changed.section = L"1.0.0-CL-129402|65A1B2C3|3F40000";    // a game update
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.section = L"1.0.0-CL-127004|65A1B2C4|3F40000";    // the same version, another exe
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.section = L"1.0.0-CL-127004|65A1B2C3|3F40001";    // another image size
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.pakHash = 0x1235;                                 // a pak change
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.revision = kRevision + 1;                         // a format change
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.storedSize = 128;                                 // a size change
    CHECK(FolderName(changed) != name);
}

static void TestPakHash()
{
    const std::vector<PakEntry> paks = {
        { "C:/Game/Content/Paks/pakchunk0.pak", "../../../StarRupture/Content/" },
        { "C:/Game/Content/Paks/~mods/a.pak", "../../../StarRupture/Content/" },
    };
    const uint64_t hash = HashPakSet(paks);

    // The order the pak list comes in does not matter.
    CHECK(HashPakSet({ paks[1], paks[0] }) == hash);

    std::vector<PakEntry> added = paks;
    added.push_back({ "C:/Game/Content/Paks/~mods/b.pak", "../../../StarRupture/Content/" });
    CHECK(HashPakSet(added) != hash);

    std::vector<PakEntry> removed = { paks[0] };
    CHECK(HashPakSet(removed) != hash);

    std::vector<PakEntry> moved = paks;
    moved[1].mountPoint = "../../../Other/";
    CHECK(HashPakSet(moved) != hash);

    // A path and a mount point do not run into each other.
    CHECK(HashPakSet({ { "ab", "c" } }) != HashPakSet({ { "a", "bc" } }));
    CHECK(HashPakSet({}) != hash);
}

// ---------------------------------------------------------------------------
// The writer, on a real folder

struct TempFolder
{
    std::wstring path;

    TempFolder()
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"image_disk_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    }

    ~TempFolder() { Remove(path); }

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

    std::vector<std::wstring> Files(const std::wstring& sub = L"") const
    {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((path + sub + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return files;
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return files;
    }

    std::vector<uint8_t> Read(const std::wstring& file) const
    {
        std::vector<uint8_t> bytes;
        HANDLE h = CreateFileW((path + L"\\" + file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return bytes;
        bytes.resize(GetFileSize(h, nullptr));
        DWORD read = 0;
        ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(h);
        return bytes;
    }
};

struct LogCollector
{
    std::mutex lock;
    std::vector<std::string> lines;
    Writer::Log Sink()
    {
        return [this](const std::string& line) { std::lock_guard<std::mutex> l(lock); lines.push_back(line); };
    }
    size_t Count() { std::lock_guard<std::mutex> l(lock); return lines.size(); }
};

static Job BytesJob(const std::wstring& folder, const std::string& name, std::vector<uint8_t> bytes, bool replace = false)
{
    Job job;
    job.folder  = [folder] { return folder; };
    job.name    = name;
    job.make    = [bytes] { return bytes; };
    job.replace = replace;
    return job;
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

static void TestWriterWrites()
{
    TempFolder temp;
    LogCollector log;
    const std::wstring folder = temp.path + L"\\a\\b";   // neither level exists yet
    {
        Writer writer(log.Sink());
        CHECK(writer.Submit(BytesJob(folder, "one.img", { 1, 2, 3, 4 })));
        CHECK(writer.Submit(BytesJob(folder, "two.img", { 5, 6 })));
        CHECK(WaitFor([&] { return temp.Files(L"\\a\\b").size() == 2; }));
        writer.Stop();
    }
    // Only the finished files: no tmp name is left behind.
    const std::vector<std::wstring> files = temp.Files(L"\\a\\b");
    CHECK(files.size() == 2);
    for (const std::wstring& file : files)
        CHECK(file.find(L".tmp") == std::wstring::npos);
    CHECK(temp.Read(L"a\\b\\one.img") == (std::vector<uint8_t>{ 1, 2, 3, 4 }));
    CHECK(temp.Read(L"a\\b\\two.img") == (std::vector<uint8_t>{ 5, 6 }));
    CHECK(log.Count() == 0);
}

static void TestWriterKeepsSameSize()
{
    TempFolder temp;
    LogCollector log;
    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 1, 1, 1, 1 })));
    CHECK(WaitFor([&] { return temp.Read(L"x.img").size() == 4; }));

    // The same size is left alone; a job that says replace writes over it.
    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 2, 2, 2, 2 })));
    CHECK(writer.Submit(BytesJob(temp.path, "marker.img", { 9 })));
    CHECK(WaitFor([&] { return temp.Read(L"marker.img").size() == 1; }));
    CHECK(temp.Read(L"x.img") == (std::vector<uint8_t>{ 1, 1, 1, 1 }));

    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 3, 3, 3, 3 }, true)));
    CHECK(WaitFor([&] { return temp.Read(L"x.img") == std::vector<uint8_t>{ 3, 3, 3, 3 }; }));

    // A different size is written.
    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 4, 4 })));
    CHECK(WaitFor([&] { return temp.Read(L"x.img") == std::vector<uint8_t>{ 4, 4 }; }));
    writer.Stop();
    CHECK(temp.Files().size() == 2);
}

static void TestWriterEmptyBytesWriteNothing()
{
    TempFolder temp;
    LogCollector log;
    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "none.img", {})));
    CHECK(writer.Submit(BytesJob(temp.path, "marker.img", { 1 })));
    CHECK(WaitFor([&] { return temp.Read(L"marker.img").size() == 1; }));
    writer.Stop();
    CHECK(temp.Files().size() == 1);
    CHECK(!writer.Off());
}

static void TestWriterOffOnUnusableFolder()
{
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    // A file where the folder should be: the folder cannot be made.
    {
        HANDLE h = CreateFileW((temp.path + L"\\blocked").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CloseHandle(h);
    }

    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path + L"\\blocked\\sub", "a.img", { 1 })));
    CHECK(WaitFor([&] { return writer.Off(); }));

    // One line, and nothing more is taken or logged.
    CHECK(log.Count() == 1);
    CHECK(!writer.Submit(BytesJob(temp.path + L"\\fine", "b.img", { 1 })));
    CHECK(!writer.Submit(BytesJob(temp.path + L"\\blocked\\sub", "c.img", { 1 })));
    writer.Stop();
    CHECK(log.Count() == 1);
    CHECK(temp.Files(L"\\fine").empty());
}

static void TestWriterQueueIsBounded()
{
    TempFolder temp;
    LogCollector log;
    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };

    Writer writer(log.Sink(), 3);
    Job blocker;
    blocker.folder = [&temp] { return temp.path; };
    blocker.name   = "blocker.img";
    blocker.make   = [&] { started = true; WaitFor([&] { return release.load(); }); return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(blocker)));
    CHECK(WaitFor([&] { return started.load(); }));

    // The worker holds one job, so three more fit and the fourth does not.
    CHECK(writer.Submit(BytesJob(temp.path, "1.img", { 1, 1 })));
    CHECK(writer.Submit(BytesJob(temp.path, "2.img", { 1, 1 })));
    CHECK(writer.Submit(BytesJob(temp.path, "3.img", { 1, 1 })));
    CHECK(!writer.Submit(BytesJob(temp.path, "4.img", { 1, 1 })));
    CHECK(!writer.Off());

    release = true;
    CHECK(WaitFor([&] { return temp.Files().size() == 4; }));
    // Room again.
    CHECK(writer.Submit(BytesJob(temp.path, "5.img", { 1, 1 })));
    writer.Stop();
}

static void TestWriterRefusesBadJobs()
{
    LogCollector log;
    Writer writer(log.Sink());
    Job noMake;
    noMake.folder = [] { return std::wstring(L"x"); };
    noMake.name   = "a.img";
    CHECK(!writer.Submit(std::move(noMake)));
    Job noFolder;
    noFolder.name = "a.img";
    noFolder.make = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(!writer.Submit(std::move(noFolder)));
}

static void TestWriterStop()
{
    TempFolder temp;
    LogCollector log;
    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };

    Writer writer(log.Sink());
    Job blocker;
    blocker.folder = [&temp] { return temp.path; };
    blocker.name   = "blocker.img";
    blocker.make   = [&] { started = true; WaitFor([&] { return release.load(); }); return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(blocker)));
    CHECK(WaitFor([&] { return started.load(); }));
    CHECK(writer.Submit(BytesJob(temp.path, "queued.img", { 2 })));

    std::thread stopper([&] { writer.Stop(); });
    // Stop has begun once nothing is taken any more.
    CHECK(WaitFor([&] { return !writer.Submit(BytesJob(temp.path, "late.img", { 3 })); }));
    release = true;
    stopper.join();

    // The job in hand finished; the one queued and the late one were never written.
    const std::vector<std::wstring> files = temp.Files();
    CHECK(files.size() == 1 && files[0] == L"blocker.img");
    CHECK(!writer.Submit(BytesJob(temp.path, "after.img", { 4 })));
    writer.Stop();   // twice is fine
    CHECK(temp.Files().size() == 1);
}

static void TestWriterStopBeforeStart()
{
    LogCollector log;
    Writer writer(log.Sink());
    writer.Stop();   // no thread was ever started
    CHECK(!writer.Submit(BytesJob(L"x", "a.img", { 1 })));
}

// ---------------------------------------------------------------------------
// The saver, on a fake readback and a fake sink

class FakeSink : public Sink
{
public:
    std::vector<Job> jobs;
    bool off      = false;
    bool accept   = true;
    int  stops    = 0;

    bool Submit(Job&& job) override
    {
        if (!accept || off)
            return false;
        jobs.push_back(std::move(job));
        return true;
    }
    bool Off() const override { return off; }
    void Stop() override { ++stops; }
};

class FakeReadbacks : public ImageDisk::Host
{
public:
    struct Entry
    {
        ImageDisk::Readback state = ImageDisk::Readback::Pending;
        std::vector<uint8_t> pixels;
    };
    std::map<uint32_t, Entry> entries;
    std::vector<uint32_t> taken;       // tickets asked for
    std::vector<bool> hadDest;         // whether each ask had somewhere to put the pixels

    ImageDisk::Readback Take(uint32_t ticket, uint8_t* dest, size_t bytes) override
    {
        taken.push_back(ticket);
        hadDest.push_back(dest != nullptr);
        const auto found = entries.find(ticket);
        if (found == entries.end())
            return ImageDisk::Readback::Gone;
        if (found->second.state == ImageDisk::Readback::Done && dest && found->second.pixels.size() == bytes)
            memcpy(dest, found->second.pixels.data(), bytes);
        return found->second.state;
    }

    std::function<std::wstring()> CacheFolder() override { return [] { return std::wstring(L"cache\\folder"); }; }
};

static ImageDisk::Started MakeStarted(uint32_t ticket, const char* path = "/Game/UI/T_Icon.T_Icon")
{
    ImageDisk::Started started;
    started.path         = path;
    started.sourceFormat = 77;
    started.sourceWidth  = 512;
    started.sourceHeight = 512;
    started.storedWidth  = 64;
    started.storedHeight = 32;
    started.levels       = ImageShrink::LevelCount(64, 32);
    started.ticket       = ticket;
    return started;
}

static void TestSaverWritesFinishedCopy()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    const std::vector<uint8_t> pixels = MakePixels(64, 32, 11);
    host.entries[5].pixels = pixels;

    saver.Begin(MakeStarted(5));
    saver.Poll(0);
    // Still on the GPU: asked, nothing queued, asked again next time.
    CHECK(host.taken.size() == 1 && sink.jobs.empty());
    saver.Poll(1);
    CHECK(host.taken.size() == 2 && sink.jobs.empty());

    host.entries[5].state = ImageDisk::Readback::Done;
    saver.Poll(2);
    CHECK(sink.jobs.size() == 1);
    // Taken once: it is not asked about again.
    saver.Poll(3);
    CHECK(host.taken.size() == 3);

    if (!sink.jobs.empty())
    {
        const Job& job = sink.jobs[0];
        CHECK(job.folder && job.folder() == L"cache\\folder");
        CHECK(job.name == FileName(ImageLibrary::NormalizeObjectPath("/Game/UI/T_Icon.T_Icon")));
        CHECK(!job.replace);

        const std::vector<uint8_t> file = job.make();
        Info info;
        const uint8_t* payload = nullptr;
        CHECK(Parse(file.data(), file.size(), info, payload) == Parsed::Ok);
        CHECK(info.path == "/Game/UI/T_Icon.T_Icon");
        CHECK(info.sourceFormat == 77 && info.sourceWidth == 512 && info.sourceHeight == 512);
        CHECK(info.storedWidth == 64 && info.storedHeight == 32);
        CHECK(payload && std::equal(pixels.begin(), pixels.end(), payload));
    }
}

static void TestSaverIgnoresWhatIsNotSaved()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    // No ticket: the readback ring was full, the image stays in memory only.
    saver.Begin(MakeStarted(0));
    saver.Poll(0);
    CHECK(host.taken.empty() && sink.jobs.empty());

    // A readback that is gone ends without a file.
    host.entries[3].state = ImageDisk::Readback::Gone;
    saver.Begin(MakeStarted(3));
    saver.Poll(1);
    saver.Poll(2);
    CHECK(host.taken.size() == 1 && sink.jobs.empty());
}

static void TestSaverWithDiskOffOnlyFreesTheReadback()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    sink.off = true;
    host.entries[2].state = ImageDisk::Readback::Done;
    host.entries[2].pixels = MakePixels(64, 32, 1);
    saver.Begin(MakeStarted(2));
    saver.Poll(0);
    CHECK(host.taken.size() == 1 && !host.hadDest[0]);
    CHECK(sink.jobs.empty());
    saver.Poll(1);
    CHECK(host.taken.size() == 1);
}

static void TestSaverQueueFullDropsTheImage()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    sink.accept = false;
    host.entries[1].state = ImageDisk::Readback::Done;
    host.entries[1].pixels = MakePixels(64, 32, 1);
    saver.Begin(MakeStarted(1));
    saver.Poll(0);
    CHECK(sink.jobs.empty());
    saver.Poll(1);
    CHECK(host.taken.size() == 1);   // not kept waiting for room
}

static void TestSaverSetList()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    // No set members: no file.
    saver.Poll(0);
    CHECK(sink.jobs.empty());

    const PluginImageId a = library.Path("/Game/UI/T_A.T_A");
    const PluginImageId b = library.Path("/Game/UI/T_B.T_B");
    const PluginImageId c = library.Path("/Game/UI/T_C.T_C");
    library.AddToSets(a, PLUGIN_IMAGE_SET_ITEMS);
    library.AddToSets(b, PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS);
    (void)c;   // in no set

    // Looked at once per interval, not per poll.
    saver.Poll(ImageDisk::kSetListIntervalMs - 1);
    CHECK(sink.jobs.empty());
    saver.Poll(ImageDisk::kSetListIntervalMs);
    CHECK(sink.jobs.size() == 1);

    if (!sink.jobs.empty())
    {
        const Job& job = sink.jobs[0];
        CHECK(job.name == SetListFileName());
        CHECK(job.replace);
        const std::vector<uint8_t> file = job.make();
        SetList sets;
        CHECK(ParseSets(file.data(), file.size(), sets) == Parsed::Ok);
        CHECK(sets.size() == 2);
        if (sets.size() == 2)
        {
            CHECK(sets[0].set == PLUGIN_IMAGE_SET_ITEMS && sets[0].paths.size() == 2);
            CHECK(sets[1].set == PLUGIN_IMAGE_SET_BUILDINGS && sets[1].paths.size() == 1);
            CHECK(sets[1].paths[0] == "/Game/UI/T_B.T_B");
        }
    }

    // Unchanged: not written again.
    saver.Poll(ImageDisk::kSetListIntervalMs * 2);
    CHECK(sink.jobs.size() == 1);

    // A new member: written again, at the next look.
    library.AddToSets(c, PLUGIN_IMAGE_SET_BUILDINGS);
    saver.Poll(ImageDisk::kSetListIntervalMs * 2 + 1);
    CHECK(sink.jobs.size() == 1);
    saver.Poll(ImageDisk::kSetListIntervalMs * 3);
    CHECK(sink.jobs.size() == 2);

    // A queue with no room: tried again at the next look.
    library.AddToSets(library.Path("/Game/UI/T_D.T_D"), PLUGIN_IMAGE_SET_ITEMS);
    sink.accept = false;
    saver.Poll(ImageDisk::kSetListIntervalMs * 4);
    CHECK(sink.jobs.size() == 2);
    sink.accept = true;
    saver.Poll(ImageDisk::kSetListIntervalMs * 5);
    CHECK(sink.jobs.size() == 3);
}

static void TestSaverNames()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    saver.Poll(0);
    CHECK(sink.jobs.empty());

    saver.Name("iron plate", "/game/ui/t_iron.t_iron");
    saver.Name("copper", "/game/ui/t_copper.t_copper");
    saver.Poll(ImageDisk::kSetListIntervalMs - 1);
    CHECK(sink.jobs.empty());
    saver.Poll(ImageDisk::kSetListIntervalMs);
    CHECK(sink.jobs.size() == 1);
    if (!sink.jobs.empty())
    {
        const Job& job = sink.jobs[0];
        CHECK(job.name == NameListFileName());
        CHECK(job.replace);
        const std::vector<uint8_t> file = job.make();
        NameList names;
        CHECK(ParseNames(file.data(), file.size(), names) == Parsed::Ok);
        CHECK(names.size() == 2);
        if (names.size() == 2)
        {
            CHECK(names[0].name == "copper" && names[0].path == "/game/ui/t_copper.t_copper");   // sorted
            CHECK(names[1].name == "iron plate");
        }
    }

    // The same pair again changes nothing; a new path for a name does.
    saver.Name("copper", "/game/ui/t_copper.t_copper");
    saver.Poll(ImageDisk::kSetListIntervalMs * 2);
    CHECK(sink.jobs.size() == 1);
    saver.Name("copper", "/game/ui/t_other.t_other");
    saver.Poll(ImageDisk::kSetListIntervalMs * 3);
    CHECK(sink.jobs.size() == 2);

    // A queue with no room: tried again at the next look.
    saver.Name("steel", "/game/ui/t_steel.t_steel");
    sink.accept = false;
    saver.Poll(ImageDisk::kSetListIntervalMs * 4);
    CHECK(sink.jobs.size() == 2);
    sink.accept = true;
    saver.Poll(ImageDisk::kSetListIntervalMs * 5);
    CHECK(sink.jobs.size() == 3);
}

static void TestSaverStopAndAbandon()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    // The GPU is gone: pending readbacks are dropped without being asked about.
    saver.Begin(MakeStarted(1));
    saver.Abandon();
    saver.Poll(0);
    CHECK(host.taken.empty());
    CHECK(sink.stops == 0);
    saver.Begin(MakeStarted(2));
    saver.Poll(1);
    CHECK(host.taken.empty());

    ImageDisk::DiskSaver other(library, host, sink);
    host.entries[4].state = ImageDisk::Readback::Done;
    host.entries[4].pixels = MakePixels(64, 32, 1);
    other.Begin(MakeStarted(4));
    other.Stop();
    CHECK(sink.stops == 1);
    other.Begin(MakeStarted(4));
    other.Poll(0);
    CHECK(host.taken.empty() && sink.jobs.empty());
}

// ---------------------------------------------------------------------------
// The name list

static void TestNameListRoundTrip()
{
    NameList names;
    names.push_back({ "copper", "/game/ui/t_copper.t_copper" });
    names.push_back({ "iron plate", "/game/ui/t_iron.t_iron" });

    const std::vector<uint8_t> file = SerializeNames(names);
    NameList read;
    CHECK(ParseNames(file.data(), file.size(), read) == Parsed::Ok);
    CHECK(read == names);

    const std::vector<uint8_t> none = SerializeNames({});
    NameList empty = names;
    CHECK(ParseNames(none.data(), none.size(), empty) == Parsed::Ok);
    CHECK(empty.empty());
}

static void TestNameListBad()
{
    NameList names;
    names.push_back({ "copper", "/game/ui/t_copper.t_copper" });
    const std::vector<uint8_t> file = SerializeNames(names);
    NameList read;

    std::vector<uint8_t> magic = file;
    magic[0] ^= 1;
    CHECK(ParseNames(magic.data(), magic.size(), read) == Parsed::BadMagic);

    std::vector<uint8_t> revision = file;
    Put32At(revision, 4, kRevision + 1);
    CHECK(ParseNames(revision.data(), revision.size(), read) == Parsed::WrongRevision);

    CHECK(ParseNames(file.data(), 8, read) == Parsed::TooShort);
    CHECK(ParseNames(file.data(), file.size() - 1, read) == Parsed::BadCrc);

    std::vector<uint8_t> flipped = file;
    flipped[flipped.size() - 8] ^= 1;
    CHECK(ParseNames(flipped.data(), flipped.size(), read) == Parsed::BadCrc);

    // A set list is not a name list.
    const std::vector<uint8_t> sets = SerializeSets({ { 1, { "/Game/A.A" } } });
    CHECK(ParseNames(sets.data(), sets.size(), read) == Parsed::BadMagic);
}

// ---------------------------------------------------------------------------
// The reader, on a real folder and a fake uploader

class FakeUploader : public Uploader
{
public:
    bool fail = false;
    int  calls = 0;
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<uint8_t> pixels;

    bool Upload(uint32_t w, uint32_t h, uint32_t l, const uint8_t* data, size_t bytes, PluginTextureHandle& texture) override
    {
        ++calls;
        width = w; height = h; levels = l;
        pixels.assign(data, data + bytes);
        if (fail)
            return false;
        texture = reinterpret_cast<PluginTextureHandle>(static_cast<uintptr_t>(0x1000 + calls * 16));
        return true;
    }
};

static void WriteFileIn(const TempFolder& temp, const std::string& name, const std::vector<uint8_t>& bytes)
{
    CreateDirectoryW(temp.path.c_str(), nullptr);
    const std::wstring path = temp.path + L"\\" + std::wstring(name.begin(), name.end());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(h);
    }
}

static std::string KeyOf(const Info& info)
{
    return ImageLibrary::NormalizeObjectPath(info.path.c_str());
}

// The reader on the folder, polled until it has listed it.
static bool Open(Reader& reader)
{
    uint64_t now = 0;
    return WaitFor([&] { return reader.Poll(now += 10) != ImageDisk::Source::State::Loading; });
}

static std::vector<ImageDisk::FileResult> Results(Reader& reader, size_t count)
{
    std::vector<ImageDisk::FileResult> got;
    WaitFor([&] { reader.Collect(got); return got.size() >= count; });
    return got;
}

static Reader::Session SessionOf(const TempFolder& temp)
{
    const std::wstring folder = temp.path;
    return [folder] { return Reader::FolderFn([folder] { return folder; }); };
}

static void TestReaderListsTheFolder()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> pixels;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info, &pixels));
    WriteFileIn(temp, "notes.txt", { 1, 2, 3 });
    WriteFileIn(temp, "0123.img", { 1, 2, 3 });   // not a name this part writes

    NameList names;
    names.push_back({ "iron plate", KeyOf(info) });
    WriteFileIn(temp, NameListFileName(), SerializeNames(names));
    SetList sets;
    sets.push_back({ PLUGIN_IMAGE_SET_ITEMS, { KeyOf(info) } });
    WriteFileIn(temp, SetListFileName(), SerializeSets(sets));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(reader.Poll(0) == ImageDisk::Source::State::Loading);
    CHECK(!reader.Has(KeyOf(info)));   // not before the listing is in
    CHECK(Open(reader));
    CHECK(reader.Poll(1) == ImageDisk::Source::State::Ready);
    CHECK(reader.Has(KeyOf(info)));
    CHECK(!reader.Has("/game/ui/t_other.t_other"));

    ImageDisk::Lists lists;
    CHECK(reader.TakeLists(lists));
    CHECK(lists.names == names);
    CHECK(lists.sets == sets);
    CHECK(!reader.TakeLists(lists));   // once
    reader.Stop();
}

static void TestReaderBadListsAreEmpty()
{
    TempFolder temp;
    WriteFileIn(temp, NameListFileName(), { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 });
    WriteFileIn(temp, SetListFileName(), { 1 });

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    ImageDisk::Lists lists;
    CHECK(reader.TakeLists(lists));
    CHECK(lists.names.empty() && lists.sets.empty());
    reader.Stop();
}

static void TestReaderMissingFolderIsEmpty()
{
    TempFolder temp;   // never made
    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Poll(1) == ImageDisk::Source::State::Ready);
    CHECK(!reader.Has("/game/ui/t_icon.t_icon"));
    reader.Stop();
}

static void TestReaderGoodFileBecomesATexture()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> pixels;
    const std::vector<uint8_t> file = Valid(&info, &pixels);
    WriteFileIn(temp, FileName(KeyOf(info)), file);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(7, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1);
    if (got.size() == 1)
    {
        CHECK(got[0].id == 7);
        CHECK(got[0].outcome == ImageDisk::FileOutcome::Texture);
        CHECK(got[0].texture != nullptr);
        CHECK(got[0].bytes == pixels.size());
    }
    CHECK(uploader.calls == 1);
    CHECK(uploader.width == info.storedWidth && uploader.height == info.storedHeight && uploader.levels == info.levels);
    CHECK(uploader.pixels == pixels);   // the file's bytes, every level
    CHECK(reader.Has(KeyOf(info)));     // still listed
    CHECK(reader.Rejected() == 0);
    CHECK(temp.Files().size() == 1);
    reader.Stop();
}

// The file's last-write time in seconds before now (0 when it cannot be read).
static long long AgeSeconds(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = now.dwLowDateTime;            a.HighPart = now.dwHighDateTime;
    b.LowPart = data.ftLastWriteTime.dwLowDateTime; b.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return static_cast<long long>((a.QuadPart - b.QuadPart) / 10000000ull);
}

static void SetAgeSeconds(const std::wstring& path, long long seconds)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= static_cast<unsigned long long>(seconds) * 10000000ull;
    FILETIME then;
    then.dwLowDateTime  = t.LowPart;
    then.dwHighDateTime = t.HighPart;
    SetFileTime(h, nullptr, nullptr, &then);
    CloseHandle(h);
}

static void TestReaderTouchesAFileItServes()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> pixels;
    const std::vector<uint8_t> file = Valid(&info, &pixels);
    const std::string name = FileName(KeyOf(info));
    WriteFileIn(temp, name, file);
    const std::wstring path = temp.path + L"\\" + std::wstring(name.begin(), name.end());
    SetAgeSeconds(path, 3600);
    CHECK(AgeSeconds(path) >= 3500);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(AgeSeconds(path) >= 3500);   // listing the folder uses nothing
    CHECK(reader.Request(7, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Texture);
    // Served from disk: the size cap now counts it as used just now (the touch is done before the result is out).
    CHECK(AgeSeconds(path) < 60);
    reader.Stop();
}

static void TestReaderDoesNotTouchAFileItCouldNotUse()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> pixels;
    const std::vector<uint8_t> file = Valid(&info, &pixels);
    const std::string name = FileName(KeyOf(info));
    WriteFileIn(temp, name, file);
    const std::wstring path = temp.path + L"\\" + std::wstring(name.begin(), name.end());
    SetAgeSeconds(path, 3600);

    FakeUploader uploader;
    uploader.fail = true;   // kept, but not served
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(7, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(AgeSeconds(path) >= 3500);
    reader.Stop();
}

static void TestReaderCorruptFileIsRejectedAndDeleted()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> file = Valid(&info);
    file[file.size() / 2] ^= 0x40;
    const std::string name = FileName(KeyOf(info));
    WriteFileIn(temp, name, file);

    LogCollector log;
    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, log.Sink());
    CHECK(Open(reader));
    CHECK(reader.Has(KeyOf(info)));
    CHECK(reader.Request(3, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1);
    if (got.size() == 1)
    {
        CHECK(got[0].id == 3);
        CHECK(got[0].outcome == ImageDisk::FileOutcome::Rejected);
        CHECK(got[0].texture == nullptr);
    }
    CHECK(uploader.calls == 0);
    CHECK(reader.Rejected() == 1);
    CHECK(!reader.Has(KeyOf(info)));    // the game is read for it now
    CHECK(temp.Files().empty());        // and the file is gone, so it is saved again
    CHECK(log.Count() == 1);
    reader.Stop();
}

static void TestReaderRefusesAFileOfAnotherImage()
{
    TempFolder temp;
    Info info;
    const std::vector<uint8_t> file = Valid(&info);
    // The file for one path sits under the name of another.
    const std::string wanted = "/game/ui/t_wanted.t_wanted";
    WriteFileIn(temp, FileName(wanted), file);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(4, wanted));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Rejected);
    CHECK(uploader.calls == 0);
    CHECK(temp.Files().empty());
    reader.Stop();
}

static void TestReaderRefusesAFileOfTheWrongSize()
{
    TempFolder temp;
    Info info = MakeInfo(ImageLibrary::kStoredSize * 2, 32);
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 5);
    const std::vector<uint8_t> file = Serialize(info, pixels.data(), pixels.size());
    CHECK(!file.empty());
    WriteFileIn(temp, FileName(KeyOf(info)), file);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(5, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Rejected);
    CHECK(uploader.calls == 0);
    reader.Stop();
}

static void TestReaderUploadFailureKeepsTheFile()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    uploader.fail = true;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(6, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(uploader.calls == 1);
    CHECK(reader.Rejected() == 0);
    CHECK(!reader.Has(KeyOf(info)));    // the game is read for it this session
    CHECK(temp.Files().size() == 1);    // the file itself is fine
    reader.Stop();
}

static void TestReaderFileGoneAfterTheListingFails()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    const std::string name = FileName(KeyOf(info));   // one string: begin() and end() of two temporaries are not a range
    DeleteFileW((temp.path + L"\\" + std::wstring(name.begin(), name.end())).c_str());
    CHECK(reader.Request(8, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(reader.Rejected() == 0);
    CHECK(!reader.Has(KeyOf(info)));
    reader.Stop();
}

static void TestReaderOffSession()
{
    LogCollector log;
    FakeUploader uploader;
    Reader reader([] { return Reader::FolderFn(); }, uploader, log.Sink());
    CHECK(reader.Poll(0) == ImageDisk::Source::State::Off);
    CHECK(!reader.Has("/game/ui/t_a.t_a"));
    CHECK(!reader.Request(1, "/game/ui/t_a.t_a"));
    CHECK(reader.Poll(1) == ImageDisk::Source::State::Off);
    CHECK(log.Count() == 1);   // said once
    reader.Stop();
}

static void TestReaderSessionEndingTurnsItOff()
{
    TempFolder temp;
    bool on = true;
    FakeUploader uploader;
    Reader reader([&] { return on ? SessionOf(temp)() : Reader::FolderFn(); }, uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Poll(1) == ImageDisk::Source::State::Ready);
    on = false;
    CHECK(reader.Poll(2) == ImageDisk::Source::State::Off);
    CHECK(!reader.Request(1, "/game/ui/t_a.t_a"));
    reader.Stop();
}

static void TestReaderAbandonDropsResults()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(9, KeyOf(info)));
    reader.Stop();   // the job in hand finishes
    reader.Abandon();
    std::vector<ImageDisk::FileResult> got;
    reader.Collect(got);
    CHECK(got.empty());
    CHECK(!reader.Request(10, KeyOf(info)));   // nothing is read after stop
}

static void TestReaderStopKeepsFinishedResults()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Request(11, KeyOf(info)));
    WaitFor([&] { return uploader.calls == 1; });
    reader.Stop();
    std::vector<ImageDisk::FileResult> got;
    reader.Collect(got);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Texture);   // left for the pump to free
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The key of the pak set, the session that holds it, and run-time objects

static void TestPakHashCoversWhatDecidesPixels()
{
    PakEntry a;
    a.path = "C:/Game/Content/Paks/~mods/a.pak";
    a.mountPoint = "../../../StarRupture/Content/";
    a.order = 3;
    a.numFiles = 10;
    a.pak = { 1000, 777 };
    PakEntry b = a;
    b.path = "C:/Game/Content/Paks/~mods/b.pak";
    b.order = 4;
    const uint64_t hash = HashPakSet({ a, b });
    CHECK(HashPakSet({ b, a }) == hash);

    // The same name with other contents: another size, another time, other .utoc/.ucas.
    PakEntry grown = a;
    grown.pak.size += 1;
    CHECK(HashPakSet({ grown, b }) != hash);
    PakEntry touched = a;
    touched.pak.writeTime += 1;
    CHECK(HashPakSet({ touched, b }) != hash);
    PakEntry toc = a;
    toc.utoc = { 5, 5 };
    CHECK(HashPakSet({ toc, b }) != hash);
    PakEntry cas = a;
    cas.ucas = { 5, 5 };
    CHECK(HashPakSet({ cas, b }) != hash);
    PakEntry files = a;
    files.numFiles += 1;
    CHECK(HashPakSet({ files, b }) != hash);

    // Two paks that swap priority.
    PakEntry swappedA = a, swappedB = b;
    swappedA.order = b.order;
    swappedB.order = a.order;
    CHECK(HashPakSet({ swappedA, swappedB }) != hash);

    // The size and the time do not run into each other.
    PakEntry x = a, y = a;
    x.pak = { 1, 2 };
    y.pak = { 2, 1 };
    CHECK(HashPakSet({ x }) != HashPakSet({ y }));
}

static void TestTransientPath()
{
    CHECK(IsTransientPath("/engine/transient.texture2d_12"));
    CHECK(IsTransientPath("/engine/transient.texture2d_0"));
    CHECK(IsTransientPath("/Engine/Transient.Texture2D_7"));
    CHECK(IsTransientPath("/memory/untitled_1.texture"));
    CHECK(!IsTransientPath("/game/ui/t_icon.t_icon"));
    CHECK(!IsTransientPath("/engine/engineresources/whitesquaretexture.whitesquaretexture"));
    CHECK(!IsTransientPath("/engine/transients/t_x.t_x"));
    CHECK(!IsTransientPath("/game/memory/t_x.t_x"));
    CHECK(!IsTransientPath(""));
}

static void TestListSerializersKeepWhatParses()
{
    // One entry the parser would refuse must not make the whole list unreadable.
    SetList sets;
    sets.push_back({ PLUGIN_IMAGE_SET_ITEMS, { "/game/a.a", "", std::string(kMaxPathLength + 1, 'x'), "/game/b.b" } });
    SetList read;
    const std::vector<uint8_t> setFile = SerializeSets(sets);
    CHECK(ParseSets(setFile.data(), setFile.size(), read) == Parsed::Ok);
    CHECK(read.size() == 1 && read[0].paths == (std::vector<std::string>{ "/game/a.a", "/game/b.b" }));

    NameList names;
    names.push_back({ "iron plate", "/game/a.a" });
    names.push_back({ "", "/game/b.b" });
    names.push_back({ "copper", std::string(kMaxPathLength + 1, 'x') });
    NameList readNames;
    const std::vector<uint8_t> nameFile = SerializeNames(names);
    CHECK(ParseNames(nameFile.data(), nameFile.size(), readNames) == Parsed::Ok);
    CHECK(readNames.size() == 1 && readNames[0].name == "iron plate");
}

// A pak list the test changes, and file stamps it controls.
struct FakePaks
{
    std::vector<PakEntry> paks;
    bool listable = true;
    bool pakMissing = false;
    uint64_t size = 100;
    int listCalls = 0;
    std::atomic<int> stampCalls{ 0 };
    LogCollector log;

    ImageDisk::PakSession Session(uint64_t recheckMs = 250)
    {
        return ImageDisk::PakSession(
            [this](std::vector<PakEntry>& out) { ++listCalls; out = paks; return listable; },
            [this](const std::string& path, FileStamp& stamp)
            {
                ++stampCalls;
                if (pakMissing)
                    return false;
                if (path.size() < 4 || path.compare(path.size() - 4, 4, ".pak") != 0)
                    return false;   // no .utoc or .ucas
                stamp = { size, 9 };
                return true;
            },
            [](uint64_t hash) { return L"root\\" + std::to_wstring(hash); },
            log.Sink(), recheckMs);
    }
};

static std::vector<PakEntry> TwoPaks()
{
    PakEntry a;
    a.path = "C:/Game/Paks/a.pak";
    a.mountPoint = "../../../Game/";
    a.order = 1;
    PakEntry b = a;
    b.path = "C:/Game/Paks/b.pak";
    b.order = 2;
    return { a, b };
}

static void TestPakSessionUnlistableIsOff()
{
    FakePaks fake;
    fake.paks = TwoPaks();
    fake.listable = false;
    ImageDisk::PakSession session = fake.Session();
    CHECK(!session.Folder(0, false));
    CHECK(session.CurrentFolder().empty());
    CHECK(!session.Folder(1000, true));
    CHECK(fake.log.Count() == 1);   // one line, then quiet
    CHECK(fake.listCalls == 1);     // and no key from a part of the set, ever

    // An empty list is no list either.
    FakePaks none;
    ImageDisk::PakSession empty = none.Session();
    CHECK(!empty.Folder(0, false));
    CHECK(none.log.Count() == 1);
}

static void TestPakSessionPakFileThatCannotBeReadIsOff()
{
    FakePaks fake;
    fake.paks = TwoPaks();
    fake.pakMissing = true;
    ImageDisk::PakSession session = fake.Session();
    const std::function<std::wstring()> folder = session.Folder(0, false);
    CHECK(static_cast<bool>(folder));
    CHECK(fake.stampCalls == 0);        // the game thread reads no file
    CHECK(folder().empty());            // the worker cannot key it
    CHECK(fake.log.Count() == 1);
    CHECK(folder().empty());
    CHECK(session.CurrentFolder().empty());
    CHECK(!session.Folder(1, true));
    CHECK(fake.log.Count() == 1);
}

static void TestPakSessionKeyFollowsTheFiles()
{
    FakePaks one;
    one.paks = TwoPaks();
    ImageDisk::PakSession first = one.Session();
    const std::function<std::wstring()> folder = first.Folder(0, false);
    CHECK(static_cast<bool>(folder));
    CHECK(one.stampCalls == 0);         // taken on the worker
    const std::wstring name = folder();
    CHECK(!name.empty() && name.rfind(L"root\\", 0) == 0);
    const int stamps = one.stampCalls;
    CHECK(stamps > 0);
    CHECK(folder() == name);            // read once
    CHECK(one.stampCalls == stamps);
    CHECK(first.CurrentFolder() == name);

    // The same names with another pak size: another folder.
    FakePaks two;
    two.paks = TwoPaks();
    two.size = 101;
    ImageDisk::PakSession second = two.Session();
    CHECK(second.Folder(0, false)() != name);

    // The same paks with their priorities swapped: another folder.
    FakePaks three;
    three.paks = TwoPaks();
    std::swap(three.paks[0].order, three.paks[1].order);
    ImageDisk::PakSession third = three.Session();
    CHECK(third.Folder(0, false)() != name);

    // The same set: the same folder.
    FakePaks four;
    four.paks = TwoPaks();
    ImageDisk::PakSession fourth = four.Session();
    CHECK(fourth.Folder(0, false)() == name);
    CHECK(one.log.Count() == 0);
}

static void TestPakSessionJobUnderAnOldKeyIsNotFiled()
{
    FakePaks fake;
    fake.paks = TwoPaks();
    ImageDisk::PakSession session = fake.Session();
    const std::function<std::wstring()> before = session.Folder(0, false);
    CHECK(!before().empty());

    // Inside the interval the set is not listed again...
    const int calls = fake.listCalls;
    CHECK(static_cast<bool>(session.Folder(100, false)));
    CHECK(fake.listCalls == calls);

    // ...but a caller that files an image asks for a look every time. A pak mounted just now:
    PakEntry added = fake.paks[0];
    added.path = "C:/Game/Paks/c.pak";
    added.order = 3;
    fake.paks.push_back(added);
    CHECK(!session.Folder(101, true));
    CHECK(fake.log.Count() == 1);

    // The job made earlier gives no folder when the worker reaches it, and nothing else does.
    CHECK(before().empty());
    CHECK(session.CurrentFolder().empty());
    CHECK(!session.Folder(102, false));
    CHECK(fake.log.Count() == 1);
}

static void TestPakSessionLooksAgainOnTheInterval()
{
    FakePaks fake;
    fake.paks = TwoPaks();
    ImageDisk::PakSession session = fake.Session(250);
    CHECK(static_cast<bool>(session.Folder(0, false)));
    fake.paks.pop_back();
    CHECK(static_cast<bool>(session.Folder(249, false)));    // not looked at yet
    CHECK(!session.Folder(250, false));                       // looked at: the set changed
    CHECK(fake.log.Count() == 1);

    // A set that cannot be listed any more cannot be vouched for either.
    FakePaks lost;
    lost.paks = TwoPaks();
    ImageDisk::PakSession other = lost.Session(250);
    CHECK(static_cast<bool>(other.Folder(0, false)));
    lost.listable = false;
    CHECK(!other.Folder(0, true));
    CHECK(lost.log.Count() == 1);
}

static void TestWriterBusyFileDoesNotSwitchOff()
{
    // A read-only file in the way of the rename: this one file is skipped, saving goes on.
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    const std::wstring name = temp.path + L"\\ro.img";
    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);
    DWORD wrote = 0;
    WriteFile(h, "ab", 2, &wrote, nullptr);
    CloseHandle(h);

    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "ro.img", { 1, 2, 3 })));
    CHECK(writer.Submit(BytesJob(temp.path, "next.img", { 7, 7 })));
    CHECK(WaitFor([&] { return temp.Read(L"next.img").size() == 2; }));
    CHECK(!writer.Off());
    CHECK(log.Count() == 0);
    CHECK(writer.Submit(BytesJob(temp.path, "more.img", { 8 })));
    CHECK(WaitFor([&] { return temp.Read(L"more.img").size() == 1; }));
    writer.Stop();

    // The temp file is not left behind, and the file in the way is as it was.
    CHECK(temp.Files().size() == 3);
    CHECK(temp.Read(L"ro.img").size() == 2);
    SetFileAttributesW(name.c_str(), FILE_ATTRIBUTE_NORMAL);
}

static void TestWriterFileHeldByAnotherHandleIsRetried()
{
    // Something (a scanner, a reader without share-delete) holds the file for a moment.
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    const std::wstring name = temp.path + L"\\held.img";
    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    WriteFile(h, "ab", 2, &wrote, nullptr);

    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "held.img", { 1, 2, 3 })));
    // Let it fail once, then let go: the later tries write the file.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CloseHandle(h);
    CHECK(WaitFor([&] { return temp.Read(L"held.img").size() == 3; }));
    CHECK(!writer.Off());
    CHECK(log.Count() == 0);
    writer.Stop();
    CHECK(temp.Files().size() == 1);
}

static void TestWriterFolderClosedToNewFilesSwitchesOff()
{
    // The file stays busy and a new file cannot be made either (a directory has the probe's name):
    // now it is the folder that is closed.
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    CreateDirectoryW((temp.path + L"\\probe." + std::to_wstring(GetCurrentProcessId()) + L".tmp").c_str(), nullptr);
    const std::wstring name = temp.path + L"\\ro.img";
    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);
    DWORD wrote = 0;
    WriteFile(h, "ab", 2, &wrote, nullptr);
    CloseHandle(h);

    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "ro.img", { 1, 2, 3 })));
    CHECK(WaitFor([&] { return writer.Off(); }));
    CHECK(log.Count() == 1);
    writer.Stop();
    SetFileAttributesW(name.c_str(), FILE_ATTRIBUTE_NORMAL);
}

static void TestWriterDropsAJobWithNoFolder()
{
    // The folder function gives nothing once the job's key is no longer the session's.
    TempFolder temp;
    LogCollector log;
    Writer writer(log.Sink());
    std::atomic<bool> made{ false };
    Job stale;
    stale.folder = [] { return std::wstring(); };
    stale.name   = "stale.img";
    stale.make   = [&made] { made = true; return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(stale)));
    CHECK(writer.Submit(BytesJob(temp.path, "marker.img", { 1 })));
    CHECK(WaitFor([&] { return temp.Read(L"marker.img").size() == 1; }));
    writer.Stop();
    CHECK(!made.load());
    CHECK(temp.Files().size() == 1);
    CHECK(!writer.Off());
}

static void TestWriterKeepsAQueuedClearWhenItSwitchesOff()
{
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    {
        HANDLE h = CreateFileW((temp.path + L"\\blocked").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CloseHandle(h);
    }

    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };
    std::atomic<bool> cleared{ false };
    Writer writer(log.Sink());

    Job hold;
    hold.folder = [&temp] { return temp.path; };
    hold.name   = "hold.img";
    hold.make   = [&] { started = true; WaitFor([&] { return release.load(); }); return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(hold)));
    CHECK(WaitFor([&] { return started.load(); }));

    // Behind the held job: a write that will fail at folder level, an ordinary job, and a Clear.
    CHECK(writer.Submit(BytesJob(temp.path + L"\\blocked\\sub", "a.img", { 1 })));
    CHECK(writer.Submit(BytesJob(temp.path, "late.img", { 2 })));
    Job clear;
    clear.folder     = [&temp] { return temp.path; };
    clear.name       = "clear";
    clear.runWhenOff = true;
    clear.make       = [&cleared] { cleared = true; return std::vector<uint8_t>(); };
    CHECK(writer.Submit(std::move(clear)));

    release = true;
    CHECK(WaitFor([&] { return writer.Off(); }));
    CHECK(WaitFor([&] { return cleared.load(); }));
    writer.Stop();
    CHECK(temp.Read(L"late.img").empty());   // dropped, as before
}

// A host whose folder the test controls.
class ControlledHost : public FakeReadbacks
{
public:
    bool folderGone = false;
    uint32_t cleared = 0;
    int folderAsked = 0;

    std::function<std::wstring()> CacheFolder() override
    {
        ++folderAsked;
        if (folderGone)
            return {};
        return [] { return std::wstring(L"cache\\folder"); };
    }
    uint32_t ClearCount() const override { return cleared; }
};

static void TestSaverNothingIsFiledOnceTheKeyIsGone()
{
    ImageLibrary::Library library;
    ControlledHost host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    host.entries[1].state = ImageDisk::Readback::Done;
    host.entries[1].pixels = MakePixels(64, 32, 1);
    host.entries[2].state = ImageDisk::Readback::Done;
    host.entries[2].pixels = MakePixels(64, 32, 2);
    host.entries[3].state = ImageDisk::Readback::Done;
    host.entries[3].pixels = MakePixels(64, 32, 3);

    saver.Begin(MakeStarted(1, "/Game/UI/T_One.T_One"));
    saver.Poll(0);
    CHECK(sink.jobs.size() == 1);

    // A pak is mounted: the folder is asked for as the next image is filed, and the job is not made.
    host.folderGone = true;
    saver.Begin(MakeStarted(2, "/Game/UI/T_Two.T_Two"));
    saver.Poll(1);
    CHECK(sink.jobs.size() == 1);
    CHECK(host.taken.size() == 2 && host.hadDest[1]);

    // From then on a finished readback is only freed.
    host.folderGone = false;
    saver.Begin(MakeStarted(3, "/Game/UI/T_Three.T_Three"));
    saver.Poll(2);
    CHECK(sink.jobs.size() == 1);
    CHECK(host.taken.size() == 3 && !host.hadDest[2]);
}

static void TestSaverLooksAtThePakSetOncePerTick()
{
    ImageLibrary::Library library;
    ControlledHost host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    for (uint32_t ticket = 1; ticket <= 3; ++ticket)
    {
        host.entries[ticket].state = ImageDisk::Readback::Done;
        host.entries[ticket].pixels = MakePixels(64, 32, static_cast<uint8_t>(ticket));
        saver.Begin(MakeStarted(ticket, ticket == 1 ? "/Game/UI/T_One.T_One" : ticket == 2 ? "/Game/UI/T_Two.T_Two" : "/Game/UI/T_Three.T_Three"));
    }

    // Three images finish in one tick: the pak set is looked at once, and all three are filed.
    saver.Poll(0);
    CHECK(sink.jobs.size() == 3);
    CHECK(host.folderAsked == 1);

    // A tick with nothing finished does not look at all; the next image looks again.
    saver.Poll(1);
    CHECK(host.folderAsked == 1);
    host.entries[4].state = ImageDisk::Readback::Done;
    host.entries[4].pixels = MakePixels(64, 32, 4);
    saver.Begin(MakeStarted(4, "/Game/UI/T_Four.T_Four"));
    saver.Poll(2);
    CHECK(sink.jobs.size() == 4);
    CHECK(host.folderAsked == 2);
}

static void TestSaverRunTimeObjectsAreNotSaved()
{
    ImageLibrary::Library library;
    ControlledHost host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    host.entries[1].state = ImageDisk::Readback::Done;
    host.entries[1].pixels = MakePixels(64, 32, 1);
    saver.Begin(MakeStarted(1, "/Engine/Transient.Texture2D_12"));
    saver.Poll(0);
    CHECK(sink.jobs.empty());
    CHECK(host.taken.size() == 1 && !host.hadDest[0]);   // freed, not copied
    CHECK(host.folderAsked == 0);

    // Not in the name list or the set list either.
    saver.Name("some item", "/Engine/Transient.Texture2D_12");
    const PluginImageId transient = library.Path("/Engine/Transient.Texture2D_12");
    library.AddToSets(transient, PLUGIN_IMAGE_SET_ITEMS);
    saver.Poll(ImageDisk::kSetListIntervalMs);
    CHECK(sink.jobs.empty());

    // A name for an ordinary texture still is.
    saver.Name("iron plate", "/Game/UI/T_Iron.T_Iron");
    saver.Poll(ImageDisk::kSetListIntervalMs * 2);
    CHECK(sink.jobs.size() == 1);
    if (!sink.jobs.empty())
    {
        const std::vector<uint8_t> file = sink.jobs[0].make();
        NameList names;
        CHECK(ParseNames(file.data(), file.size(), names) == Parsed::Ok);
        CHECK(names.size() == 1 && names[0].name == "iron plate");
    }
}

static void TestSaverListsAreWrittenAgainAfterAClear()
{
    ImageLibrary::Library library;
    ControlledHost host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    const PluginImageId a = library.Path("/Game/UI/T_A.T_A");
    library.AddToSets(a, PLUGIN_IMAGE_SET_ITEMS);
    saver.Name("iron plate", "/Game/UI/T_A.T_A");
    saver.Poll(ImageDisk::kSetListIntervalMs);
    CHECK(sink.jobs.size() == 2);   // the set list and the name list

    saver.Poll(ImageDisk::kSetListIntervalMs * 2);
    CHECK(sink.jobs.size() == 2);   // unchanged: not again

    host.cleared = 1;               // the cache folder was emptied
    saver.Poll(ImageDisk::kSetListIntervalMs * 3);
    CHECK(sink.jobs.size() == 4);
    saver.Poll(ImageDisk::kSetListIntervalMs * 4);
    CHECK(sink.jobs.size() == 4);
}

static void TestReaderRunTimeObjectsAreNotServed()
{
    TempFolder temp;
    const std::string key = "/engine/transient.texture2d_12";
    Info info = MakeInfo();
    info.path = "/Engine/Transient.Texture2D_12";
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 5);
    WriteFileIn(temp, FileName(key), Serialize(info, pixels.data(), pixels.size()));

    // The names list and a set also point at it.
    NameList names;
    names.push_back({ "iron plate", "/game/ui/t_icon.t_icon" });
    names.push_back({ "made", key });
    WriteFileIn(temp, NameListFileName(), SerializeNames(names));
    SetList sets;
    sets.push_back({ PLUGIN_IMAGE_SET_ITEMS, { key, "/game/ui/t_icon.t_icon" } });
    WriteFileIn(temp, SetListFileName(), SerializeSets(sets));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(!reader.Has(key));
    const std::string fileName = FileName(key);
    CHECK(temp.Read(std::wstring(fileName.begin(), fileName.end())).empty());   // removed

    ImageDisk::Lists lists;
    CHECK(reader.TakeLists(lists));
    CHECK(lists.names.size() == 1 && lists.names[0].name == "iron plate");
    CHECK(lists.sets.size() == 1 && lists.sets[0].paths == (std::vector<std::string>{ "/game/ui/t_icon.t_icon" }));
    reader.Stop();
}

static void TestReaderRequestForARunTimeObjectIsNotServed()
{
    TempFolder temp;
    const std::string key = "/engine/transient.texture2d_3";
    Info info = MakeInfo();
    info.path = key;
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 5);
    // Listed before the file existed: only a direct request can reach it.
    CreateDirectoryW(temp.path.c_str(), nullptr);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    const std::string name = FileName(key);
    WriteFileIn(temp, name, Serialize(info, pixels.data(), pixels.size()));
    CHECK(reader.Request(9, key));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(uploader.calls == 0);
    CHECK(temp.Files().empty());
    reader.Stop();
}

static void TestReaderBusyFileIsNotDeleted()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));
    const std::string name = FileName(KeyOf(info));
    const std::wstring path = temp.path + L"\\" + std::wstring(name.begin(), name.end());

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));

    // Something holds the file with no sharing: it cannot be opened now, and it is not at fault.
    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(reader.Request(10, KeyOf(info)));
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CloseHandle(held);
    CHECK(reader.Rejected() == 0);
    CHECK(uploader.calls == 0);
    CHECK(temp.Files().size() == 1);
    reader.Stop();
}

static void TestReaderWithNoFolderListsNothing()
{
    FakeUploader uploader;
    Reader reader([] { return Reader::FolderFn([] { return std::wstring(); }); }, uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.Poll(1) == ImageDisk::Source::State::Ready);
    CHECK(!reader.Has("/game/ui/t_icon.t_icon"));
    ImageDisk::Lists lists;
    CHECK(reader.TakeLists(lists));
    CHECK(lists.names.empty() && lists.sets.empty());
    reader.Stop();
}

static void TestReaderExactCopyIsUploadedAtTheAskedSize()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> pixels;
    const std::vector<uint8_t> file = Valid(&info, &pixels);   // stored 64 x 32
    WriteFileIn(temp, FileName(KeyOf(info)), file);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.RequestExact(8, KeyOf(info), 32) == ImageDisk::Source::ExactAnswer::Started);
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1);
    if (got.size() == 1)
    {
        CHECK(got[0].id == 8 && got[0].exactSize == 32);
        CHECK(got[0].outcome == ImageDisk::FileOutcome::Texture);
        CHECK(got[0].texture != nullptr);
        CHECK(got[0].width == 32 && got[0].height == 16);
        CHECK(got[0].bytes == 32u * 16u * 4u);
    }
    CHECK(uploader.calls == 1);
    CHECK(uploader.width == 32 && uploader.height == 16 && uploader.levels == 1);
    CHECK(uploader.pixels.size() == 32u * 16u * 4u);
    CHECK(reader.Rejected() == 0);
    CHECK(temp.Files().size() == 1);   // the file is left as it was
    reader.Stop();
}

static void TestReaderExactCopyNoSmallerThanTheStoredPictureIsSame()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.RequestExact(8, KeyOf(info), 64) == ImageDisk::Source::ExactAnswer::Started);
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].exactSize == 64 && got[0].outcome == ImageDisk::FileOutcome::Same);
    CHECK(uploader.calls == 0);
    reader.Stop();
}

static void TestReaderExactCopyWithNoFileFailsAndDeletesNothing()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.RequestExact(8, "/game/ui/t_other.t_other", 32) == ImageDisk::Source::ExactAnswer::Started);
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].exactSize == 32 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(uploader.calls == 0);
    CHECK(reader.Rejected() == 0);
    CHECK(temp.Files().size() == 1);
    reader.Stop();
}

static void TestReaderExactCopyOfABadFileFailsAndKeepsTheFile()
{
    TempFolder temp;
    Info info;
    std::vector<uint8_t> file = Valid(&info);
    file[file.size() - 1] ^= 0xFF;   // damaged pixels
    file.resize(file.size() - 7);    // and cut short
    WriteFileIn(temp, FileName(KeyOf(info)), file);

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.RequestExact(8, KeyOf(info), 32) == ImageDisk::Source::ExactAnswer::Started);
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].exactSize == 32 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(uploader.calls == 0);
    CHECK(reader.Rejected() == 0);      // the stored copy's own read decides about the file
    CHECK(temp.Files().size() == 1);
    reader.Stop();
}

static void TestReaderExactCopyUploadFailure()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    uploader.fail = true;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    CHECK(reader.RequestExact(8, KeyOf(info), 32) == ImageDisk::Source::ExactAnswer::Started);
    const std::vector<ImageDisk::FileResult> got = Results(reader, 1);
    CHECK(got.size() == 1 && got[0].exactSize == 32 && got[0].outcome == ImageDisk::FileOutcome::Failed);
    CHECK(got[0].texture == nullptr);
    CHECK(uploader.calls == 1);
    CHECK(reader.Rejected() == 0);
    CHECK(temp.Files().size() == 1);
    reader.Stop();
}

static void TestReaderExactCopyOfATransientTextureIsRefusedForGood()
{
    TempFolder temp;
    Info info;
    WriteFileIn(temp, FileName(KeyOf(MakeInfo())), Valid(&info));

    FakeUploader uploader;
    Reader reader(SessionOf(temp), uploader, nullptr);
    CHECK(Open(reader));
    // A run-time texture has no file: no later ask can change that, so the answer is not "busy".
    CHECK(reader.RequestExact(8, "/engine/transient.texture2d_12", 32) == ImageDisk::Source::ExactAnswer::Refused);
    CHECK(reader.RequestExact(8, "/memory/untitled_1.texture", 32) == ImageDisk::Source::ExactAnswer::Refused);
    CHECK(reader.RequestExact(8, KeyOf(info), 0) == ImageDisk::Source::ExactAnswer::Refused);
    CHECK(uploader.calls == 0);
    reader.Stop();
}

int main()
{
    RunTest("crc32", TestCrc);
    RunTest("an image file reads back as it was written", TestRoundTrip);
    RunTest("every shape the shrink makes round-trips", TestRoundTripShapes);
    RunTest("a file that cannot be valid is not written", TestSerializeRefusals);
    RunTest("a file with the wrong magic is refused", TestBadMagic);
    RunTest("a file of another revision is refused", TestWrongRevision);
    RunTest("a truncated file is refused", TestTruncated);
    RunTest("a changed byte fails the crc", TestCrcMismatch);
    RunTest("a header that disagrees with itself is refused", TestBadHeader);
    RunTest("a set list reads back as it was written", TestSetListRoundTrip);
    RunTest("a bad set list is refused", TestSetListBad);
    RunTest("file names", TestFileName);
    RunTest("the folder changes when any input changes", TestFolderKey);
    RunTest("the pak set hash", TestPakHash);
    RunTest("the pak set hash covers what decides the pixels", TestPakHashCoversWhatDecidesPixels);
    RunTest("run-time object paths", TestTransientPath);
    RunTest("the list serializers keep what parses", TestListSerializersKeepWhatParses);
    RunTest("the writer writes files and makes the folders", TestWriterWrites);
    RunTest("the writer leaves a file of the same size alone", TestWriterKeepsSameSize);
    RunTest("a job with no bytes writes nothing", TestWriterEmptyBytesWriteNothing);
    RunTest("a folder that cannot be made switches the disk part off once", TestWriterOffOnUnusableFolder);
    RunTest("a file held by something else is skipped, not the whole disk part", TestWriterBusyFileDoesNotSwitchOff);
    RunTest("a file held for a moment is written on a later try", TestWriterFileHeldByAnotherHandleIsRetried);
    RunTest("a folder closed to new files switches the disk part off once", TestWriterFolderClosedToNewFilesSwitchesOff);
    RunTest("a job whose folder is gone is dropped unwritten", TestWriterDropsAJobWithNoFolder);
    RunTest("a queued Clear survives the switch-off", TestWriterKeepsAQueuedClearWhenItSwitchesOff);
    RunTest("the writer's queue is bounded", TestWriterQueueIsBounded);
    RunTest("the writer refuses a job without bytes or folder", TestWriterRefusesBadJobs);
    RunTest("nothing is written after stop", TestWriterStop);
    RunTest("stop before any job", TestWriterStopBeforeStart);
    RunTest("a finished readback becomes a file", TestSaverWritesFinishedCopy);
    RunTest("no ticket and a lost readback make no file", TestSaverIgnoresWhatIsNotSaved);
    RunTest("with the disk part off a readback is only freed", TestSaverWithDiskOffOnlyFreesTheReadback);
    RunTest("a full queue drops the image", TestSaverQueueFullDropsTheImage);
    RunTest("the set list is written when it changes", TestSaverSetList);
    RunTest("stop and a lost GPU end the saver", TestSaverStopAndAbandon);
    RunTest("the name list is written when it changes", TestSaverNames);
    RunTest("a name list reads back as it was written", TestNameListRoundTrip);
    RunTest("a bad name list is refused", TestNameListBad);
    RunTest("the reader lists the folder and its lists", TestReaderListsTheFolder);
    RunTest("a bad list is left empty", TestReaderBadListsAreEmpty);
    RunTest("a missing folder lists nothing", TestReaderMissingFolderIsEmpty);
    RunTest("a good file becomes a texture of its bytes", TestReaderGoodFileBecomesATexture);
    RunTest("a file served from disk is touched", TestReaderTouchesAFileItServes);
    RunTest("a file that could not be served is not touched", TestReaderDoesNotTouchAFileItCouldNotUse);
    RunTest("a corrupt file is rejected and deleted", TestReaderCorruptFileIsRejectedAndDeleted);
    RunTest("a file of another image is rejected", TestReaderRefusesAFileOfAnotherImage);
    RunTest("a file of the wrong size is rejected", TestReaderRefusesAFileOfTheWrongSize);
    RunTest("an upload that fails keeps the file", TestReaderUploadFailureKeepsTheFile);
    RunTest("a file gone after the listing fails", TestReaderFileGoneAfterTheListingFails);
    RunTest("an off session reads nothing", TestReaderOffSession);
    RunTest("a session that ends turns the reader off", TestReaderSessionEndingTurnsItOff);
    RunTest("abandon drops the results", TestReaderAbandonDropsResults);
    RunTest("stop keeps what was finished", TestReaderStopKeepsFinishedResults);
    RunTest("an unlistable pak set switches the disk part off", TestPakSessionUnlistableIsOff);
    RunTest("a pak file that cannot be read switches the disk part off", TestPakSessionPakFileThatCannotBeReadIsOff);
    RunTest("the folder key follows the pak files", TestPakSessionKeyFollowsTheFiles);
    RunTest("a job under an old key is not filed", TestPakSessionJobUnderAnOldKeyIsNotFiled);
    RunTest("the pak set is looked at again on the interval", TestPakSessionLooksAgainOnTheInterval);
    RunTest("nothing is filed once the key is gone", TestSaverNothingIsFiledOnceTheKeyIsGone);
    RunTest("the pak set is looked at once per tick", TestSaverLooksAtThePakSetOncePerTick);
    RunTest("run-time objects are not saved", TestSaverRunTimeObjectsAreNotSaved);
    RunTest("the lists are written again after a clear", TestSaverListsAreWrittenAgainAfterAClear);
    RunTest("run-time objects are not served and their files go", TestReaderRunTimeObjectsAreNotServed);
    RunTest("a request for a run-time object is not served", TestReaderRequestForARunTimeObjectIsNotServed);
    RunTest("a file held by something else is not deleted", TestReaderBusyFileIsNotDeleted);
    RunTest("a session with no folder lists nothing", TestReaderWithNoFolderListsNothing);

    RunTest("an exact copy is uploaded at the asked size", TestReaderExactCopyIsUploadedAtTheAskedSize);
    RunTest("an exact copy no smaller than the stored picture is the same", TestReaderExactCopyNoSmallerThanTheStoredPictureIsSame);
    RunTest("an exact copy with no file fails and deletes nothing", TestReaderExactCopyWithNoFileFailsAndDeletesNothing);
    RunTest("an exact copy of a bad file fails and keeps the file", TestReaderExactCopyOfABadFileFailsAndKeepsTheFile);
    RunTest("an exact copy whose upload fails ends as failed", TestReaderExactCopyUploadFailure);
    RunTest("an exact copy of a transient texture is refused for good", TestReaderExactCopyOfATransientTextureIsRefusedForGood);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
