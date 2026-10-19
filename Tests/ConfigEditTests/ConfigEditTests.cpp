// ConfigEdit tests. See Tests/README.md.
//
// A console program: no game, no plugin DLLs. The service under test, the
// config manager that feeds it and the callback registry are the loader's own
// source files; test_doubles.cpp replaces only what they call out to.
#include "test_doubles.h"

#include "config/config_manager.h"
#include "config/config_edit.h"
#include "UI/plugin_panel_registry.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal harness
// ---------------------------------------------------------------------------

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

#define CHECK_STR(actual, expected)                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        const std::string a_ = (actual);                                         \
        const std::string e_ = (expected);                                       \
        if (a_ != e_) {                                                          \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n      got      \"%s\"\n      expected \"%s\"\n", \
                   __FILE__, __LINE__, #actual, a_.c_str(), e_.c_str());         \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static const ConfigEntry kEntries[] =
{
    { "Drone", "Speed", ConfigValueType::Float,   "10.0", "Top speed", 0.0f, 100.0f },
    { "Drone", "Name",  ConfigValueType::String,  "abc",  "A label",   0.0f, 0.0f   },
    { "Input", "Boost", ConfigValueType::Keybind, "F5",   "Boost key", 0.0f, 0.0f   },
};
static const ConfigSchema kSchema = { kEntries, 3 };

static IPluginSelf g_selfA = {};
static IPluginSelf g_selfB = {};

static const char* const kPluginA = "ConfigEditTestA";
static const char* const kPluginB = "ConfigEditTestB";

static std::vector<std::string> g_callsA; // "section.key=value" per OnConfigChanged
static std::vector<std::string> g_callsB;

static void OnChangedA(const char* section, const char* key, const char* value)
{
    g_callsA.push_back(std::string(section) + "." + key + "=" + value);
}

static void OnChangedB(const char* section, const char* key, const char* value)
{
    g_callsB.push_back(std::string(section) + "." + key + "=" + value);
}

// A handler that clamps by calling back into ConfigEdit, the case the mutex
// has to be released for.
static void OnChangedClamp(const char* section, const char* key, const char* value)
{
    g_callsA.push_back(std::string(section) + "." + key + "=" + value);
    if (strcmp(key, "Speed") == 0 && atof(value) > 50.0)
        ConfigEdit::SetLive(kPluginA, section, key, "50");
}

// The clamp a real plugin can write: it only has IPluginConfig, so it answers
// the notification with Write*, which updates the table through NoteWritten.
static void OnChangedClampViaWrite(const char* section, const char* key, const char* value)
{
    g_callsA.push_back(std::string(section) + "." + key + "=" + value);
    if (strcmp(key, "Speed") == 0 && atof(value) > 50.0)
        g_selfA.config->WriteString(&g_selfA, section, key, "50");
}

static std::wstring IniPath(const char* plugin)
{
    wchar_t path[MAX_PATH];
    ConfigEdit::GetIniPath(plugin, path, MAX_PATH);
    return path;
}

// What the file says right now, or "<absent>".
static std::string DiskValue(const char* plugin, const char* section, const char* key)
{
    wchar_t wsec[64], wkey[64], out[256];
    swprintf_s(wsec, L"%S", section);
    swprintf_s(wkey, L"%S", key);
    GetPrivateProfileStringW(wsec, wkey, L"<absent>", out, ARRAYSIZE(out), IniPath(plugin).c_str());
    char narrow[256];
    snprintf(narrow, sizeof(narrow), "%ls", out);
    return narrow;
}

static void WriteDisk(const char* plugin, const char* section, const char* key, const char* value)
{
    wchar_t wsec[64], wkey[64], wval[256];
    swprintf_s(wsec, L"%S", section);
    swprintf_s(wkey, L"%S", key);
    swprintf_s(wval, L"%S", value);
    WritePrivateProfileStringW(wsec, wkey, wval, IniPath(plugin).c_str());
}

static std::string Get(const char* plugin, const char* section, const char* key)
{
    char value[256];
    if (!ConfigEdit::Get(plugin, section, key, value, sizeof(value)))
        return "<none>";
    return value;
}

static FILETIME LastWrite(const char* plugin)
{
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    GetFileAttributesExW(IniPath(plugin).c_str(), GetFileExInfoStandard, &info);
    return info.ftLastWriteTime;
}

// Puts the file's modified time well in the past, so a later write shows up as
// the time moving.
static void BackdateFile(const char* plugin)
{
    HANDLE h = CreateFileW(IniPath(plugin).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    FILETIME old = {};
    old.dwHighDateTime = 0x01D00000; // 2014
    SetFileTime(h, nullptr, nullptr, &old);
    CloseHandle(h);
}

static bool SameTime(const FILETIME& a, const FILETIME& b)
{
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// A fresh INI for plugin A from the schema (what PluginInit does), plus the
// Block flag the config page writes beside a keybind, then the table loaded.
static void FreshPluginA()
{
    DeleteFileW(IniPath(kPluginA).c_str());
    g_selfA.config->InitializeFromSchema(&g_selfA, &kSchema);
    WriteDisk(kPluginA, "Input", "BoostBlocking", "1");

    TestDoubles::ResetRecordedCalls();
    g_callsA.clear();
    g_callsB.clear();
    ConfigEdit::Load(kPluginA);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// SetLive changes the value and notifies, with no disk write. Commit writes it.
// A second Commit with nothing changed does not touch the file.
static void Test_LiveCommitAndNoopWrite()
{
    FreshPluginA();
    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfA, OnChangedA);

    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "10.000000");
    const unsigned before = ConfigEdit::ChangeCount(kPluginA);

    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "20");
    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "20");
    CHECK_STR(DiskValue(kPluginA, "Drone", "Speed"), "10.000000"); // live: not on disk yet
    CHECK(g_callsA.size() == 1);
    CHECK(!g_callsA.empty() && g_callsA[0] == "Drone.Speed=20");
    CHECK(ConfigEdit::ChangeCount(kPluginA) != before);

    ConfigEdit::Commit(kPluginA, "Drone", "Speed");
    CHECK_STR(DiskValue(kPluginA, "Drone", "Speed"), "20");
    CHECK(g_callsA.size() == 1); // Commit does not notify

    // Nothing changed since: the file must not be rewritten.
    BackdateFile(kPluginA);
    const FILETIME backdated = LastWrite(kPluginA);
    ConfigEdit::Commit(kPluginA, "Drone", "Speed");
    CHECK(SameTime(LastWrite(kPluginA), backdated));

    // A real change does write it.
    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "25");
    ConfigEdit::Commit(kPluginA, "Drone", "Speed");
    CHECK(!SameTime(LastWrite(kPluginA), backdated));
    CHECK_STR(DiskValue(kPluginA, "Drone", "Speed"), "25");

    // A value dragged and put back where it started is also not a write.
    BackdateFile(kPluginA);
    const FILETIME backdated2 = LastWrite(kPluginA);
    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "30");
    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "25");
    ConfigEdit::Commit(kPluginA, "Drone", "Speed");
    CHECK(SameTime(LastWrite(kPluginA), backdated2));

    // A key the INI never had is added by SetLive and written by Commit.
    ConfigEdit::SetLive(kPluginA, "Drone", "Extra", "7");
    CHECK_STR(DiskValue(kPluginA, "Drone", "Extra"), "<absent>");
    ConfigEdit::Commit(kPluginA, "Drone", "Extra");
    CHECK_STR(DiskValue(kPluginA, "Drone", "Extra"), "7");

    UI::PluginPanelRegistry::UnregisterOnConfigChanged(&g_selfA, OnChangedA);
}

