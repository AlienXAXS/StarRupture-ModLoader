#include "pch.h"
#include "http_server_hook.h"

#include "hooks/hooks_common.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "hooks/memory/engine_allocator.h"
#include "hooks/game/scan_patterns.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <mutex>
#include <cwchar>
#include <filesystem>
#include <fstream>

namespace Hooks::HttpServer
{

	// ---------------------------------------------------------------------------
	// FHttpConnection::ProcessRequest — see ScanPatterns::FHttpConnection_ProcessRequest
	//
	// Signature (from IDA):
	//   void __fastcall FHttpConnection::ProcessRequest(__int64 a1, _QWORD *a2, __int64 a3)
	// a2  : TSharedPtr<FHttpServerRequest> — *a2 is the raw request pointer.
	// a3  : TFunction<void(TUniquePtr<FHttpServerResponse>&&)> — response callback.
	//
	// Confirmed FHttpServerRequest offsets (verified experimentally):
	//   obj+0   : HTTP verb  FString { wchar_t* Data @+0,  int32 Num @+8  }
	//   obj+16  : RelativePath FString { wchar_t* Data @+16, int32 Num @+24 }
	//   obj+280 : Body TArray<uint8> { uint8* Data @+280, int32 Num @+288 }
	// ---------------------------------------------------------------------------

	static constexpr size_t k_VerbOffset = 0;
	static constexpr size_t k_UrlOffset = 16;
	static constexpr size_t k_BodyOffset = 280;

	// ---------------------------------------------------------------------------
	// Hook state
	// ---------------------------------------------------------------------------
	using ProcessRequest_t = void(__fastcall*)(__int64 a1, uint64_t* a2, __int64 a3);
	static ProcessRequest_t g_original = nullptr;
	static Hooks::Hook      g_hook;

	// FHttpServerResponse::Error — found by scanning ProcessRequest for mov edx, 404 + CALL
	using FHttpServerResponseError_t = void(__fastcall*)(void** outPtr, int32_t code, void* errCode, void* errMsg);
	static uintptr_t g_errorFuncAddr = 0;

	// FHttpServerResponse::Create — confirmed via IDA (FPerfCounters::ProcessStatsRequest usage).
	// Signature (actual ABI, MSVC x64):
	//   __int64* __fastcall Create(__int64* retStorage,  // RCX — hidden return-value ptr
	//       const FString& body,  // RDX — body as FString (UTF-16)
	//    const FString& contentType) // R8 — content-type FString
	// Both body and content-type are FString (wchar_t*, int32 Num, int32 Max).
	using FHttpServerResponseCreate_t = void(__fastcall*)(__int64* retStorage,
		void* body,
		void* contentType);
	static uintptr_t g_createFuncAddr = 0;

	// ---------------------------------------------------------------------------
	// Route registry
	// ---------------------------------------------------------------------------
	struct RouteEntry
	{
		std::string  pluginName; // original casing (for logging)
		std::string  folderName; // original casing (for logging)
		std::wstring fsRoot;     // absolute path on disk: <exe_dir>\Plugins\<pluginName>\<folderName>
		std::string  urlPrefix;  // lowercased, no /plugins/ prefix: "/<pluginName>/<FolderName>/"
	};

	static std::vector<RouteEntry> g_routes;
	static std::mutex g_routesMutex;

	// ---------------------------------------------------------------------------
	// Raw-response route registry (v22)
	/// SEH-only trampoline — no C++ objects in scope so __try is legal.
	/// Calls the plugin's route callback; on exception signals a 500 should be sent.
	// ---------------------------------------------------------------------------

	struct RawRouteEntry
	{
		std::string pluginName; // original casing (for logging)
		std::string urlPrefix;  // lowercased: "/<pluginName>/<urlPrefix>/"
		PluginHttpRouteCallback callback;
	};

	static std::vector<RawRouteEntry> g_rawRoutes;
	static std::mutex g_rawRoutesMutex;

	// ---------------------------------------------------------------------------
	// Raw-request filter registry
	// ---------------------------------------------------------------------------
	static std::vector<PluginHttpRequestFilterCallback> g_filters;
	static std::mutex g_filtersMutex;

	// ---------------------------------------------------------------------------
	// TFunction callback type helpers
	// ---------------------------------------------------------------------------
	using CallbackInvokeFn = void(__fastcall*)(__int64 callable, void** response);
	using CallbackGetTargetFn = __int64(__fastcall*)(__int64 storage);

	// ---------------------------------------------------------------------------
	// FString / TArray mirror structs (must exactly match UE5 x64 layout)
	// ---------------------------------------------------------------------------
	struct FakeString
	{
		wchar_t* Data;
		int32_t  Num;
		int32_t  Max;
	};

	struct FakeByteArray
	{
		uint8_t* Data;
		int32_t  Num;
		int32_t  Max;
	};

	// ===========================================================================
	// SEH-only helpers — plain functions, no C++ object unwinding, __try is legal.
	// ===========================================================================

	static bool IsReadableMemory(uintptr_t addr, size_t size)
	{
		if (!addr || !size) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
		if (mbi.State != MEM_COMMIT) return false;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
		return addr + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	}

