#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD

#include "modloader_hello.h"
#include "network_channel/network_channel.h"
#include "logging/logger.h"
#include "hooks/game/game_instance_init/game_instance_init.h"

#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"

#include <cwchar>

namespace Hooks::ModLoaderHello
{
	// exec signature: (UObject* Context, FFrame& Stack, void* Result)
	using ExecFunc_t = void(__fastcall*)(void* context, void* stack, void* result);

	// FFrame::Locals -- vptr, Node, Object, Code, Locals.
	static constexpr size_t kFFrameLocalsOffset = 0x20;

	// Params::PlayerController_ClientMessage:
	//   +0x00 FString S  { wchar_t* Data, int32 Num, int32 Max }
	//   +0x10 FName   Type
	//   +0x18 float   MsgLifeTime
	struct FStringView { wchar_t* Data; int32_t Num; int32_t Max; };

	// The greeting is short by construction; anything longer is not ours.
	static constexpr size_t kMaxPayloadChars = 128;

	static Hook            g_hook;
	static ExecFunc_t      g_origExec = nullptr;
	static SDK::UFunction* g_func     = nullptr;
	static HelloCallback   g_cb       = nullptr;

	// Match the sentinel and copy out what follows it. POD only -- this runs
	// inside __try, and the reader is a pointer that came off the network.
	static bool ExtractPayloadSEH(const void* params, wchar_t* out, size_t outChars)
	{
		__try
		{
			const FStringView* s = reinterpret_cast<const FStringView*>(params);
			if (!s->Data || s->Num <= 0) return false;

			const wchar_t* sentinel = NetworkChannel::kHelloSentinel;
			const size_t   sentLen  = wcslen(sentinel);

			// Num counts the null terminator, so a bare sentinel is sentLen + 1.
			if (static_cast<size_t>(s->Num) < sentLen + 1) return false;
			if (wcsncmp(s->Data, sentinel, sentLen) != 0) return false;

			wcsncpy_s(out, outChars, s->Data + sentLen, _TRUNCATE);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Returns true if this was our greeting (and it has been dispatched).
	static bool HandleIfHello(const void* params)
	{
		if (!params) return false;

		wchar_t payload[kMaxPayloadChars] = {};
		if (!ExtractPayloadSEH(params, payload, kMaxPayloadChars)) return false;

		ModLoaderLogger::LogDebug(L"[ModLoaderHello] Greeting received from the authority: '%s'", payload);

		if (g_cb)
		{
			try { g_cb(payload); }
			catch (...) { ModLoaderLogger::LogError(L"[ModLoaderHello] Exception in hello callback"); }
		}
		return true;
	}

	static void ProcessEventObserver(void* /*obj*/, void* fn, void* params)
	{
		if (fn != g_func) return;
		HandleIfHello(params);
		// Nothing to suppress here: the observer runs alongside the original call
		// rather than in place of it. The exec hook below is what suppresses.
	}

	static void __fastcall ExecHook(void* context, void* stack, void* result)
	{
		void* locals = *reinterpret_cast<void**>(static_cast<uint8_t*>(stack) + kFFrameLocalsOffset);

		if (HandleIfHello(locals))
			return; // consumed -- do not hand our sentinel to the game

		if (g_origExec) g_origExec(context, stack, result);
	}

	void SetCallback(HelloCallback cb) { g_cb = cb; }

	bool Install()
	{
		if (g_hook.installed) return true;

		SDK::UClass* pcClass = SDK::APlayerController::StaticClass();
		if (!pcClass)
		{
			ModLoaderLogger::LogError(L"[ModLoaderHello] APlayerController::StaticClass() returned null");
			return false;
		}

		// Resolved by class + name rather than by a global name search: several
		// classes can own a function called ClientMessage, and hooking the wrong
		// exec thunk would be a silent no-op.
		g_func = pcClass->GetFunction("PlayerController", "ClientMessage");
		if (!g_func)
		{
			ModLoaderLogger::LogError(
				L"[ModLoaderHello] PlayerController.ClientMessage UFunction not found -- this client "
				L"cannot be greeted, so plugin networking will stay disabled");
			return false;
		}

		auto execAddr = reinterpret_cast<uintptr_t>(g_func->ExecFunction);
		if (!execAddr)
		{
			ModLoaderLogger::LogError(L"[ModLoaderHello] ClientMessage ExecFunction pointer is null");
			return false;
		}

		const bool ok = g_hook.Install(
			execAddr, reinterpret_cast<void*>(&ExecHook), reinterpret_cast<void**>(&g_origExec));

		if (!ok)
		{
			ModLoaderLogger::LogError(L"[ModLoaderHello] Failed to hook ClientMessage exec thunk");
			return false;
		}

		Hooks::GameInstanceInit::RegisterProcessEventCallback(&ProcessEventObserver);

		// One line, once, at startup. Says what it is watching for in terms of
		// what it means rather than where it is; the address is a Debug detail.
		ModLoaderLogger::LogInfo(
			L"[ModLoaderHello] Ready -- will listen for a server to identify itself as running the "
			L"mod loader when joining a session");
		ModLoaderLogger::LogDebug(
			L"[ModLoaderHello] ClientMessage exec thunk hooked at 0x%llX",
			static_cast<unsigned long long>(execAddr));
		return true;
	}

	void Remove()
	{
		Hooks::GameInstanceInit::UnregisterProcessEventCallback(&ProcessEventObserver);
		g_hook.Remove();
		g_origExec = nullptr;
		g_func     = nullptr;
		g_cb       = nullptr;
	}

	bool IsInstalled() { return g_hook.installed; }

} // namespace Hooks::ModLoaderHello

#endif // MODLOADER_CLIENT_BUILD
