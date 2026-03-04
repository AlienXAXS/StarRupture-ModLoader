#include "http_connection.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cwchar>

// ---------------------------------------------------------------------------
// FHttpConnection::ProcessRequest hook
//
// Signature (from IDA):
// void __fastcall FHttpConnection::ProcessRequest(__int64 a1, _QWORD *a2, __int64 a3)
//
// a2 is a TSharedPtr<FHttpServerRequest> — *a2 is the raw request pointer.
// a3 is a TFunction<void(TUniquePtr<FHttpServerResponse>&&)> — the response callback.
//
// Confirmed FHttpServerRequest offsets (verified experimentally):
//   obj+16  : FString RelativePath  { wchar_t* Data @+16, int32 Num @+24 }
//   obj+280 : TArray<uint8> Body    { uint8*   Data @+280, int32 Num @+288 }
//
// Pattern:
// 48 89 5C 24 ?? 55 56 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 79
// ---------------------------------------------------------------------------

static constexpr const char* PROCESS_REQUEST_PATTERN =
	"48 89 5C 24 ?? 55 56 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 79";

static constexpr size_t URL_OFFSET  = 16;
static constexpr size_t BODY_OFFSET = 280;

static constexpr const char* REMOTE_CALL_URL     = "/remote/object/call";
static constexpr const char* ALLOWED_OBJECT_PATH =
	"/Game/Chimera/Maps/DedicatedServerStart.DedicatedServerStart:PersistentLevel.BP_DedicatedServerSettingsActor_C_1.DedicatedServerSettingsComp";

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
typedef void(__fastcall* ProcessRequest_t)(__int64 a1, uint64_t* a2, __int64 a3);
static ProcessRequest_t g_originalProcessRequest = nullptr;
static HookHandle       g_hookHandle             = nullptr;

// Address of FHttpServerResponse::Error — resolved at install time by scanning
// the bytes of ProcessRequest for "mov edx, 404 (0x194)" followed by a CALL.
// typedef: void __fastcall FHttpServerResponse_Error(void** outPtr, int32 code, FString* errCode, FString* errMsg)
typedef void(__fastcall* FHttpServerResponseError_t)(void** outPtr, int32_t code, void* errCode, void* errMsg);
static uintptr_t g_errorFuncAddr = 0;

// ---------------------------------------------------------------------------
// Safe memory helpers
// ---------------------------------------------------------------------------
static bool IsReadableMemory(uintptr_t addr, size_t size)
{
	if (!addr || !size) return false;
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT) return false;
	if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
	return addr + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

static size_t TryReadBytes(uintptr_t addr, void* out, size_t len)
{
	if (!IsReadableMemory(addr, 1)) return 0;
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return 0;
	uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	size_t readable = (regionEnd > addr) ? (regionEnd - addr) : 0;
	size_t n = len < readable ? len : readable;
	__try { memcpy(out, reinterpret_cast<const void*>(addr), n); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	return n;
}

// ---------------------------------------------------------------------------
// Request field readers
// ---------------------------------------------------------------------------

// Reads FString at req+offset and returns it as a UTF-8 std::string.
static std::string ReadFStringUtf8(uintptr_t req, size_t offset)
{
	uintptr_t dataPtr = 0;
	if (TryReadBytes(req + offset, &dataPtr, sizeof(dataPtr)) != sizeof(dataPtr) || !dataPtr)
		return {};

	std::vector<wchar_t> wbuf(512);
	size_t got = TryReadBytes(dataPtr, wbuf.data(), wbuf.size() * sizeof(wchar_t));
	size_t numChars = got / sizeof(wchar_t);

	size_t len = 0;
	while (len < numChars && wbuf[len]) ++len;
	if (!len) return {};

	int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), (int)len, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) return {};
	std::string out(needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), (int)len, &out[0], needed, nullptr, nullptr);
	return out;
}