// A handler that answers a change by setting the value again (a clamp) must not
// deadlock: the service does not hold its lock while it calls handlers.
static void Test_HandlerMayCallBack()
{
    FreshPluginA();
    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfA, OnChangedClamp);

    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "80");
    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "50");
    CHECK(g_callsA.size() == 2); // the 80, then the clamp's own 50

    UI::PluginPanelRegistry::UnregisterOnConfigChanged(&g_selfA, OnChangedClamp);
}

// Committing a different Keybind value moves the registration and the blocking
// state from the old combo to the new one. Committing it unchanged does not.
static void Test_KeybindRebindOnCommit()
{
    FreshPluginA();
    const std::string owner = std::string(kPluginA) + "|Input|Boost";

    // Load applied the Block flag the INI had.
    CHECK(TestDoubles::blockingCalls.size() == 1);
    if (TestDoubles::blockingCalls.size() == 1)
    {
        CHECK_STR(TestDoubles::blockingCalls[0].owner, owner);
        CHECK_STR(TestDoubles::blockingCalls[0].combo, "F5");
        CHECK(TestDoubles::blockingCalls[0].blocking);
    }
    CHECK(ConfigEdit::GetBlocking(kPluginA, "Input", "Boost"));

    TestDoubles::ResetRecordedCalls();
    ConfigEdit::SetLive(kPluginA, "Input", "Boost", "Ctrl+F6");
    CHECK(TestDoubles::rebindCalls.empty()); // not until Commit
    ConfigEdit::Commit(kPluginA, "Input", "Boost");

    CHECK_STR(DiskValue(kPluginA, "Input", "Boost"), "Ctrl+F6");
    CHECK(TestDoubles::rebindCalls.size() == 1);
    if (TestDoubles::rebindCalls.size() == 1)
    {
        CHECK_STR(TestDoubles::rebindCalls[0].plugin, kPluginA);
        CHECK_STR(TestDoubles::rebindCalls[0].oldCombo, "F5");
        CHECK_STR(TestDoubles::rebindCalls[0].newCombo, "Ctrl+F6");
    }
    CHECK(TestDoubles::blockingCalls.size() == 2);
    if (TestDoubles::blockingCalls.size() == 2)
    {
        CHECK_STR(TestDoubles::blockingCalls[0].combo, "F5");       // old combo released
        CHECK(!TestDoubles::blockingCalls[0].blocking);
        CHECK_STR(TestDoubles::blockingCalls[1].combo, "Ctrl+F6");  // new combo claimed
        CHECK(TestDoubles::blockingCalls[1].blocking);              // with the old Block state
        CHECK_STR(TestDoubles::blockingCalls[1].owner, owner);
    }

    // Same value again: no write, so no rebind.
    TestDoubles::ResetRecordedCalls();
    ConfigEdit::Commit(kPluginA, "Input", "Boost");
    CHECK(TestDoubles::rebindCalls.empty());
    CHECK(TestDoubles::blockingCalls.empty());

    // A non-keybind commit never touches the registry.
    ConfigEdit::SetLive(kPluginA, "Drone", "Name", "xyz");
    ConfigEdit::Commit(kPluginA, "Drone", "Name");
    CHECK(TestDoubles::rebindCalls.empty());
    CHECK(TestDoubles::blockingCalls.empty());

    // The Block toggle writes the INI at once and tells the registry.
    ConfigEdit::SetBlocking(kPluginA, "Input", "Boost", false);
    CHECK_STR(DiskValue(kPluginA, "Input", "BoostBlocking"), "0");
    CHECK(!ConfigEdit::GetBlocking(kPluginA, "Input", "Boost"));
    CHECK(TestDoubles::blockingCalls.size() == 1);
    if (TestDoubles::blockingCalls.size() == 1)
    {
        CHECK_STR(TestDoubles::blockingCalls[0].combo, "Ctrl+F6");
        CHECK(!TestDoubles::blockingCalls[0].blocking);
    }
}

