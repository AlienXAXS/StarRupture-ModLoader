#include "pch.h"
#include "plugin_guard.h"
#include "logging/logger.h"

#include <atomic>
#include <cstring>
#include <malloc.h>
#include <mutex>

namespace PluginGuard
{
	// One per plugin DLL. `base` is written last on Track and first on
	// Untrack, so a reader that sees a non-zero base sees the rest of it; the
	// filter reads these on a faulting thread without taking s_mutex.
	struct Slot
	{
		std::atomic<uintptr_t>   base{ 0 };
		std::atomic<uintptr_t>   end{ 0 };
		std::atomic<bool>        off{ false };
		std::atomic<uint32_t>    code{ 0 };
		std::atomic<const char*> callback{ nullptr };
		char                     name[64] = {};
	};

	// More plugins than this are loaded without the guard (and say so).
	static constexpr size_t kMaxModules = 128;

	static Slot                  s_slots[kMaxModules];
	static std::atomic<int>      s_offCount{ 0 };   // slots with off set: IsOff's fast path
	static std::atomic<Reporter> s_reporter{ nullptr };
	static std::mutex            s_mutex;           // Track, Untrack and switching a plugin off

	// Callbacks that faulted at an address no module holds any more, each logged once.
	static constexpr size_t      kMaxStale = 32;
	static uintptr_t             s_staleLogged[kMaxStale];
	static size_t                s_staleCount = 0;

	static uintptr_t ImageEnd(HMODULE module)
	{
		const auto base = reinterpret_cast<uintptr_t>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		return base + nt->OptionalHeader.SizeOfImage;
	}

	// Lock-free: the filter runs it on the faulting thread.
	static Slot* SlotFor(uintptr_t address)
	{
		for (Slot& s : s_slots)
		{
			const uintptr_t base = s.base.load(std::memory_order_acquire);
			if (base && address >= base && address < s.end.load(std::memory_order_relaxed))
				return &s;
		}
		return nullptr;
	}

	// The call itself faulted, on an address that is no longer code: the
	// callback of a DLL that is gone. Memory that is still executable (a
	// thunk a plugin passed as its callback, or another DLL mapped there since)
	// does not count.
	static bool StaleCall(const void* callback, uintptr_t faultAddress)
	{
		if (faultAddress != reinterpret_cast<uintptr_t>(callback))
			return false;
		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(callback, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
			return true;
		constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		return (mbi.Protect & kExecute) == 0;
	}

	void SetReporter(Reporter reporter)
	{
		s_reporter.store(reporter);
	}

	void Track(HMODULE module, const char* pluginName)
	{
		if (!module) return;
		const auto base = reinterpret_cast<uintptr_t>(module);

		std::lock_guard<std::mutex> lock(s_mutex);
		Slot* free = nullptr;
		for (Slot& s : s_slots)
		{
			const uintptr_t b = s.base.load(std::memory_order_relaxed);
			if (b == base) return;                  // already tracked
			if (!b && !free) free = &s;
		}
		if (!free)
		{
			ModLoaderLogger::LogWarn(L"[PluginGuard] More than %zu plugins: %S runs without the crash guard",
			                         kMaxModules, pluginName ? pluginName : "?");
			return;
		}
		strncpy_s(free->name, pluginName ? pluginName : "?", _TRUNCATE);
		free->off.store(false, std::memory_order_relaxed);
		free->code.store(0, std::memory_order_relaxed);
		free->callback.store(nullptr, std::memory_order_relaxed);
		free->end.store(ImageEnd(module), std::memory_order_relaxed);
		free->base.store(base, std::memory_order_release);
	}

	void Untrack(HMODULE module)
	{
		if (!module) return;
		const auto base = reinterpret_cast<uintptr_t>(module);

		std::lock_guard<std::mutex> lock(s_mutex);
		for (Slot& s : s_slots)
		{
			if (s.base.load(std::memory_order_relaxed) != base) continue;
			s.base.store(0, std::memory_order_release);
			if (s.off.exchange(false))
				s_offCount.fetch_sub(1);
			return;
		}
	}

	bool IsOff(const void* address)
	{
		if (s_offCount.load(std::memory_order_relaxed) == 0)
			return false;
		const Slot* s = SlotFor(reinterpret_cast<uintptr_t>(address));
		return s && s->off.load(std::memory_order_acquire);
	}

	bool GetFault(HMODULE module, Fault* out)
	{
		if (!module || s_offCount.load(std::memory_order_relaxed) == 0)
			return false;
		std::lock_guard<std::mutex> lock(s_mutex);
		const Slot* s = SlotFor(reinterpret_cast<uintptr_t>(module));
		if (!s || !s->off.load()) return false;
		if (out)
		{
			out->code     = s->code.load();
			out->callback = s->callback.load();
		}
		return true;
	}

	HMODULE ModuleOf(const void* address)
	{
		HMODULE owner = nullptr;
		if (!address ||
		    !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        static_cast<LPCSTR>(address), &owner))
			return nullptr;
		return owner;
	}

	namespace Detail
	{
		static constexpr size_t kMaxFrames = 24;

		// Filled by the filter, read once the handler has run.
		struct Capture
		{
			uint32_t  code;
			uintptr_t address;
			uintptr_t frames[kMaxFrames];
			size_t    frameCount;
		};