// Reads TArray<uint8> at req+offset and returns the bytes as a std::string.
static std::string ReadByteArray(uintptr_t req, size_t offset)
{
	uintptr_t dataPtr = 0;
	if (TryReadBytes(req + offset, &dataPtr, sizeof(dataPtr)) != sizeof(dataPtr) || !dataPtr)
		return {};

	int32_t num = 0;
	if (TryReadBytes(req + offset + 8, &num, sizeof(num)) != sizeof(num) || num <= 0 || num > 16 * 1024 * 1024)
		return {};

	if (!IsReadableMemory(dataPtr, (size_t)num))
		return {};

	std::string out((size_t)num, '\0');
	if (TryReadBytes(dataPtr, &out[0], (size_t)num) != (size_t)num)
		return {};
	return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON string-value extractor
// Handles: "key": "value"  with basic escape sequences.
// ---------------------------------------------------------------------------
static std::string ExtractJsonString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	auto pos = json.find(needle);
	if (pos == std::string::npos) return {};
	pos += needle.size();

	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':' || json[pos] == '\n' || json[pos] == '\r'))
		++pos;

	if (pos >= json.size() || json[pos] != '"') return {};
	++pos; // skip opening quote

	std::string value;
	value.reserve(256);
	while (pos < json.size() && json[pos] != '"')
	{
		if (json[pos] == '\\' && pos + 1 < json.size())
		{
			++pos;
			switch (json[pos])
			{
				case '"':  value += '"';  break;
				case '\\': value += '\\'; break;
				case '/':  value += '/';  break;
				case 'n':  value += '\n'; break;
				case 'r':  value += '\r'; break;
				case 't':  value += '\t'; break;
				default:   value += json[pos]; break;
			}
		}
		else
			value += json[pos];
		++pos;
	}
	return value;
}