// A plugin's own IPluginConfig::Write* shows up through Get and Snapshot, bumps
// the change counter, and does not fire OnConfigChanged.
static void Test_PluginWriteVisibleThroughGet()
{
    FreshPluginA();
    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfA, OnChangedA);

    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "10.000000");
    unsigned count = ConfigEdit::ChangeCount(kPluginA);

    CHECK(g_selfA.config->WriteFloat(&g_selfA, "Drone", "Speed", 33.5f));
    CHECK_STR(DiskValue(kPluginA, "Drone", "Speed"), "33.500000");
    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "33.500000");
    CHECK(ConfigEdit::ChangeCount(kPluginA) != count);

    CHECK(g_selfA.config->WriteInt(&g_selfA, "Drone", "Name", 42));
    CHECK_STR(Get(kPluginA, "Drone", "Name"), "42");

    CHECK(g_selfA.config->WriteBool(&g_selfA, "Input", "BoostBlocking", false));
    CHECK(!ConfigEdit::GetBlocking(kPluginA, "Input", "Boost"));

    // Writing the value that is already there is not a change.
    count = ConfigEdit::ChangeCount(kPluginA);
    CHECK(g_selfA.config->WriteInt(&g_selfA, "Drone", "Name", 42));
    CHECK(ConfigEdit::ChangeCount(kPluginA) == count);

    // A key the INI did not have appears at the end of its section.
    CHECK(g_selfA.config->WriteString(&g_selfA, "Drone", "NewKey", "hello"));
    CHECK_STR(Get(kPluginA, "Drone", "NewKey"), "hello");
    std::vector<ConfigEdit::Entry> snap;
    ConfigEdit::Snapshot(kPluginA, snap);
    int newKeyAt = -1, lastDroneAt = -1, firstInputAt = -1;
    for (int i = 0; i < static_cast<int>(snap.size()); ++i)
    {
        if (strcmp(snap[i].key, "NewKey") == 0) newKeyAt = i;
        if (strcmp(snap[i].section, "Drone") == 0) lastDroneAt = i;
        if (strcmp(snap[i].section, "Input") == 0 && firstInputAt < 0) firstInputAt = i;
    }
    CHECK(newKeyAt >= 0 && newKeyAt == lastDroneAt && newKeyAt < firstInputAt);

    // The writer has applied its own change: nobody is notified.
    CHECK(g_callsA.empty());

    // A plugin nobody opened has no table to update, and loading it later reads
    // the file, which has the write.
    DeleteFileW(IniPath(kPluginB).c_str());
    CHECK(g_selfB.config->WriteString(&g_selfB, "Sec", "K", "v"));
    CHECK(ConfigEdit::ChangeCount(kPluginB) == 0);
    CHECK_STR(Get(kPluginB, "Sec", "K"), "v");
    DeleteFileW(IniPath(kPluginB).c_str());

    UI::PluginPanelRegistry::UnregisterOnConfigChanged(&g_selfA, OnChangedA);
}