		static constexpr DWORD kCppException = 0xE06D7363;   // MSVC's throw

		// The faulting thread's return addresses, walked from the fault's
		// CONTEXT. Done in the filter: once the handler runs, the stack below it
		// has been unwound and is reused. Holds no C++ objects (C2712).
		static size_t WalkFrames(const CONTEXT& fault, uintptr_t* frames, size_t max)
		{
			CONTEXT ctx = fault;
			size_t  n   = 0;
			__try
			{
				while (n < max && ctx.Rip)
				{
					frames[n++] = static_cast<uintptr_t>(ctx.Rip);
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
					if (fn)
					{
						void*   handlerData = nullptr;
						DWORD64 establisher = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
					}
					else
					{
						ctx.Rip  = *reinterpret_cast<const DWORD64*>(ctx.Rsp);   // a leaf function
						ctx.Rsp += sizeof(DWORD64);
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}   // a stack too broken to walk further
			return n;
		}

		// A fault under a plugin's callback but outside its code: logged once
		// per exception (nested guarded calls each see it), then passed on.
		static void NotePassedOn(const EXCEPTION_RECORD* record, const Slot* owner, const char* what, uintptr_t fault)
		{
			static thread_local const EXCEPTION_RECORD* t_noted = nullptr;
			if (t_noted == record) return;
			t_noted = record;

			const Slot* where = SlotFor(fault);
			ModLoaderLogger::LogError(L"[PluginGuard] Fault (code=0x%08X at %p) under plugin '%S', in its %S, "
			                          L"raised in %S: not contained, the crash goes on as it would without the guard",
			                          record->ExceptionCode, reinterpret_cast<void*>(fault), owner->name, what,
			                          where ? where->name : "engine or loader code");
		}

		// Plain function, no C++ objects: it runs on the faulting thread,
		// before anything has been unwound.
		static LONG Filter(EXCEPTION_POINTERS* ep, const void* callback, const char* what, Capture* out)
		{
			const DWORD code = ep->ExceptionRecord->ExceptionCode;
			if (code == kCppException)
				return EXCEPTION_CONTINUE_SEARCH;            // the dispatcher's catch handles it

			// Contained only when the faulting instruction is the plugin's own:
			// engine or loader frames below the callback may hold locks that an
			// unwind past them would never release.
			const uintptr_t fault = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
			const Slot* owner = SlotFor(reinterpret_cast<uintptr_t>(callback));
			if (owner ? SlotFor(fault) != owner : !StaleCall(callback, fault))
			{
				if (owner) NotePassedOn(ep->ExceptionRecord, owner, what, fault);
				return EXCEPTION_CONTINUE_SEARCH;            // not the plugin's own code: as before
			}

			out->code       = code;
			out->address    = fault;
			out->frameCount = 0;
			if (code == EXCEPTION_STACK_OVERFLOW)
				out->frames[out->frameCount++] = fault;      // no stack to spare for a walk
			else
				out->frameCount = WalkFrames(*ep->ContextRecord, out->frames, kMaxFrames);
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// Must hold no C++ objects (C2712).
		static bool RunSEH(const void* callback, const char* what, Thunk thunk, void* body, Capture* out)
		{
			__try { thunk(body); return true; }
			__except (Filter(GetExceptionInformation(), callback, what, out)) { return false; }
		}

		static void SwitchOff(const void* callback, const char* what, const Capture& c)
		{
			if (c.code == EXCEPTION_STACK_OVERFLOW)
				_resetstkoflw();

			Report report = {};
			char   name[sizeof(Slot::name)] = {};   // the slot may be re-used once s_mutex is released
			{
				std::lock_guard<std::mutex> lock(s_mutex);
				Slot* s = SlotFor(reinterpret_cast<uintptr_t>(callback));
				if (!s)
				{
					// Nothing to switch off: the call landed where the callback's DLL used to be.
					const auto address = reinterpret_cast<uintptr_t>(callback);
					for (size_t i = 0; i < s_staleCount; ++i)
						if (s_staleLogged[i] == address) return;
					if (s_staleCount < kMaxStale) s_staleLogged[s_staleCount++] = address;
					ModLoaderLogger::LogError(L"[PluginGuard] %S callback at %p crashed (code=0x%08X): "
					                          L"it is in no loaded module, left behind by a plugin that was unloaded",
					                          what, callback, c.code);
					return;
				}
				if (s->off.load())
					return;                                   // switched off already, by another thread
				s->code.store(c.code);
				s->callback.store(what);
				s->off.store(true, std::memory_order_release);
				s_offCount.fetch_add(1);
				strncpy_s(name, s->name, _TRUNCATE);
				report = { reinterpret_cast<HMODULE>(s->base.load()), name, what, c.code, c.address, c.frames, c.frameCount };
			}
			if (Reporter r = s_reporter.load())
				r(report);
			else
				ModLoaderLogger::LogError(L"[PluginGuard] Plugin '%S' CRASHED in %S (code=0x%08X) and is switched off",
				                          report.pluginName, what, c.code);
		}

		bool Run(const void* callback, const char* what, Thunk thunk, void* body)
		{
			Capture c;
			if (RunSEH(callback, what, thunk, body, &c))
				return true;
			SwitchOff(callback, what, c);
			return false;
		}
	}
}
