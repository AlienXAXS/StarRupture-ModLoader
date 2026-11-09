#pragma once

#include "memory_scanner/scan_validation.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ScanPatterns { struct PreflightEntry; }
struct PluginInfo;

// ---------------------------------------------------------------------------
// VersionMap -- picks the locator for the game build that is running.
//
// A component that knows where a function sits on more than one game build
// hands every locator it has for it (one per changelist, see
// PluginScanCandidate in plugins/plugin_interface.h). The choice is made here,
// on top of ScanValidation, so every locator gets the same uniqueness and kind
// checks the request's own pattern gets:
//
//   1. Exact. The row whose changelist is the running one. On the very exe the
//      row was verified on (same changelist, PE TimeDateStamp and SizeOfImage),
//      it must land on its recorded RVA: same binary, different answer (another
//      address, or the pattern not found) is a failure with no fallback. On
//      another exe with the same changelist, a row whose pattern is not found
//      is logged and skipped. A row with no pattern is an address for its own
//      exe only, so it needs that exe's stamp and size and an RVA.
//   2. Agreement. Every other locator, the request's own pattern included. At
//      least one must resolve, and every one that resolves must give the same
//      final address. Different addresses are a failure (AMBIGUOUS).
//   3. Otherwise a failure, recorded as a miss.
//
// Rows from the loader's own map (fromMap) come before the component's own:
// the map's row for the running changelist is tried first, then the
// component's. When the component's own row is for this very exe, the map's
// pick must land on that row's RVA, or the result is AMBIGUOUS (no scan).
//
// With no map row for the running changelist, the map supplies the row of the
// latest changelist it has one for (ResolveMapKey reads no older ones), and
// rule 2 rescans it on this build: its pattern must resolve, uniquely and as
// the request's kind, before it counts. A row that does not resolve here is
// not used. Then the component's own pattern and rows; if none of them
// resolves, the key is unresolved, which the caller turns into a feature
// turned off or a plugin refused (plugins/plugin_hook_report.h). An address
// that was not found on this build is never used.
//
// One "[Map]" line per key says which rule picked the address, and from
// where: "loader map, CL-128786 row", "own CL-127004 row", "own pattern".
// ---------------------------------------------------------------------------

namespace VersionMap
{
	struct Candidate
	{
		uint32_t    gameCL     = 0;
		uint32_t    imageStamp = 0;   // 0 = unknown
		uint32_t    imageSize  = 0;
		std::string pattern;          // empty = address-only: honoured on the exact exe only
		int32_t     resultOffset    = 0;
		int32_t     followRel32At   = -1;
		int32_t     followRipDispAt = -1;
		uint32_t    rva = 0;          // 0 = none
		bool        fromMap = false;  // a row of the loader's map, not the component's own
	};

	// The loader's map as data: one row per record of the generated header
	// (hooks/game/version_map.generated.h) or of the feed (below). Strings are
	// owned, so a map read at run time outlives its text. An empty pattern is
	// the header's nullptr, an empty member or owners likewise.
	struct MapBuild
	{
		uint32_t    gameCL = 0, imageStamp = 0, imageSize = 0;
		std::string source;
	};
	struct MapFunctionRow
	{
		std::string key;
		uint32_t    gameCL = 0, imageStamp = 0, imageSize = 0;
		int32_t     kind = 0;
		uint32_t    fp = 0;
		bool        absent = false;
		std::string pattern;
		int32_t     resultOffset = 0, followRel32At = -1, followRipDispAt = -1;
		uint32_t    rva = 0;
		std::string owners;   // an absent row of a global key: "A,B", empty = every owner
	};
	struct MapFieldRow
	{
		std::string key;      // "Class::Member"
		uint32_t    gameCL = 0;
		int32_t     offset = 0;
		uint32_t    size = 0;
		uint8_t     boolMask = 0;
		std::string member;   // the member's name on this build, when it was renamed
	};
	struct MapData
	{
		std::vector<MapBuild>       builds;      // by CL
		std::vector<MapFunctionRow> functions;   // by key, fp, CL
		std::vector<MapFieldRow>    fields;      // by key, CL
	};

	// The map compiled into this loader.
	const MapData& BaselineMap();

	// The map every lookup reads: the baseline, unless SetActiveMap replaced
	// it. SetActiveMap is for the start of the process (a loader that merged a
	// feed; a tool given a map to test with), before anything reads; the map
	// stays as it was set for the rest of the process.
	const MapData& ActiveMap();
	void SetActiveMap(std::shared_ptr<const MapData> map);