// A handler that clamps the value it was just told about, through the
// IPluginConfig a plugin really has. Commit persists the table, so it is the
// clamped value that reaches the file, not the one the page tried to set.
static void Test_ClampedValueIsPersisted()
{
    FreshPluginA();
    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfA, OnChangedClampViaWrite);

    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "80");
    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "50");
    CHECK_STR(DiskValue(kPluginA, "Drone", "Speed"), "50"); // the plugin's own write

    ConfigEdit::Commit(kPluginA, "Drone", "Speed");
    CHECK_STR(DiskValue(kPluginA, "Drone", "Speed"), "50");
    CHECK(g_callsA.size() == 1); // the plugin's write does not notify again

    UI::PluginPanelRegistry::UnregisterOnConfigChanged(&g_selfA, OnChangedClampViaWrite);
}

// A hand-edited INI that spells a section or key in another case still applies
// its Block state when the plugin is loaded: INI names are case-insensitive.
static void Test_BlockStateIgnoresCase()
{
    DeleteFileW(IniPath(kPluginA).c_str());
    WriteDisk(kPluginA, "input", "boost", "F5");
    WriteDisk(kPluginA, "input", "BOOSTBLOCKING", "1");
    TestDoubles::ResetRecordedCalls();
    ConfigEdit::Load(kPluginA);

    CHECK(TestDoubles::blockingCalls.size() == 1);
    if (TestDoubles::blockingCalls.size() == 1)
    {
        CHECK_STR(TestDoubles::blockingCalls[0].owner, std::string(kPluginA) + "|Input|Boost");
        CHECK_STR(TestDoubles::blockingCalls[0].combo, "F5");
        CHECK(TestDoubles::blockingCalls[0].blocking);
    }
    CHECK(ConfigEdit::GetBlocking(kPluginA, "Input", "Boost"));

    // Same file, no Block flag: applied as not blocking.
    DeleteFileW(IniPath(kPluginA).c_str());
    WriteDisk(kPluginA, "input", "boost", "F5");
    TestDoubles::ResetRecordedCalls();
    ConfigEdit::Load(kPluginA);
    CHECK(TestDoubles::blockingCalls.size() == 1);
    if (TestDoubles::blockingCalls.size() == 1)
        CHECK(!TestDoubles::blockingCalls[0].blocking);
}