	static size_t TryReadBytesSEH(uintptr_t addr, void* out, size_t n)
	{
		__try { memcpy(out, reinterpret_cast<const void*>(addr), n); return n; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	static bool SetConnectionStateSEH(__int64 a1)
	{
		__try { *reinterpret_cast<int32_t*>(a1 + 24) = 2; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static bool InvokeResponseCallbackSEH(__int64 a3, void** response)
	{
		__try
		{
			CallbackInvokeFn invoke = *reinterpret_cast<CallbackInvokeFn*>(a3);
			__int64 storage = a3 + 16;
			if (*reinterpret_cast<__int64*>(a3 + 40))
				storage = *reinterpret_cast<__int64*>(a3 + 40);
			__int64* vtable = *reinterpret_cast<__int64**>(storage);
			auto getTarget = reinterpret_cast<CallbackGetTargetFn>(vtable[1]);
			__int64 target = getTarget(storage);
			invoke(target, response);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static HttpRequestAction InvokeFilterSEH(PluginHttpRequestFilterCallback cb,
		const PluginHttpRequest* req)
	{
		__try { return cb(req); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return HttpRequestAction::Approve; }
	}

	// ===========================================================================
	// Higher-level helpers — may use C++ objects
	// ===========================================================================

	static size_t TryReadBytes(uintptr_t addr, void* out, size_t len)
	{
		if (!IsReadableMemory(addr, 1)) return 0;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return 0;
		uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		size_t readable = (regionEnd > addr) ? (regionEnd - addr) : 0;
		size_t n = len < readable ? len : readable;
		return TryReadBytesSEH(addr, out, n);
	}

	static std::string ReadFStringUtf8Lower(uintptr_t req, size_t offset)
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
		for (size_t i = 0; i < len; ++i) wbuf[i] = static_cast<wchar_t>(towlower(wbuf[i]));
		int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(len), nullptr, 0, nullptr, nullptr);
		if (needed <= 0) return {};
		std::string out(needed, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(len), &out[0], needed, nullptr, nullptr);
		return out;
	}

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
		int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(len), nullptr, 0, nullptr, nullptr);
		if (needed <= 0) return {};
		std::string out(needed, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(len), &out[0], needed, nullptr, nullptr);
		return out;
	}

	static std::string ReadByteArray(uintptr_t req, size_t offset)
	{
		uintptr_t dataPtr = 0;
		if (TryReadBytes(req + offset, &dataPtr, sizeof(dataPtr)) != sizeof(dataPtr) || !dataPtr)
			return {};
		int32_t num = 0;
		if (TryReadBytes(req + offset + 8, &num, sizeof(num)) != sizeof(num) ||
			num <= 0 || num > 16 * 1024 * 1024)
			return {};
		if (!IsReadableMemory(dataPtr, static_cast<size_t>(num))) return {};
		std::string out(static_cast<size_t>(num), '\0');
		if (TryReadBytes(dataPtr, &out[0], static_cast<size_t>(num)) != static_cast<size_t>(num))
			return {};
		return out;
	}

	static HttpMethod ParseVerb(const std::string& v)
	{
		if (v == "get")     return HttpMethod::Get;
		if (v == "post")    return HttpMethod::Post;
		if (v == "put")     return HttpMethod::Put;
		if (v == "delete")  return HttpMethod::Delete;
		if (v == "patch")   return HttpMethod::Patch;
		if (v == "options") return HttpMethod::Options;
		if (v == "head")    return HttpMethod::Head;
		return HttpMethod::Other;
	}

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
	// FindCreateFunction — direct pattern scan for FHttpServerResponse::Create.
	// Falls back to the heuristic scan of ProcessRequest if the pattern is not
	// found (e.g. on a game update before the pattern is refreshed).
	// ---------------------------------------------------------------------------
	static uintptr_t FindCreateFunction(uintptr_t processRequestAddr, uintptr_t errorFuncAddr)
	{
		// Primary: direct pattern scan — most reliable.
		uintptr_t direct = Scanner::FindPatternInMainModule(
			"FHttpServerResponse::Create", ScanPatterns::FHttpServerResponse_Create);
		if (direct)
		{
			ModLoaderLogger::LogDebug(L"[HttpServer] FindCreateFunction: found via direct pattern scan at 0x%llX",
				static_cast<unsigned long long>(direct));
			return direct;
		}
		ModLoaderLogger::LogWarn(L"[HttpServer] FindCreateFunction: direct pattern scan failed, trying heuristics");

		// Fallback Strategy A: scan ProcessRequest for "mov edx, 0C8h" (200) + nearby E8 CALL.
		uint8_t buf[8192] = {};
		size_t got = TryReadBytes(processRequestAddr, buf, sizeof(buf));
		if (got < 16) return 0;

		static const uint8_t kMov200[] = { 0xBA, 0xC8, 0x00, 0x00, 0x00 };
		for (size_t i = 0; i + 40 < got; ++i)
		{
			if (memcmp(buf + i, kMov200, sizeof(kMov200)) != 0) continue;
			for (size_t j = i + sizeof(kMov200); j + 5 <= got && j < i + 40; ++j)
			{
				if (buf[j] != 0xE8) continue;
				int32_t rel = 0;
				memcpy(&rel, buf + j + 1, sizeof(rel));
				uintptr_t target = processRequestAddr + j + 5 + static_cast<intptr_t>(rel);
				if (target == errorFuncAddr || !IsReadableMemory(target, 16)) continue;
				ModLoaderLogger::LogDebug(L"[HttpServer] FindCreateFunction: found via Strategy A at 0x%llX",
					static_cast<unsigned long long>(target));
				return target;
			}
		}

		// Fallback Strategy B: proximity to Error + prologue filter.
		if (!errorFuncAddr) return 0;
		static constexpr uintptr_t kProximityWindow = 512 * 1024;
		for (size_t i = 0; i + 5 <= got; ++i)
		{
			if (buf[i] != 0xE8) continue;
			int32_t rel = 0;
			memcpy(&rel, buf + i + 1, sizeof(rel));
			uintptr_t target = processRequestAddr + i + 5 + static_cast<intptr_t>(rel);
			if (target == errorFuncAddr || !IsReadableMemory(target, 16)) continue;
			uintptr_t dist = (target > errorFuncAddr) ? (target - errorFuncAddr) : (errorFuncAddr - target);
			if (dist > kProximityWindow) continue;
			uint8_t prologue[5] = {};
			if (TryReadBytes(target, prologue, 5) == 5 &&
				prologue[0] == 0xBA && prologue[2] == 0x00 && prologue[3] == 0x00 && prologue[4] == 0x00)
				continue;
			ModLoaderLogger::LogDebug(L"[HttpServer] FindCreateFunction: found via Strategy B at 0x%llX",
				static_cast<unsigned long long>(target));
			return target;
		}
		return 0;
	}

	static void InvokeResponseCallback(__int64 a3, void** response)
	{
		if (!InvokeResponseCallbackSEH(a3, response))
			ModLoaderLogger::LogError(L"[HttpServer] Exception invoking response callback");
	}

	// Returns true for MIME types whose body bytes must NOT be round-tripped
	// through UTF-16.  MultiByteToWideChar corrupts arbitrary binary data.
	static bool IsBinaryMimeType(const wchar_t* mime)
	{
		if (!mime) return true;
		// Text types can safely pass through FString (UTF-16 round-trip).
		// Everything else (images, binary formats, wasm, etc.) is opaque binary.
		return !(wcsncmp(mime, L"text/", 5) == 0 ||
			wcscmp(mime, L"application/javascript") == 0 ||
			wcscmp(mime, L"application/json") == 0 ||
			wcscmp(mime, L"image/svg+xml") == 0);
	}

	// Offset of the TArray<uint8> Body field inside FHttpServerResponse.
	// UE5 layout (x64): Code(int32)@0, Headers(TMap)@8 (0x50 bytes), Body@0x58.
	// Verified experimentally: Create() with an empty FString body leaves
	// Body.Data==null and Body.Num==0 at this offset.
	static constexpr size_t k_ResponseBodyOffset = 0x58;

	static void SendBlockedResponse(__int64 a1, __int64 a3)
	{
		ModLoaderLogger::LogTrace(L"[HttpServer] SendBlockedResponse: building 403 response");
		if (!g_errorFuncAddr)
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Error not located — dropping connection");
			return;
		}
		if (!EngineAllocator::IsAvailable())
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] Engine allocator unavailable — dropping connection");
			return;
		}

		static constexpr wchar_t kErrCode[] = L"errors.com.epicgames.httpserver.forbidden";
		static constexpr int32_t kErrCodeChars = static_cast<int32_t>(sizeof(kErrCode) / sizeof(wchar_t));