	// modloader.ini [Debug] DisablePluginVersionMap=1: the map serves no rows to
	// plugins, so each falls back to its own patterns and rows (or its feature
	// turns off). The loader's own lookups (owner "ModLoader") keep the map.
	// Set once at startup, before the first lookup.
	void SetPluginMapOff(bool off);
	bool PluginMapOff();

	// The feed format this loader reads (the `format` line of a feed).
	constexpr int kFeedFormat = 1;

	// Reads the text of a feed (version-map-client.v1.txt without its `sig`
	// line) into `out`: tab-separated lines, LF, a final LF, in the order
	// `format`, `build`, `function`/`absent`, `field`, as ci/VERSION-MAP.md
	// lists. Strict: a known line with too few columns, a number out of range,
	// a row whose build line is missing or says another stamp or size, or a
	// scoped row (any kind but FunctionStart and Data) with no fp rejects the
	// whole text, with `error` naming the line. `out` is left empty on failure.
	// Lenient where a newer feed may grow: a line of a kind this loader does not
	// know is skipped (one log line counts them), and columns past the ones a
	// known kind has are ignored.
	bool ParseFeed(const std::string& text, MapData& out, std::string& error);

	// A game exe: the CL from the end of its ProductVersion, the stamp and
	// size from its PE headers.
	struct Exe
	{
		uint32_t cl         = 0;      // 0 = the ProductVersion has no CL-<n>
		uint32_t imageStamp = 0;
		uint32_t imageSize  = 0;
	};

	// The exe mapped at `module`.
	Exe ExeOf(HMODULE module);

	// The running game exe. Read once.
	Exe RunningExe();

	// The trailing CL-<n> of a ProductVersion, or 0 (also for a leading zero).
	uint32_t ParseCL(const std::wstring& productVersion);

	// The game builds this loader runs on, the gate VerifyGameVersion applies.
	// The client runs on every build its map has rows for (kBuilds; CI adds a
	// build only once every one of the loader's patterns resolves on it), the
	// server only on the build it was written for (kBuiltCL). Any other build
	// is refused.
	std::vector<uint32_t> SupportedBuilds();

	// True when the ProductVersion ends in the CL of one of SupportedBuilds().
	bool IsSupported(const std::wstring& productVersion);

	// The game build every v70 and v71 plugin was compiled against: the one
	// the last loader without a version map required. A plugin before v70 was
	// compiled against an older build, and gets gameCL 0, which matches none.
	constexpr uint32_t kLegacyPluginCL = 127004;
	constexpr int      kLegacyPluginInterface = 70;

	// The game build a plugin says its Game-SDK came from (the v72 PluginInfo
	// tail), and what it declares about itself. A v70 or v71 plugin has no
	// tail and gets kLegacyPluginCL; so does a v72 plugin whose tail is 0,
	// with declared false. One before v70 gets 0. The plugin manager copies
	// this into the plugin's record at load.
	struct PluginGameBuild
	{
		uint32_t gameCL   = kLegacyPluginCL;
		uint32_t mapFlags = 0;
		bool     declared = false;   // the tail names a build
	};
	PluginGameBuild ReadPluginGameBuild(const PluginInfo& info);

	// "CL-127004", "CL-127004 (none declared)", or "a build before CL-127004".
	std::string DescribePluginGameBuild(const PluginGameBuild& build);

	// Whether a plugin may run on `running`, judged against the build it was
	// written for (`built`), not the loader's. On that build it loads as ever.
	// On any other supported build its compiled-in layouts and its SDK's
	// Offsets:: point where its own build had things, so it runs only when it
	// takes all of them from this build: interface 72 or later, PLUGIN_MAP_MIGRATED
	// in the tail, and GetSdkGlobals in PluginInit. A tail that sets
	// PLUGIN_MAP_MIGRATED and names no build is refused on every build, and so
	// is a plugin built for gameCL 0 (before v70), whatever runs. The
	// loader asks at DLL load, passing true for the globals it cannot know yet,
	// and again after PluginInit. Empty = admitted; else the reason, for the
	// log and the plugin list.
	std::string OffBuildRefusal(const Exe& running, int interfaceVersion, const PluginGameBuild& built, bool askedSdkGlobals);