// When a plugin is unloaded the loader forgets the callbacks it still had
// registered; the plugin next to it keeps its own.
static void Test_CallbackPurgedOnUnload()
{
    FreshPluginA();
    DeleteFileW(IniPath(kPluginB).c_str());
    WriteDisk(kPluginB, "Sec", "K", "1");
    ConfigEdit::Load(kPluginB);

    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfA, OnChangedA);
    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfB, OnChangedB);

    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "11");
    ConfigEdit::SetLive(kPluginB, "Sec", "K", "2");
    CHECK(g_callsA.size() == 1);
    CHECK(g_callsB.size() == 1);

    // The plugin is unloaded without having unregistered.
    UI::PluginPanelRegistry::ForgetConfigCallbacks(&g_selfA);

    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "12");
    ConfigEdit::SetLive(kPluginB, "Sec", "K", "3");
    CHECK(g_callsA.size() == 1); // not called into the unloaded plugin
    CHECK(g_callsB.size() == 2);
    CHECK_STR(Get(kPluginA, "Drone", "Speed"), "12"); // the value still changed

    // The slot is kept across an unload, so a reloaded plugin registers afresh.
    UI::PluginPanelRegistry::RegisterOnConfigChanged(&g_selfA, OnChangedA);
    ConfigEdit::SetLive(kPluginA, "Drone", "Speed", "13");
    CHECK(g_callsA.size() == 2);

    UI::PluginPanelRegistry::ForgetConfigCallbacks(&g_selfA);
    UI::PluginPanelRegistry::ForgetConfigCallbacks(&g_selfB);
    DeleteFileW(IniPath(kPluginB).c_str());
}

// The test above proves the purge works; this one proves the loader calls it.
// plugin_manager.cpp cannot be linked into a console program (it is the plugin
// loader), so its three paths that unmap a plugin -- unload all, unload one,
// reload -- are read as source. Each must call ForgetConfigCallbacks, and call
// it before FreeLibrary, since afterwards the callback's address is freed
// memory. Delete any one call and this fails.
static std::string StripLineComments(const std::string& src)
{
    std::string out;
    for (size_t pos = 0; pos < src.size();)
    {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) eol = src.size();
        std::string line = src.substr(pos, eol - pos);
        const size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        out += line;
        out += '\n';
        pos = eol + 1;
    }
    return out;
}

// Text of the function whose definition starts with signature, up to its
// closing brace (a tab-indented "}" line; the functions sit in a namespace).
static std::string FunctionBody(const std::string& src, const char* signature)
{
    const size_t start = src.find(signature);
    if (start == std::string::npos) return "";
    const size_t end = src.find("\n\t}", start);
    return end == std::string::npos ? "" : src.substr(start, end - start);
}

static void Test_UnloadPathsPurgeCallbacks()
{
    // <solution>\build\tests\config_edit_tests.exe: the exe's name, then tests, then build.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    for (int up = 0; up < 3; ++up)
        path.erase(path.find_last_of(L"\\/"));
    path += L"\\StarRupture-ModLoader-Core\\plugins\\plugin_manager.cpp";

    std::ifstream in(path.c_str(), std::ios::binary);
    CHECK(in.good());
    const std::string src = StripLineComments(
        std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));

    const char* const kPaths[] =
    {
        "void UnloadAllPlugins()",
        "bool UnloadPlugin(int index)",
        "bool ReloadPlugin(int index)",
    };
    for (const char* signature : kPaths)
    {
        printf("    %s\n", signature);
        const std::string body = FunctionBody(src, signature);
        CHECK(!body.empty());

        const size_t forget = body.find("ForgetConfigCallbacks(");
        const size_t free   = body.find("FreeLibrary(");
        CHECK(forget != std::string::npos);
        CHECK(free != std::string::npos);
        CHECK(forget < free);
    }
}

// ---------------------------------------------------------------------------

int main()
{
    ModLoaderLogger::InitializeConfigManager();

    g_selfA.name   = kPluginA;
    g_selfA.config = ModLoaderLogger::GetPluginConfig();
    g_selfB.name   = kPluginB;
    g_selfB.config = ModLoaderLogger::GetPluginConfig();
    TestDoubles::SetPluginSelves(&g_selfA, &g_selfB);

    RunTest("LiveCommitAndNoopWrite",       Test_LiveCommitAndNoopWrite);
    RunTest("HandlerMayCallBack",           Test_HandlerMayCallBack);
    RunTest("KeybindRebindOnCommit",        Test_KeybindRebindOnCommit);
    RunTest("PluginWriteVisibleThroughGet", Test_PluginWriteVisibleThroughGet);
    RunTest("ClampedValueIsPersisted",      Test_ClampedValueIsPersisted);
    RunTest("BlockStateIgnoresCase",        Test_BlockStateIgnoresCase);
    RunTest("CallbackPurgedOnUnload",       Test_CallbackPurgedOnUnload);
    RunTest("UnloadPathsPurgeCallbacks",    Test_UnloadPathsPurgeCallbacks);

    DeleteFileW(IniPath(kPluginA).c_str());
    ModLoaderLogger::ShutdownConfigManager();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