// ---------------------------------------------------------------------------
// FHttpServerResponse::Error discovery
//
// Scans the bytes of FHttpConnection::ProcessRequest for the instruction
// sequence "mov edx, 0x194" (BA 94 01 00 00 = 404 decimal) immediately
// followed by a direct CALL (E8).  The only call with that exact second
// argument inside ProcessRequest is FHttpServerResponse::Error.
// ---------------------------------------------------------------------------
static uintptr_t FindErrorFunction(uintptr_t processRequestAddr)
{
	uint8_t buf[4096] = {};
	size_t got = TryReadBytes(processRequestAddr, buf, sizeof(buf));
	if (got < 16) return 0;

	// BA 94 01 00 00 = mov edx, 194h (404)
	static const uint8_t kMov404[] = { 0xBA, 0x94, 0x01, 0x00, 0x00 };

	for (size_t i = 0; i + 40 < got; ++i)
	{
		if (memcmp(buf + i, kMov404, sizeof(kMov404)) != 0)
			continue;

		// Scan forward up to 40 bytes for a direct relative CALL (E8 xx xx xx xx)
		for (size_t j = i + sizeof(kMov404); j + 5 <= got && j < i + 40; ++j)
		{
			if (buf[j] != 0xE8)
				continue;

			int32_t rel = 0;
			memcpy(&rel, buf + j + 1, sizeof(rel));
			uintptr_t target = processRequestAddr + j + 5 + static_cast<intptr_t>(rel);

			if (IsReadableMemory(target, 16))
				return target;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// 403 response sender
//
// Constructs a real FHttpServerResponse via FHttpServerResponse::Error and
// fires it through the a3 TFunction callback.
//
// TFunction invocation sequence is taken verbatim from the ProcessRequest
// pseudocode (confirmed offsets — see comment block at top of file):
//
//   v15 = a3 + 16                                  // default: inline storage
//   v16 = *(func_ptr*)a3                            // the invoke wrapper fn
//   if (*QWORD(a3 + 40)) v15 = *QWORD(a3 + 40)    // override with heap storage
//   v17 = (*(vtable[1] of v15))(v15)               // get bound callable target
//   v16(v17, &response)                             // invoke
//
// FString layout on x64 (TArray<wchar_t, TSizedHeapAllocator<32>>):
//   { wchar_t* Data(8), int32 Num(4), int32 Max(4) }  = 16 bytes
// ---------------------------------------------------------------------------

// Must match TArray<wchar_t> layout exactly.
struct FakeString
{
	wchar_t* Data;
	int32_t  Num;
	int32_t  Max;
};

typedef void(__fastcall* CallbackInvokeFn)(__int64 callable, void** response);
typedef __int64(__fastcall* CallbackGetTargetFn)(__int64 storage);

static void SendBlockedResponse(__int64 a1, __int64 a3)
{
	if (!g_errorFuncAddr)
	{
		LOG_WARN("[RemoteVulnerabilityPatcher] FHttpServerResponse::Error not located — connection dropped without response");
		return;
	}

	auto* hooks = GetHooks();
	if (!hooks || !hooks->IsEngineAllocatorAvailable())
	{
		LOG_WARN("[RemoteVulnerabilityPatcher] Engine allocator unavailable — connection dropped without response");
		return;
	}

	// Build engine-heap-allocated FString for the error code.
	// Using the same naming convention as UE5's built-in error codes.
	static const wchar_t kErrorCode[] = L"errors.com.epicgames.httpserver.forbidden";
	static const int32_t kErrorCodeChars = (int32_t)(sizeof(kErrorCode) / sizeof(wchar_t)); // includes null

	wchar_t* errData = static_cast<wchar_t*>(
		hooks->EngineAlloc(sizeof(kErrorCode), 4));
	if (!errData)
	{
		LOG_WARN("[RemoteVulnerabilityPatcher] EngineAlloc failed — connection dropped without response");
		return;
	}
	memcpy(errData, kErrorCode, sizeof(kErrorCode));

	FakeString errCodeStr = { errData, kErrorCodeChars - 1, kErrorCodeChars }; // Num excludes null
	FakeString emptyStr   = { nullptr, 0, 0 };

	// Create the 403 response.  Error() will move-from the FStrings (Data → null on move).
	void* response = nullptr;
	reinterpret_cast<FHttpServerResponseError_t>(g_errorFuncAddr)(
		&response, 403, &errCodeStr, &emptyStr);

	// Free error code data if not taken (i.e. Error() copied rather than moved).
	if (errCodeStr.Data)
		hooks->EngineFree(errCodeStr.Data);

	if (!response)
	{
		LOG_WARN("[RemoteVulnerabilityPatcher] FHttpServerResponse::Error returned null — connection dropped without response");
		return;
	}

	// The response callback asserts state == AwaitingProcessing (2).
	// The original ProcessRequest calls FHttpConnection::ChangeState(a1, 2) as its
	// first action — which we bypassed.  State is stored at a1+24 (confirmed from
	// the pseudocode assertion: *(_DWORD*)(a1+24) != 1).
	__try { *reinterpret_cast<int32_t*>(a1 + 24) = 2; }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LOG_ERROR("[RemoteVulnerabilityPatcher] Failed to set connection state — connection dropped without response");
		return;
	}

	// Invoke the TFunction<void(TUniquePtr<FHttpServerResponse>&&)> callback.
	// Replicates the invocation sequence from the ProcessRequest pseudocode exactly.
	__try
	{
		CallbackInvokeFn  invoke  = *reinterpret_cast<CallbackInvokeFn*>(a3);
		__int64           storage =  a3 + 16;

		if (*reinterpret_cast<__int64*>(a3 + 40))
			storage = *reinterpret_cast<__int64*>(a3 + 40);

		__int64*           vtable    = *reinterpret_cast<__int64**>(storage);
		CallbackGetTargetFn getTarget =  reinterpret_cast<CallbackGetTargetFn>(vtable[1]);
		__int64            target    =  getTarget(storage);

		invoke(target, &response);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LOG_ERROR("[RemoteVulnerabilityPatcher] Exception during response callback invocation");
	}
}

// ---------------------------------------------------------------------------
// Detour
// ---------------------------------------------------------------------------
static void __fastcall Hook_ProcessRequest(__int64 a1, uint64_t* a2, __int64 a3)
{
	if (a2)
	{
		uint64_t p = 0;
		if (TryReadBytes(reinterpret_cast<uintptr_t>(a2), &p, sizeof(p)) == sizeof(p) && p != 0)
		{
			std::string url  = ReadFStringUtf8(static_cast<uintptr_t>(p), URL_OFFSET);
			std::string body = ReadByteArray(static_cast<uintptr_t>(p), BODY_OFFSET);

			// Security: block unauthorized /remote/object/call requests
			if (ServerUtilityConfig::Config::GetRemoteVulnerabilityPatch()
				&& url == REMOTE_CALL_URL)
			{
				std::string objectPath   = ExtractJsonString(body, "objectPath");
				std::string functionName = ExtractJsonString(body, "functionName");

				if (objectPath.find(ALLOWED_OBJECT_PATH) == std::string::npos)
				{
					LOG_WARN("[RemoteVulnerabilityPatcher] Blocked unauthorized /remote/object/call");
					LOG_WARN("[RemoteVulnerabilityPatcher]   objectPath:   '%s'", objectPath.c_str());
					LOG_WARN("[RemoteVulnerabilityPatcher]   functionName: '%s'", functionName.c_str());
					SendBlockedResponse(a1, a3);
					return;
				}
			}
		}
	}

	if (g_originalProcessRequest)
		g_originalProcessRequest(a1, a2, a3);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void HttpConnectionHook::Install()
{
	auto* scanner = GetScanner();
	auto* hooks   = GetHooks();
	if (!scanner || !hooks)
	{
		LOG_ERROR("[RemoteVulnerabilityPatcher] Scanner or hooks interface not available");
		return;
	}

	LOG_INFO("[RemoteVulnerabilityPatcher] Scanning for FHttpConnection::ProcessRequest...");

	uintptr_t addr = scanner->FindPatternInMainModule(PROCESS_REQUEST_PATTERN);
	if (addr == 0)
	{
		LOG_ERROR("[RemoteVulnerabilityPatcher] Pattern scan failed - could not locate FHttpConnection::ProcessRequest");
		return;
	}

	LOG_INFO("[RemoteVulnerabilityPatcher] Found FHttpConnection::ProcessRequest at 0x%llX",
		static_cast<unsigned long long>(addr));

	// Scan ProcessRequest's own bytes to find FHttpServerResponse::Error.
	// The function calls Error(out, 404, errCode, errMsg) for the not-found path —
	// we look for "mov edx, 0x194" (BA 94 01 00 00) then the following E8 CALL.
	g_errorFuncAddr = FindErrorFunction(addr);
	if (g_errorFuncAddr)
		LOG_INFO("[RemoteVulnerabilityPatcher] Found FHttpServerResponse::Error at 0x%llX",
			static_cast<unsigned long long>(g_errorFuncAddr));
	else
		LOG_WARN("[RemoteVulnerabilityPatcher] Could not locate FHttpServerResponse::Error — blocked requests will drop the connection instead of receiving a 403");

	g_hookHandle = hooks->InstallHook(
		addr,
		reinterpret_cast<void*>(&Hook_ProcessRequest),
		reinterpret_cast<void**>(&g_originalProcessRequest));

	if (!g_hookHandle)
	{
		LOG_ERROR("[RemoteVulnerabilityPatcher] InstallHook failed!");
		return;
	}

	LOG_INFO("[RemoteVulnerabilityPatcher] Hook installed successfully (handle=%p)", g_hookHandle);
}

void HttpConnectionHook::Remove()
{
	if (!g_hookHandle)
	{
		LOG_DEBUG("[RemoteVulnerabilityPatcher] No hook installed - nothing to remove");
		return;
	}

	LOG_INFO("[RemoteVulnerabilityPatcher] Removing hook (handle=%p)...", g_hookHandle);

	auto* hooks = GetHooks();
	if (hooks && hooks->RemoveHook)
		hooks->RemoveHook(g_hookHandle);
	else
		LOG_WARN("[RemoteVulnerabilityPatcher] Hook interface not available - cannot remove hook cleanly");

	g_hookHandle             = nullptr;
	g_originalProcessRequest = nullptr;
	g_errorFuncAddr          = 0;

	LOG_INFO("[RemoteVulnerabilityPatcher] Hook removed successfully");
}