	// The classes the startup layout check found changed on this game build
	// (core/layout_check.h), when it did not switch plugins off: each by its
	// SDK name (ACrX) and as /Script/<Module>.<SDK name>, and each by its
	// Module.Name path (LiveField::paths), which lets a class whose base
	// changed be told from one whose own layout did not. Set once, on the
	// init thread, before any plugin loads.
	void SetLayoutChanges(std::vector<std::string> classNames, std::vector<std::string> paths = {});

	// Whether a plugin may run although classes changed: only one that takes
	// every game layout from this build (interface 72 or later,
	// PLUGIN_MAP_MIGRATED, and a declared game build) may. Any other keeps
	// the layouts compiled in and would read the changed classes' old ones.
	// Empty = admitted (also when nothing changed); else the reason.
	std::string LayoutRefusal(int interfaceVersion, const PluginGameBuild& built);

	// Whether the Game-SDK globals found on `running` may be handed to plugins
	// (IPluginEngineEvents::GetSdkGlobals), as absolute addresses, 0 = not
	// found. A plugin needs GObjects and FName::AppendString to set up its
	// SDK; on any build other than kBuiltCL it also needs GWorld, because its
	// SDK's own UWorld::GetWorld reads kBuiltCL's address. Empty = hand them
	// out; else what is missing, for the log.
	std::string SdkGlobalsWithheld(const Exe& running, uintptr_t gobjects, uintptr_t appendString, uintptr_t gworld);

	// Resolves `request` (the component's own pattern, kind and transforms)
	// with `candidates`, as described above. `owner` names the component in
	// the [Map] line. The result reads like ScanValidation's: a failure's
	// detail says which rule failed and what each locator found.
	ScanValidation::Result Select(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& candidates, const Exe& running);

	// The fingerprint a request's rows are kept under in the loader's map, for
	// a key of a kind other than FunctionStart and Data (a site, which names no
	// symbol, so its address is right only for the request it was verified
	// with). FNV-1a 32 over, in order, each integer 4 bytes little-endian: the
	// kind; the pattern as Scanner::ParsePattern reads it (two upper-case hex
	// digits or ?? per byte, one space between), as its length then its bytes;
	// resultOffset; followRel32At (-1 when not followed); followRipDispAt; and
	// the site arithmetic revision (PreflightEntry::siteArithmetic, 0 for a
	// plugin). 0 when the pattern does not parse; a hash of 0 is given as 1, so
	// 0 always means none.
	uint32_t Fingerprint(int kind, const std::string& pattern, int32_t resultOffset,
		int32_t followRel32At, int32_t followRipDispAt, uint32_t siteArithmetic);

	// A plugin's map-key request (PLUGIN_SCAN_FLAG_MAP_KEY). request.name is
	// the key: for kinds FunctionStart and Data a global one ("Class::Function"
	// or a global's name, never with a '/'), for every other kind the owner's
	// own site, "<owner>/<name>", which needs the request's pattern for its
	// fingerprint. The map's rows apply only when they are of the request's
	// kind and, on a site, have its fingerprint; otherwise they are set aside
	// and logged. An absent row for the running build that binds the request
	// (a global key's listed owner, a site's fingerprint) is a failure with
	// nothing else tried. Then Select, over the map's rows and `own`, the
	// plugin's own rows. A key the map does not hold falls back to the
	// plugin's own pattern and rows; with neither it is a failure. A plugin
	// named ModLoader is refused: the map's ModLoader/ keys are the loader's.
	// ownerNameShared: another loaded plugin has the owner's name, so its site
	// keys could be either plugin's, and a site key is refused (a global key
	// is not affected). The client only; on the server the map holds no rows
	// for it.
	ScanValidation::Result ResolveMapKey(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& own, const Exe& running, bool ownerNameShared);

	// The key a registry row is kept under in the loader's map: its name for
	// kinds FunctionStart and Data, "ModLoader/<name>" for a site of any other
	// kind, with the site's fingerprint (0 for a global key).
	std::string LoaderEntryKey(const ScanPatterns::PreflightEntry& entry);
	uint32_t LoaderEntryFingerprint(const ScanPatterns::PreflightEntry& entry);

