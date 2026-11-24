// The loader's call to a plugin's PluginSetHostInfo export, and what a plugin does with it.
//
// A console program. The test is a stand-in plugin: it declares need 73 and optional 80
// (the header defines, per translation unit, which accessors exist), exports
// PluginSetHostInfo through PLUGIN_EXPORT_HOST_INFO(), and the loader's own
// PluginHostInfoCall::Run (plugins/plugin_host_info.h) makes the call, the same function
// plugin_manager.cpp uses. Host tables are plain byte buffers sized to the real structs
// with a marker in each optional slot, so a read at the wrong offset shows.
#define PLUGIN_NEEDS_INTERFACE 73
#define PLUGIN_OPTIONAL_INTERFACE 80

#include "plugins/plugin_interface.h"
#include "plugins/plugin_host_info.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

PLUGIN_EXPORT_HOST_INFO()

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                      \
	do {                                                                                 \
		++g_checks;                                                                      \
		if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
	} while (0)

// Host tables: zeroed bytes, the optional slots hold a marker. The tables the plugin sees
// at need 73 are smaller than these (the newer members are hidden), so the buffers are
// sized from the real structs, which the loader's own build has in full.
static alignas(16) unsigned char g_hooks[256];
static alignas(16) unsigned char g_network[256];
static alignas(16) unsigned char g_imgui[2112];

static int g_imagesMarker, g_hostRulesMarker, g_registryMarker;

static PluginDrawResult FakeTriangles(PluginDrawList, PluginTextureHandle, const PluginDrawVertex*, uint32_t, const PluginDrawIndex*, uint32_t)
{
	return PluginDrawResult();
}
static int FakeServerAddress(char*, size_t) { return 0; }

template<class T>
static void PutSlot(unsigned char* table, size_t offset, T value)
{
	std::memcpy(table + offset, &value, sizeof(T));
}

static void FillTables()
{
	PutSlot(g_hooks, PLUGIN_OPT_OFFSET_IMAGES, reinterpret_cast<IPluginImages*>(&g_imagesMarker));
	PutSlot(g_hooks, PLUGIN_OPT_OFFSET_HOST_RULES, reinterpret_cast<IPluginHostRules*>(&g_hostRulesMarker));
	PutSlot(g_hooks, PLUGIN_OPT_OFFSET_REGISTRY, reinterpret_cast<IPluginRegistry*>(&g_registryMarker));
	PutSlot(g_network, PLUGIN_OPT_OFFSET_SERVER_ADDRESS, &FakeServerAddress);
	PutSlot(g_imgui, PLUGIN_OPT_OFFSET_ADD_TEXTURED_TRIANGLES, &FakeTriangles);
}

static const IPluginHooks*          Hooks()   { return reinterpret_cast<const IPluginHooks*>(g_hooks); }
static const IPluginNetworkChannel* Network() { return reinterpret_cast<const IPluginNetworkChannel*>(g_network); }
static const IModLoaderImGui*       ImGuiT()  { return reinterpret_cast<const IModLoaderImGui*>(g_imgui); }

static void ReportLevel(uint32_t level)
{
	PluginHostInfo info = { sizeof(PluginHostInfo), level };
	PluginRecordHostInfo(&info);
}

static int   g_filterCalls = 0;
static DWORD g_filterCode = 0;
static LONG Filter(EXCEPTION_POINTERS* ep)
{
	++g_filterCalls;
	g_filterCode = ep->ExceptionRecord->ExceptionCode;
	return EXCEPTION_EXECUTE_HANDLER;
}

static PluginHostInfo g_seen = {};
static void RecordingExport(const PluginHostInfo* info) { g_seen = *info; }

static void FaultingExport(const PluginHostInfo*)
{
	volatile int* p = nullptr;
	*p = 1;
}

static void ExpectOptionalOff()
{
	CHECK(PluginOptImages(Hooks()) == nullptr);
	CHECK(PluginOptHostRules(Hooks()) == nullptr);
	CHECK(PluginOptRegistry(Hooks()) == nullptr);
	CHECK(PluginOptServerAddress(Network()) == nullptr);
	CHECK(PluginOptDL_AddTexturedTriangles(ImGuiT()) == nullptr);
}

int main()
{
	FillTables();

	// 1. A loader that never calls the export (v1.26.1, v1.27.0): the level is the declared
	//    need, every optional member is off.
	CHECK(PluginHostLevel() == 73);
	CHECK(PluginHostHas(73));
	CHECK(!PluginHostHas(74));
	CHECK(!PluginHostHas(80));
	ExpectOptionalOff();

	// 2. An export that gets a short or missing struct changes nothing.
	PluginRecordHostInfo(nullptr);
	CHECK(PluginHostLevel() == 73);
	PluginHostInfo shortInfo = { 4, 80 };
	PluginRecordHostInfo(&shortInfo);
	CHECK(PluginHostLevel() == 73);
	ExpectOptionalOff();

	// 3. The loader's call hands over its own size and level.
	CHECK(PluginHostInfoCall::Run(&RecordingExport, &Filter));
	CHECK(g_seen.structSize == sizeof(PluginHostInfo));
	CHECK(g_seen.hostInterface == PLUGIN_INTERFACE_VERSION_MAX);
	CHECK(g_filterCalls == 0);

	// 4. The call to the real export turns the optional members on.
	CHECK(PluginHostInfoCall::Run(&PluginSetHostInfo, &Filter));
	CHECK(PluginHostLevel() == PLUGIN_INTERFACE_VERSION_MAX);
	CHECK(PluginHostHas(80));
	CHECK(PluginOptImages(Hooks()) == reinterpret_cast<IPluginImages*>(&g_imagesMarker));
	CHECK(PluginOptHostRules(Hooks()) == reinterpret_cast<IPluginHostRules*>(&g_hostRulesMarker));
	CHECK(PluginOptRegistry(Hooks()) == reinterpret_cast<IPluginRegistry*>(&g_registryMarker));
	CHECK(PluginOptServerAddress(Network()) == &FakeServerAddress);
	CHECK(PluginOptDL_AddTexturedTriangles(ImGuiT()) == &FakeTriangles);
	CHECK(PluginOptRegistry(nullptr) == nullptr);   // no table, no read

	// 5. A loader that reports a lower level: only the members it has.
	ReportLevel(78);
	CHECK(PluginHostHas(78));
	CHECK(!PluginHostHas(79));
	CHECK(PluginOptImages(Hooks()) != nullptr);
	CHECK(PluginOptDL_AddTexturedTriangles(ImGuiT()) == &FakeTriangles);
	CHECK(PluginOptHostRules(Hooks()) == nullptr);
	CHECK(PluginOptRegistry(Hooks()) == nullptr);
	CHECK(PluginOptServerAddress(Network()) == nullptr);

	// 6. A fault in the export is reported to the caller and seen by the filter.
	g_filterCalls = 0;
	CHECK(!PluginHostInfoCall::Run(&FaultingExport, &Filter));
	CHECK(g_filterCalls == 1);
	CHECK(g_filterCode == EXCEPTION_ACCESS_VIOLATION);

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