		auto errData = static_cast<wchar_t*>(EngineAllocator::Alloc(sizeof(kErrCode), 4));
		if (!errData) { ModLoaderLogger::LogWarn(L"[HttpServer] EngineAlloc failed — dropping connection"); return; }
		memcpy(errData, kErrCode, sizeof(kErrCode));

		FakeString errCodeStr = { errData, kErrCodeChars - 1, kErrCodeChars };
		FakeString emptyStr = { nullptr, 0, 0 };

		void* response = nullptr;
		reinterpret_cast<FHttpServerResponseError_t>(g_errorFuncAddr)(&response, 403, &errCodeStr, &emptyStr);
		if (errCodeStr.Data) EngineAllocator::Free(errCodeStr.Data);

		if (!response)
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Error returned null — dropping connection");
			return;
		}
		ModLoaderLogger::LogTrace(L"[HttpServer] SendBlockedResponse: FHttpServerResponse::Error returned response=0x%p, setting connection state", response);
		if (!SetConnectionStateSEH(a1))
		{
			ModLoaderLogger::LogError(L"[HttpServer] Failed to set connection state for 403");
			return;
		}
		ModLoaderLogger::LogTrace(L"[HttpServer] SendBlockedResponse: invoking response callback (403)");
		InvokeResponseCallback(a3, &response);
		ModLoaderLogger::LogTrace(L"[HttpServer] SendBlockedResponse: done");
	}

	static void SendFileResponse(__int64 a1, __int64 a3,
		const std::vector<uint8_t>& body, const wchar_t* contentType)
	{
		ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse: body=%zu bytes, content-type=%s", body.size(), contentType);
		if (!g_createFuncAddr)
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Create not located — cannot serve file");
			return;
		}
		if (!EngineAllocator::IsAvailable())
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] Engine allocator unavailable — cannot serve file");
			return;
		}

		// Content-type FString (always needed).
		const size_t ctLen = wcslen(contentType) + 1;
		wchar_t* ctData = static_cast<wchar_t*>(EngineAllocator::Alloc(ctLen * sizeof(wchar_t), 4));
		if (!ctData) { ModLoaderLogger::LogWarn(L"[HttpServer] EngineAlloc for content-type failed"); return; }
		memcpy(ctData, contentType, ctLen * sizeof(wchar_t));
		FakeString ctStr = { ctData, static_cast<int32_t>(ctLen), static_cast<int32_t>(ctLen) };

		if (IsBinaryMimeType(contentType))
		{
			// ---------------------------------------------------------------
			// Binary path: call Create with an empty body FString, then write
			// the raw bytes directly into the response object's Body TArray.
			// This avoids the MultiByteToWideChar round-trip that corrupts
			// arbitrary binary data (PNG, JPEG, WASM, etc.).
			// ---------------------------------------------------------------
			FakeString emptyBody = { nullptr, 0, 0 };

			void* responseStorage = nullptr;
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (binary): calling Create with empty body (0x%llX)",
				static_cast<unsigned long long>(g_createFuncAddr));
			reinterpret_cast<FHttpServerResponseCreate_t>(g_createFuncAddr)(
				reinterpret_cast<__int64*>(&responseStorage), &emptyBody, &ctStr);
			EngineAllocator::Free(ctData);

			if (!responseStorage)
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Create returned null (binary path)");
				return;
			}

			// Patch Body TArray<uint8> in-place with the raw file bytes.
			if (!body.empty())
			{
				uint8_t* rawData = static_cast<uint8_t*>(EngineAllocator::Alloc(body.size(), 1));
				if (!rawData)
				{
					ModLoaderLogger::LogWarn(L"[HttpServer] EngineAlloc for binary body failed (%zu bytes)", body.size());
					// Fall through — response will have an empty body but correct status/content-type.
				}
				else
				{
					memcpy(rawData, body.data(), body.size());
					// Write directly into the response object's TArray<uint8> Body field.
					auto* bodyArray = reinterpret_cast<FakeByteArray*>(
						reinterpret_cast<uint8_t*>(responseStorage) + k_ResponseBodyOffset);
					bodyArray->Data = rawData;
					bodyArray->Num  = static_cast<int32_t>(body.size());
					bodyArray->Max  = static_cast<int32_t>(body.size());
					ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (binary): patched Body TArray: Data=%p Num=%d",
						rawData, bodyArray->Num);
				}
			}

			void* response = responseStorage;
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (binary): response=0x%p, setting connection state", response);
			if (!SetConnectionStateSEH(a1))
			{
				ModLoaderLogger::LogError(L"[HttpServer] Failed to set connection state for 200 (binary)");
				return;
			}
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (binary): invoking response callback");
			InvokeResponseCallback(a3, &response);
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (binary): done");
		}
		else
		{
			// ---------------------------------------------------------------
			// Text path: convert body bytes (UTF-8) to UTF-16 for FString.
			// Safe for HTML, CSS, JS, JSON, SVG, plain text.
			// ---------------------------------------------------------------
			int bodyWideLen = 0;
			wchar_t* bodyData = nullptr;
			if (!body.empty())
			{
				bodyWideLen = MultiByteToWideChar(CP_UTF8, 0,
					reinterpret_cast<const char*>(body.data()), static_cast<int>(body.size()),
					nullptr, 0);
				if (bodyWideLen > 0)
				{
					bodyData = static_cast<wchar_t*>(EngineAllocator::Alloc(
						static_cast<size_t>(bodyWideLen + 1) * sizeof(wchar_t), 4));
					if (!bodyData) { EngineAllocator::Free(ctData); ModLoaderLogger::LogWarn(L"[HttpServer] EngineAlloc for body failed"); return; }
					MultiByteToWideChar(CP_UTF8, 0,
						reinterpret_cast<const char*>(body.data()), static_cast<int>(body.size()),
						bodyData, bodyWideLen);
					bodyData[bodyWideLen] = L'\0';
				}
				else bodyWideLen = 0;
				ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (text): body wide len=%d, allocated at 0x%p", bodyWideLen, bodyData);
			}

			const int32_t bodyNum = bodyData ? (bodyWideLen + 1) : 0;
			FakeString bodyStr = { bodyData, bodyNum, bodyNum };

			void* responseStorage = nullptr;
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (text): calling FHttpServerResponse::Create (0x%llX)",
				static_cast<unsigned long long>(g_createFuncAddr));
			reinterpret_cast<FHttpServerResponseCreate_t>(g_createFuncAddr)(
				reinterpret_cast<__int64*>(&responseStorage), &bodyStr, &ctStr);
			if (ctData)   EngineAllocator::Free(ctData);
			if (bodyData) EngineAllocator::Free(bodyData);

			void* response = responseStorage;
			if (!response)
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Create returned null — cannot serve file");
				return;
			}
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (text): Create returned response=0x%p, setting connection state", response);
			if (!SetConnectionStateSEH(a1))
			{
				ModLoaderLogger::LogError(L"[HttpServer] Failed to set connection state for 200");
				return;
			}
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (text): invoking response callback (200)");
			InvokeResponseCallback(a3, &response);
			ModLoaderLogger::LogTrace(L"[HttpServer] SendFileResponse (text): done");
		}
	}

	// ---------------------------------------------------------------------------
	// Send an arbitrary response via FHttpServerResponse::Create / Error
	// Used by raw-response routes so the plugin doesn't need its own response construction.
	// ---------------------------------------------------------------------------
	static void SendRawResponse(__int64 a1, __int64 a3,
		int statusCode, const char* contentType,
		const char* body, size_t bodyLen)
	{
		if (!contentType || !contentType[0]) contentType = "text/plain";
		if (!body) bodyLen = 0;

		ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: status=%d content-type=%S body_len=%zu",
			statusCode, contentType, bodyLen);

		if (statusCode != 200)
		{
			if (!g_errorFuncAddr)
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] SendRawResponse: FHttpServerResponse::Error not located, dropping");
				return;
			}
			if (!EngineAllocator::IsAvailable())
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] SendRawResponse: allocator unavailable, dropping");
				return;
			}

			char codeStr[32]{};
			snprintf(codeStr, sizeof(codeStr), "errors.com.epicgames.httpserver.%d", statusCode);
			wchar_t wCodeStr[64]{};
			int wLen = MultiByteToWideChar(CP_UTF8, 0, codeStr, -1, wCodeStr, 64);
			if (wLen <= 1) wLen = 1;

			auto codeData = static_cast<wchar_t*>(EngineAllocator::Alloc(wLen * sizeof(wchar_t), 4));
			if (!codeData) return;
			memcpy(codeData, wCodeStr, wLen * sizeof(wchar_t));

			FakeString codeFs = { codeData, wLen - 1, wLen };
			FakeString emptyFs = { nullptr, 0, 0 };

			ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: calling FHttpServerResponse::Error (0x%llX), code=%S",
				static_cast<unsigned long long>(g_errorFuncAddr), wCodeStr);
			void* response = nullptr;
			reinterpret_cast<FHttpServerResponseError_t>(g_errorFuncAddr)(
				&response, statusCode, &codeFs, &emptyFs);
			EngineAllocator::Free(codeData);

			if (!response)
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] SendRawResponse: FHttpServerResponse::Error returned null, dropping");
				return;
			}
			ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: error response=0x%p, setting state + invoking callback", response);
			if (!SetConnectionStateSEH(a1)) return;
			InvokeResponseCallback(a3, &response);
			ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: done (error path)");
			return;
		}

		// --- 200 OK path ---
		if (!g_createFuncAddr)
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] SendRawResponse: FHttpServerResponse::Create not located, dropping");
			return;
		}
		if (!EngineAllocator::IsAvailable())
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] SendRawResponse: allocator unavailable, dropping");
			return;
		}

		// Create takes FString* for body — convert UTF-8 body bytes to UTF-16.
		wchar_t* bodyData = nullptr;
		if (bodyLen > 0)
		{
			int bodyWideLen = MultiByteToWideChar(CP_UTF8, 0, body, static_cast<int>(bodyLen), nullptr, 0);
			if (bodyWideLen > 0)
			{
				bodyData = static_cast<wchar_t*>(EngineAllocator::Alloc(
					static_cast<size_t>(bodyWideLen + 1) * sizeof(wchar_t), 4));
				if (!bodyData) return;
				MultiByteToWideChar(CP_UTF8, 0, body, static_cast<int>(bodyLen), bodyData, bodyWideLen);
				bodyData[bodyWideLen] = L'\0';
			}
			ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: body wide len=%d allocated at 0x%p", bodyWideLen, bodyData);
		}

		// Content-type (wide)
		int ctNeeded = MultiByteToWideChar(CP_UTF8, 0, contentType, -1, nullptr, 0);
		if (ctNeeded <= 0) ctNeeded = 1;
		auto ctData = static_cast<wchar_t*>(EngineAllocator::Alloc(static_cast<size_t>(ctNeeded) * sizeof(wchar_t), 4));
		if (!ctData) { if (bodyData) EngineAllocator::Free(bodyData); return; }
		MultiByteToWideChar(CP_UTF8, 0, contentType, -1, ctData, ctNeeded);

		// FString::Num includes the null terminator. Num=0 when Data is null.
		const int32_t bodyNum = bodyData ? (static_cast<int32_t>(bodyLen) + 1) : 0;
		FakeString bodyFs = { bodyData, bodyNum,   bodyNum };
		FakeString ctFs = { ctData,   ctNeeded,  ctNeeded };

		ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: calling FHttpServerResponse::Create (0x%llX)",
			static_cast<unsigned long long>(g_createFuncAddr));
		void* response = nullptr;
		reinterpret_cast<FHttpServerResponseCreate_t>(g_createFuncAddr)(
			reinterpret_cast<__int64*>(&response), &bodyFs, &ctFs);
		EngineAllocator::Free(ctData);
		if (bodyData) EngineAllocator::Free(bodyData);

		if (!response)
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] SendRawResponse: FHttpServerResponse::Create returned null, dropping");
			return;
		}
		ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: ok response=0x%p, setting state + invoking callback", response);
		if (!SetConnectionStateSEH(a1)) return;
		InvokeResponseCallback(a3, &response);
		ModLoaderLogger::LogTrace(L"[HttpServer] SendRawResponse: done (ok path)");
	}

	// ---------------------------------------------------------------------------
	// MIME type table
	// ---------------------------------------------------------------------------
	static const wchar_t* GetMimeType(std::wstring_view ext)
	{
		if (ext == L".html" || ext == L".htm") return L"text/html";
		if (ext == L".css")return L"text/css";
		if (ext == L".js")   return L"application/javascript";
		if (ext == L".json") return L"application/json";
		if (ext == L".png")  return L"image/png";
		if (ext == L".jpg" || ext == L".jpeg") return L"image/jpeg";
		if (ext == L".gif")  return L"image/gif";
		if (ext == L".svg")  return L"image/svg+xml";
		if (ext == L".ico")  return L"image/x-icon";
		if (ext == L".wasm") return L"application/wasm";
		if (ext == L".txt")  return L"text/plain";
		if (ext == L".xml")  return L"text/xml";
		return L"application/octet-stream";
	}

	// ---------------------------------------------------------------------------
	// Path safety
	// ---------------------------------------------------------------------------
	static bool IsSafeUrlPath(std::string_view path)
	{
		if (path.find("..") != std::string_view::npos) return false;
		if (path.find('\\') != std::string_view::npos) return false;
		return true;
	}

	static std::string UrlDecode(std::string_view in)
	{
		std::string out;
		out.reserve(in.size());
		for (size_t i = 0; i < in.size(); ++i)
		{
			if (in[i] == '%' && i + 2 < in.size())
			{
				auto hexVal = [](char c) -> int {
					if (c >= '0' && c <= '9') return c - '0';
					if (c >= 'a' && c <= 'f') return 10 + c - 'a';
					if (c >= 'A' && c <= 'F') return 10 + c - 'A';
					return -1;
					};
				int h = hexVal(in[i + 1]), l = hexVal(in[i + 2]);
				if (h >= 0 && l >= 0) { out += static_cast<char>((h << 4) | l); i += 2; continue; }
			}
			out += in[i];
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// Static-file route dispatch — Phase 2b
	// Returns true if the URL matched a route (served or blocked).
	/// Returns false to let the original engine handler produce a 404.
	// ---------------------------------------------------------------------------

	// Default documents tried in order when the URL resolves to a directory.
	static constexpr const char* k_DefaultDocs[] = { "index.html", "index.htm" };

	static bool TryServeStaticFile(__int64 a1, __int64 a3, const std::string& urlLower)
	{
		std::lock_guard lock(g_routesMutex);

		ModLoaderLogger::LogTrace(L"[HttpServer] TryServeStaticFile: checking %zu registered static route(s) for url=%S",
			g_routes.size(), urlLower.c_str());

		for (const auto& route : g_routes)
		{
			ModLoaderLogger::LogTrace(L"[HttpServer]   static route check: url=%S prefix=%S",
				urlLower.c_str(), route.urlPrefix.c_str());

			if (urlLower.size() < route.urlPrefix.size()) continue;
			if (urlLower.compare(0, route.urlPrefix.size(), route.urlPrefix) != 0) continue;

			ModLoaderLogger::LogTrace(L"[HttpServer]   static route matched: prefix=%S plugin=%S folder=%S",
				route.urlPrefix.c_str(), route.pluginName.c_str(), route.folderName.c_str());

			std::string rel = UrlDecode(urlLower.substr(route.urlPrefix.size()));
			// The trailing slash normalisation in Detour means urlLower always ends with '/'.
			// After stripping the prefix, rel will end with '/' when the prefix is an exact
			// match (directory request) or when the sub-path itself ends with '/'.
			// Strip that trailing slash from rel so the path logic below is clean.
			if (!rel.empty() && rel.back() == '/') rel.pop_back();

			ModLoaderLogger::LogTrace(L"[HttpServer]   relative path after decode+trim: '%S'", rel.c_str());

			if (!IsSafeUrlPath(rel))
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] Blocked path traversal attempt: %S", rel.c_str());
				SendBlockedResponse(a1, a3);
				return true;
			}

			// ---------------------------------------------------------------------------
			// Determine whether this looks like a directory or a file request.
			// A request is treated as a directory request (and triggers default-doc
			// probing) when:
			//   1. rel is empty           — bare prefix hit, e.g. /plugin/ui/
			//   2. rel has no extension   — e.g. /plugin/ui/dashboard  (no dot after last '/')
			// A request is treated as a direct file request otherwise.
			// ---------------------------------------------------------------------------
			auto IsDirectoryRequest = [](const std::string& r) -> bool
				{
					if (r.empty()) return true;
					// r has already had its trailing slash stripped by the caller, so the last
					// character is never '/'. Find the last path segment and check for a dot.
					// A segment with a dot is treated as a file (e.g. "index.html", "app.js").
					// A segment without a dot is treated as a directory (e.g. "dashboard", "").
					auto lastSlash = r.rfind('/');
					std::string_view lastSegment = (lastSlash == std::string::npos)
						? std::string_view(r)
						: std::string_view(r).substr(lastSlash + 1);
					// If the last segment is empty (shouldn't happen after trimming, but be safe)
					// or contains no dot, treat as directory.
					return lastSegment.empty() || lastSegment.find('.') == std::string_view::npos;
				};

			const bool isDirRequest = IsDirectoryRequest(rel);
			ModLoaderLogger::LogTrace(L"[HttpServer]   request type: %s", isDirRequest ? L"directory" : L"file");

			// Helper: build, safety-check, and if valid return the resolved fs path.
			// Returns an empty path on any safety violation (and sends 403 internally).
			auto ResolvePath = [&](const std::string& relPath) -> std::filesystem::path
				{
					std::wstring wRel(relPath.begin(), relPath.end());
					std::filesystem::path resolved = (std::filesystem::path(route.fsRoot) / wRel).lexically_normal();

					auto rootNorm = std::filesystem::path(route.fsRoot).lexically_normal();
					auto [rootEnd, _] = std::mismatch(rootNorm.begin(), rootNorm.end(),
						resolved.begin(), resolved.end());
					if (rootEnd != rootNorm.end())
					{
						ModLoaderLogger::LogWarn(L"[HttpServer] Path escaped route root, blocked: %s", resolved.c_str());
						SendBlockedResponse(a1, a3);
						return {};
					}
					return resolved;
				};

			// Helper: read and serve a resolved path. Returns true on success.
			auto ServeFile = [&](const std::filesystem::path& fsPath) -> bool
				{
					ModLoaderLogger::LogTrace(L"[HttpServer]   opening file: %s", fsPath.c_str());

					std::ifstream file(fsPath, std::ios::binary | std::ios::ate);
					if (!file.is_open())
					{
						ModLoaderLogger::LogTrace(L"[HttpServer]   file not found: %s", fsPath.c_str());
						return false;
					}

					std::streamsize size = file.tellg();
					ModLoaderLogger::LogTrace(L"[HttpServer]   file opened, size=%lld bytes", static_cast<long long>(size));
					file.seekg(0, std::ios::beg);

					if (size < 0 || size > 32 * 1024 * 1024)
					{
						ModLoaderLogger::LogWarn(L"[HttpServer] File too large: %s", fsPath.c_str());
						ModLoaderLogger::LogTrace(L"[HttpServer]   file size out of bounds (%lld), skipping", static_cast<long long>(size));
						return false;
					}

					std::vector<uint8_t> body(static_cast<size_t>(size));
					if (!file.read(reinterpret_cast<char*>(body.data()), size))
					{
						ModLoaderLogger::LogWarn(L"[HttpServer] Failed to read file: %s", fsPath.c_str());
						ModLoaderLogger::LogTrace(L"[HttpServer]   file read failed, skipping");
						return false;
					}

					const wchar_t* mime = GetMimeType(fsPath.extension().wstring());
					ModLoaderLogger::LogDebug(L"[HttpServer] Serving %s (%zu bytes, %s)", fsPath.c_str(), body.size(), mime);
					ModLoaderLogger::LogTrace(L"[HttpServer]   dispatching to SendFileResponse: %zu bytes, mime=%s", body.size(), mime);
					SendFileResponse(a1, a3, body, mime);
					ModLoaderLogger::LogTrace(L"[HttpServer]   SendFileResponse returned");
					return true;
				};

			if (isDirRequest)
			{
				// Probe each default document in priority order.
				ModLoaderLogger::LogTrace(L"[HttpServer]   directory request — probing default documents under '%S'",
					rel.c_str());

				for (const char* defaultDoc : k_DefaultDocs)
				{
					std::string candidate = rel.empty() ? defaultDoc : (rel + "/" + defaultDoc);
					ModLoaderLogger::LogTrace(L"[HttpServer]   trying default doc: %S", candidate.c_str());

					std::filesystem::path fsPath = ResolvePath(candidate);
					if (fsPath.empty()) return true; // safety violation already handled (403 sent)

					if (ServeFile(fsPath))
					{
						ModLoaderLogger::LogTrace(L"[HttpServer]   served default doc '%S', TryServeStaticFile done", defaultDoc);
						return true;
					}
				}

				// No default document found — fall through to let the engine produce a 404.
				ModLoaderLogger::LogDebug(L"[HttpServer] No default document found under route %S for url=%S",
					route.urlPrefix.c_str(), urlLower.c_str());
				ModLoaderLogger::LogTrace(L"[HttpServer]   no default doc found, returning false");
				return false;
			}
			else
			{
				// Direct file request — resolve and serve exactly what was asked for.
				std::filesystem::path fsPath = ResolvePath(rel);
				if (fsPath.empty()) return true; // 403 already sent

				ModLoaderLogger::LogTrace(L"[HttpServer]   resolved fs path: %s", fsPath.c_str());

				if (ServeFile(fsPath))
				{
					ModLoaderLogger::LogTrace(L"[HttpServer]   TryServeStaticFile done (direct file)");
					return true;
				}

				// File genuinely not found — let the engine produce a 404.
				ModLoaderLogger::LogDebug(L"[HttpServer] File not found: %s", fsPath.c_str());
				ModLoaderLogger::LogTrace(L"[HttpServer]   file not found, returning false");
				return false;
			}
		}

		ModLoaderLogger::LogTrace(L"[HttpServer] TryServeStaticFile: no static route matched url=%S", urlLower.c_str());
		return false;
	}

	// ---------------------------------------------------------------------------
	// Raw-route dispatch — Phase 2a (runs before static-file Phase 2b)
	// Returns true if a raw route matched (even if the callback left an empty body).
	// ---------------------------------------------------------------------------

	/// SEH-only trampoline — no C++ objects in scope so __try is legal.
	/// Calls the plugin's route callback; on exception signals a 500 should be sent.
	static bool InvokeRawRouteCallbackSEH(PluginHttpRouteCallback cb,
		const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		__try { cb(req, resp); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// NOTE: TryServeStaticFile is defined above — no forward declaration needed.

	static bool TryDispatchRawRoute(__int64 a1, __int64 a3,
		const std::string& urlLower,
		const std::string& urlOrig,
		const std::string& verbLower,
		const std::string& bodyStr)
	{
		std::lock_guard lock(g_rawRoutesMutex);

		ModLoaderLogger::LogTrace(L"[HttpServer] TryDispatchRawRoute: checking %zu registered raw route(s) for url=%S",
			g_rawRoutes.size(), urlLower.c_str());

		for (const auto& route : g_rawRoutes)
		{
			ModLoaderLogger::LogTrace(L"[HttpServer]   raw route check: url=%S prefix=%S plugin=%S",
				urlLower.c_str(), route.urlPrefix.c_str(), route.pluginName.c_str());

			if (urlLower.size() < route.urlPrefix.size()) continue;
			if (urlLower.compare(0, route.urlPrefix.size(), route.urlPrefix) != 0) continue;

			ModLoaderLogger::LogDebug(L"[HttpServer] Raw route matched: %S -> %S",
				urlOrig.c_str(), route.pluginName.c_str());
			ModLoaderLogger::LogTrace(L"[HttpServer]   raw route matched: prefix=%S, invoking plugin callback",
				route.urlPrefix.c_str());

			PluginHttpRequest req{};
			req.url = urlOrig.c_str();
			req.body = bodyStr.empty() ? nullptr : bodyStr.c_str();
			req.bodyLen = bodyStr.size();
			req.method = ParseVerb(verbLower);

			PluginHttpResponse resp{};
			resp.statusCode = 200;
			resp.contentType = "text/plain";
			resp.body = nullptr;
			resp.bodyLen = 0;

			ModLoaderLogger::LogTrace(L"[HttpServer]   calling InvokeRawRouteCallbackSEH for prefix=%S",
				route.urlPrefix.c_str());
			if (!InvokeRawRouteCallbackSEH(route.callback, &req, &resp))
			{
				ModLoaderLogger::LogError(L"[HttpServer] Exception in raw-route callback for %S",
					route.urlPrefix.c_str());
				ModLoaderLogger::LogTrace(L"[HttpServer]   callback threw exception, sending 500");
				SendRawResponse(a1, a3, 500, "text/plain", nullptr, 0);
				return true;
			}

			ModLoaderLogger::LogTrace(L"[HttpServer]   callback returned: status=%d content-type=%S body_len=%zu",
				resp.statusCode,
				resp.contentType ? resp.contentType : "(null)",
				resp.bodyLen);
			// Modified to always use empty fsPath
			SendRawResponse(a1, a3, resp.statusCode, resp.contentType, resp.body, resp.bodyLen);
			ModLoaderLogger::LogTrace(L"[HttpServer]   TryDispatchRawRoute done for prefix=%S", route.urlPrefix.c_str());
			return true;
		}

		ModLoaderLogger::LogTrace(L"[HttpServer] TryDispatchRawRoute: no raw route matched url=%S", urlLower.c_str());
		return false;
	}

	// ---------------------------------------------------------------------------
	// Detour
	// ---------------------------------------------------------------------------
	static void __fastcall Detour(__int64 a1, uint64_t* a2, __int64 a3)
	{
		if (a2)
		{
			uint64_t p = 0;
			if (TryReadBytes(reinterpret_cast<uintptr_t>(a2), &p, sizeof(p)) == sizeof(p) && p != 0)
			{
				std::string verbLower = ReadFStringUtf8Lower(p, k_VerbOffset);
				std::string urlLower = ReadFStringUtf8Lower(p, k_UrlOffset);
				std::string urlOrig = ReadFStringUtf8(p, k_UrlOffset);
				std::string body = ReadByteArray(p, k_BodyOffset);

				// Normalise: strip query-string / fragment, then ensure a trailing slash
				// so that /foo/bar and /foo/bar/ both match the registered prefix /foo/bar/.
				// Query strings and fragments are not part of the path and must be removed
				// before prefix matching, otherwise /foo/bar?x=1 would never match /foo/bar/.
				auto StripSuffix = [](std::string& s)
					{
						auto q = s.find('?');
						if (q != std::string::npos) s.erase(q);
						auto f = s.find('#');
						if (f != std::string::npos) s.erase(f);
						if (s.empty() || s.back() != '/') s += '/';
					};
				StripSuffix(urlLower);
				// Apply the same normalisation to urlOrig (preserve original casing for logging,
				// but strip query/fragment and add trailing slash for consistent logging).
				{
					auto q = urlOrig.find('?');
					if (q != std::string::npos) urlOrig.erase(q);
					auto f = urlOrig.find('#');
					if (f != std::string::npos) urlOrig.erase(f);
					if (urlOrig.empty() || urlOrig.back() != '/') urlOrig += '/';
				}

				ModLoaderLogger::LogTrace(L"[HttpServer] URL normalised: lower=%S orig=%S",
					urlLower.c_str(), urlOrig.c_str());

				// ---------------------------------------------------------------------------
				// Trace: log every incoming HTTP request with full details
				// ---------------------------------------------------------------------------
				{
					std::wstring wVerb(verbLower.begin(), verbLower.end());
					std::wstring wUrl(urlOrig.begin(), urlOrig.end());

					ModLoaderLogger::LogTrace(
						L"[HttpServer] >> Incoming request: method=%s url=%s body_len=%zu a1=0x%llX a2=0x%llX a3=0x%llX",
						wVerb.empty() ? L"(unknown)" : wVerb.c_str(),
						wUrl.empty() ? L"(unknown)" : wUrl.c_str(),
						body.size(),
						static_cast<unsigned long long>(a1),
						reinterpret_cast<unsigned long long>(a2),
						static_cast<unsigned long long>(a3));

					if (!body.empty())
					{
						// Log up to 512 bytes of body as UTF-8 (converted to wide for the logger).
						// Truncate at the first embedded null so the string stays printable.
						constexpr size_t kMaxBodyLog = 512;
						size_t logLen = (std::min)(body.size(), kMaxBodyLog);
						// Clamp at embedded null
						size_t nullPos = body.find('\0');
						if (nullPos != std::string::npos && nullPos < logLen) logLen = nullPos;

						int wNeeded = MultiByteToWideChar(CP_UTF8, 0, body.c_str(), static_cast<int>(logLen), nullptr, 0);
						if (wNeeded > 0)
						{
							std::wstring wBody(static_cast<size_t>(wNeeded), L'\0');
							MultiByteToWideChar(CP_UTF8, 0, body.c_str(), static_cast<int>(logLen), wBody.data(), wNeeded);
							ModLoaderLogger::LogTrace(L"[HttpServer]    body (first %zu bytes)%s: %s",
								logLen,
								body.size() > kMaxBodyLog ? L" [truncated]" : L"",
								wBody.c_str());
						}
						else
						{
							ModLoaderLogger::LogTrace(L"[HttpServer]    body (%zu bytes, non-UTF-8 / binary)", body.size());
						}
					}
					else
					{
						ModLoaderLogger::LogTrace(L"[HttpServer] body: (empty)");
					}
				}

				// Phase 1: raw-request filters — first Deny wins
				{
					PluginHttpRequest req{};
					req.url = urlOrig.c_str();
					req.body = body.empty() ? nullptr : body.c_str();
					req.bodyLen = body.size();
					req.method = ParseVerb(verbLower);

					std::lock_guard lock(g_filtersMutex);
					ModLoaderLogger::LogTrace(L"[HttpServer] Phase 1: evaluating %zu filter(s)", g_filters.size());
					size_t filterIdx = 0;
					for (auto* cb : g_filters)
					{
						if (!cb) { ++filterIdx; continue; }
						ModLoaderLogger::LogTrace(L"[HttpServer]   filter[%zu]: invoking for url=%S", filterIdx, urlOrig.c_str());
						HttpRequestAction action = InvokeFilterSEH(cb, &req);
						ModLoaderLogger::LogTrace(L"[HttpServer]   filter[%zu]: result=%s", filterIdx,
							action == HttpRequestAction::Deny ? L"Deny" : L"Approve");
						if (action == HttpRequestAction::Deny)
						{
							ModLoaderLogger::LogDebug(L"[HttpServer] Request denied by filter: %S", urlOrig.c_str());
							ModLoaderLogger::LogTrace(L"[HttpServer]   filter[%zu] denied request, sending 403", filterIdx);
							SendBlockedResponse(a1, a3);
							return;
						}
						++filterIdx;
					}
					ModLoaderLogger::LogTrace(L"[HttpServer] Phase 1: all filters approved");
				}

				// Phase 2a: raw-response routes — plugin-owned handlers
				ModLoaderLogger::LogTrace(L"[HttpServer] Phase 2a: dispatching to raw routes");
				if (TryDispatchRawRoute(a1, a3, urlLower, urlOrig, verbLower, body))
				{
					ModLoaderLogger::LogTrace(L"[HttpServer] Phase 2a: raw route handled request, returning");
					return;
				}

				// Phase 2b: static-file routes
				ModLoaderLogger::LogTrace(L"[HttpServer] Phase 2b: dispatching to static-file routes");
				if (TryServeStaticFile(a1, a3, urlLower))
				{
					ModLoaderLogger::LogTrace(L"[HttpServer] Phase 2b: static-file route handled request, returning");
					return;
				}

				ModLoaderLogger::LogTrace(L"[HttpServer] Phase 2a+2b: no mod route matched, falling through to engine handler");
			}
		}

		// Phase 3: original engine handler
		ModLoaderLogger::LogTrace(L"[HttpServer] Phase 3: forwarding to original FHttpConnection::ProcessRequest (0x%llX)",
			reinterpret_cast<unsigned long long>(g_original));
		if (g_original) g_original(a1, a2, a3);
		ModLoaderLogger::LogTrace(L"[HttpServer] Phase 3: original handler returned");
	}

	// ---------------------------------------------------------------------------
	// Public API
	// ---------------------------------------------------------------------------

	bool Install()
	{
		if (g_hook.installed) return true;

		ModLoaderLogger::LogInfo(L"[HttpServer] Scanning for FHttpConnection::ProcessRequest...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"FHttpConnection::ProcessRequest", ScanPatterns::FHttpConnection_ProcessRequest);
		if (!addr)
		{
			ModLoaderLogger::LogError(L"[HttpServer] Pattern scan failed — hook not installed");
			return false;
		}

		auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		ModLoaderLogger::LogInfo(L"[HttpServer] Found FHttpConnection::ProcessRequest at 0x%llX (base+0x%llX)",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base));

		g_errorFuncAddr = FindErrorFunction(addr);
		if (g_errorFuncAddr)
			ModLoaderLogger::LogDebug(L"[HttpServer] Found FHttpServerResponse::Error at 0x%llX",
				static_cast<unsigned long long>(g_errorFuncAddr));
		else
			ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Error not found — denied requests will drop the connection");

		g_createFuncAddr = FindCreateFunction(addr, g_errorFuncAddr);
		if (g_createFuncAddr)
			ModLoaderLogger::LogDebug(L"[HttpServer] Found FHttpServerResponse::Create at 0x%llX",
				static_cast<unsigned long long>(g_createFuncAddr));
		else
			ModLoaderLogger::LogWarn(L"[HttpServer] FHttpServerResponse::Create not found — HTTP OK responses will not function");

		bool ok = g_hook.Install(addr, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original));

		if (ok) ModLoaderLogger::LogInfo(L"[HttpServer] Hook installed successfully");
		else    ModLoaderLogger::LogError(L"[HttpServer] Hook installation failed");
		return ok;
	}

	void Remove()
	{
		if (!g_hook.installed) return;

		ModLoaderLogger::LogInfo(L"[HttpServer] Removing hook...");
		g_hook.Remove();
		g_original = nullptr; g_errorFuncAddr = 0; g_createFuncAddr = 0;
		{ std::lock_guard lock(g_routesMutex);    g_routes.clear(); }
		{ std::lock_guard lock(g_rawRoutesMutex); g_rawRoutes.clear(); }
		{ std::lock_guard lock(g_filtersMutex);   g_filters.clear(); }
		ModLoaderLogger::LogInfo(L"[HttpServer] Hook removed");
	}

	bool IsInstalled() { return g_hook.installed; }

	bool AddRoute(const char* pluginName, const char* folderName)
	{
		if (!pluginName || !folderName || folderName[0] == '\0')
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] AddRoute: invalid arguments");
			return false;
		}

		// Resolve the plugin's directory from the modloader DLL path, not the exe path.
		// The modloader DLL lives at:  <game>\Binaries\Win64\version.dll  (or similar)
		// Plugin DLLs live at:         <game>\Binaries\Win64\Plugins\<pluginName>\<pluginName>.dll
		// Static files live at:    <game>\Binaries\Win64\Plugins\<pluginName>\<folderName>\
		// We find the modloader DLL path and derive the Plugins directory from its parent.
		wchar_t dllPath[MAX_PATH]{};
		HMODULE hSelf = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&AddRoute),
			&hSelf);
		GetModuleFileNameW(hSelf, dllPath, MAX_PATH);
		// Strip filename to get directory containing the modloader DLL
		if (wchar_t* s = wcsrchr(dllPath, L'\\')) *s = L'\0';

		wchar_t pluginNameW[256]{}, folderNameW[256]{};
		MultiByteToWideChar(CP_UTF8, 0, pluginName, -1, pluginNameW, 256);
		MultiByteToWideChar(CP_UTF8, 0, folderName, -1, folderNameW, 256);

		wchar_t fsRoot[MAX_PATH]{};
		swprintf_s(fsRoot, L"%s\\Plugins\\%s\\%s", dllPath, pluginNameW, folderNameW);

		auto toLower = [](std::string s) {
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
			};
		std::string prefix = "/" + toLower(pluginName) + "/" + toLower(folderName) + "/";

		std::lock_guard lock(g_routesMutex);
		for (const auto& r : g_routes)
			if (r.urlPrefix == prefix)
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] AddRoute: already registered: %S", prefix.c_str());
				return false;
			}

		RouteEntry entry;
		entry.pluginName = pluginName;
		entry.folderName = folderName;
		entry.fsRoot = fsRoot;
		entry.urlPrefix = prefix;
		g_routes.push_back(std::move(entry));

		ModLoaderLogger::LogInfo(L"[HttpServer] Route registered: %S  ->  %s", prefix.c_str(), fsRoot);
		return true;
	}

	void RemoveRoute(const char* pluginName, const char* folderName)
	{
		if (!pluginName || !folderName) return;
		auto toLower = [](std::string s) {
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
			};
		std::string prefix = "/" + toLower(pluginName) + "/" + toLower(folderName) + "/";
		std::lock_guard lock(g_routesMutex);
		auto it = std::remove_if(g_routes.begin(), g_routes.end(),
			[&](const RouteEntry& r) { return r.urlPrefix == prefix; });
		if (it != g_routes.end())
		{
			g_routes.erase(it, g_routes.end());
			ModLoaderLogger::LogInfo(L"[HttpServer] Route unregistered: %S", prefix.c_str());
		}
	}

	void RegisterRawRequestFilter(PluginHttpRequestFilterCallback callback)
	{
		if (!callback) { ModLoaderLogger::LogWarn(L"[HttpServer] RegisterRawRequestFilter: null callback"); return; }
		std::lock_guard lock(g_filtersMutex);
		g_filters.push_back(callback);
		ModLoaderLogger::LogDebug(L"[HttpServer] Raw-request filter registered (%zu total)", g_filters.size());
	}

	void UnregisterRawRequestFilter(PluginHttpRequestFilterCallback callback)
	{
		if (!callback) return;
		std::lock_guard lock(g_filtersMutex);
		auto it = std::find(g_filters.begin(), g_filters.end(), callback);
		if (it != g_filters.end())
		{
			g_filters.erase(it);
			ModLoaderLogger::LogDebug(L"[HttpServer] Raw-request filter unregistered (%zu remaining)", g_filters.size());
		}
	}

	// ---------------------------------------------------------------------------
	// Raw-response route registration
	// ---------------------------------------------------------------------------
	bool AddRawRoute(const char* pluginName, const char* urlPrefix, PluginHttpRouteCallback callback)
	{
		if (!pluginName || !urlPrefix || urlPrefix[0] == '\0' || !callback)
		{
			ModLoaderLogger::LogWarn(L"[HttpServer] AddRawRoute: invalid arguments");
			return false;
		}

		auto toLower = [](std::string s) {
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
			};
		// URL prefix: /<pluginName>/<urlPrefix>/  (no /plugins/ prefix)
		std::string prefix = "/" + toLower(pluginName) + "/" + toLower(urlPrefix) + "/";

		std::lock_guard lock(g_rawRoutesMutex);
		for (const auto& r : g_rawRoutes)
			if (r.urlPrefix == prefix)
			{
				ModLoaderLogger::LogWarn(L"[HttpServer] AddRawRoute: already registered: %S", prefix.c_str());
				return false;
			}

		RawRouteEntry entry;
		entry.pluginName = pluginName;
		entry.urlPrefix = prefix;
		entry.callback = callback;
		g_rawRoutes.push_back(std::move(entry));

		ModLoaderLogger::LogInfo(L"[HttpServer] Raw route registered: %S", prefix.c_str());
		return true;
	}

	void RemoveRawRoute(const char* pluginName, const char* urlPrefix)
	{
		if (!pluginName || !urlPrefix) return;

		auto toLower = [](std::string s) {
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
			};
		std::string prefix = "/" + toLower(pluginName) + "/" + toLower(urlPrefix) + "/";

		std::lock_guard lock(g_rawRoutesMutex);
		auto it = std::remove_if(g_rawRoutes.begin(), g_rawRoutes.end(),
			[&](const RawRouteEntry& r) { return r.urlPrefix == prefix; });
		if (it != g_rawRoutes.end())
		{
			g_rawRoutes.erase(it, g_rawRoutes.end());
			ModLoaderLogger::LogInfo(L"[HttpServer] Raw route unregistered: %S", prefix.c_str());
		}
	}

} // namespace Hooks::HttpServer