	// The loader's own registry row (ScanPatterns::PreflightRegistry), resolved
	// with the rows its map (hooks/game/version_map.generated.h) holds for it,
	// under LoaderEntryKey and by the rules of ResolveMapKey. With no rows, the
	// row's own pattern alone, as before the map. The client only: the server
	// build stays on its exact CL.
	//
	// A row marked siteArithmetic takes map rows only where they carry its own
	// pattern, because its hook site adds offsets that only that pattern's
	// shape makes true.
	ScanValidation::Result ResolveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const Exe& running);

	// What preflight picked for a registry pattern, by its text, so the hook
	// sites that scan the same ScanPatterns:: constant get the same address
	// (0 = none). Set during preflight, on the init thread, before any hook
	// site asks; only rows with no follow and no site arithmetic are kept,
	// because a site's own scan gives the match itself.
	void RememberPick(const ScanPatterns::PreflightEntry& entry, uintptr_t address);
	uintptr_t PickFor(const std::string& pattern);

	// ---- Fields ------------------------------------------------------------
	//
	// Where a member of a game class sits on the running build (see
	// PluginFieldRequest in plugins/plugin_interface.h for what a component
	// asks). A reflected member's offset is read from the running game's own
	// reflection data. A row (the running changelist's, else the latest one's,
	// which can also give the member's new name) only cross-checks it: where
	// the row and the game disagree the game wins and the disagreement is
	// logged. A native member (one reflection does not describe) is the
	// running changelist's row, or the compiled offset on the build it was
	// compiled for, and nothing else: a row carried forward from an older
	// changelist cannot be checked on this build, so it is not used. It must
	// lie inside its class as the running game reflects the class.
	//
	// The rows are the loader's map's (kFields, under the request's key) and
	// then the component's own. The map's are used only when its key names
	// the request's class and member, or the member one of its rows records
	// as renamed; otherwise they are set aside and logged. An own row for a
	// changelist the map has a row for must agree with it, or the request
	// fails. The client only: on the server the map holds no rows for it, and
	// with no reflection reader there a native member is not bounded.

	// One build's value for a field.
	struct FieldCandidate
	{
		uint32_t    gameCL   = 0;
		int32_t     offset   = -1;
		uint32_t    size     = 0;
		uint8_t     boolMask = 0;
		std::string member;           // the name on that build if it was renamed, else empty
		bool        fromMap  = false; // a row of the loader's map, not the component's own
	};

	struct FieldRequest
	{
		std::string key;              // "Class::Member", for the [Map] line and the failure
		std::string className;        // short name, or /Script/<Module>.<Name>
		std::string member;           // empty = the size of className itself
		uint32_t    flags       = 0;  // PluginFieldFlags
		uint32_t    size        = 0;  // 0 = a reflected member of any size
		uint32_t    builtCL     = 0;
		int32_t     builtOffset = -1;
		std::vector<FieldCandidate> candidates;
	};

	struct FieldValue
	{
		int32_t  offset   = -1;
		uint32_t size     = 0;
		uint8_t  boolMask = 0;
	};

	// What the running game reflects for one member (member empty: for the
	// class itself, whose size is its PropertiesSize). error is set when it
	// has no answer: no such class, more than one, or no such member.
	// unreflected is set when the game has no reflected class or struct of that
	// name (a plain C++ type), as against any other failure.
	struct LiveField
	{
		std::string error;
		bool        unreflected = false;
		int32_t     offset   = 0;
		uint32_t    size     = 0;
		bool        isBool   = false;
		uint8_t     boolMask = 0;     // FBoolProperty::FieldMask when isBool
		std::vector<std::string> paths;  // Module.Name of the class and each of its supers, set whenever it was found
	};
	using LiveFieldSource = LiveField (*)(const std::string& className, const std::string& member);

	// The reflection reader SelectField asks. Set once, before the first
	// field is asked for: in the game, LayoutCheck's (core/layout_check.h).
	// With none set, every reflected field fails.
	void SetLiveFieldSource(LiveFieldSource source);

	// Applies the rules above to one request, logs its [Map] line, and on
	// success writes out. On failure, error says which rule failed.
	bool SelectField(const char* owner, const FieldRequest& request, const Exe& running,
		FieldValue& out, std::string& error);

	// The members the loader itself reads per build (SdkGlobals::ResolveFields),
	// each a key of kFields. The map holds other components' members too.
	inline constexpr const char* kLoaderFieldKeys[] = {
		"ACrCrafter::CraftComponent",
		"UWorld::GameState",
		"UWorld::LineBatchers",
		"UWorld::NetDriver",
	};

	// The request for one of the loader's own members ("Class::Member", one of
	// kLoaderFieldKeys); SelectField adds the rows its map holds for it. size
	// and builtOffset are the member's in the SDK the loader is built against.
	FieldRequest LoaderFieldRequest(const char* key, uint32_t size, int32_t builtOffset);
}
