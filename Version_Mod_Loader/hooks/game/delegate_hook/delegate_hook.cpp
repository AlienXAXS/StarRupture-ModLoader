#include "pch.h"
#include "delegate_hook.h"
#include "logging/logger.h"
#include "hooks/hooks_common.h"
#include "hooks/memory/engine_allocator.h"
#include "hooks/game/gobject_walk/gobject_walk.h"
#include "hooks/game/scan_patterns.h"
#include "memory_scanner/scanner.h"
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>

// ---------------------------------------------------------------------------
// Phase 3 implementation notes (see delegate_hook.h for the overall design).
//
// PHASE 3 REDESIGN (2026-06-21) -- synthetic UFunction + UClass::
// FindFunctionByName hook, replacing Phase 2's FuncMap/AllFunctionsCache slot
// override.
//
// Phase 2 overwrote a REAL UFunction's slot in its owning UClass's FuncMap/
// AllFunctionsCache so that UClass::FindFunctionByName would resolve to our
// clone instead. That's shared engine state: anything else resolving the
// same function name via FindFunctionByName (another delegate, a Blueprint
// binding) also got redirected, with no way to tell apart from a real call.
// That's a correctness bug, not just a scaling limitation -- see git history
// for the case that exposed it (CloseSessionConfirmationWidget firing our
// callback when a player legitimately closed that dialog).
//
// Phase 3 never touches any real UClass's FuncMap/AllFunctionsCache at all.
// Instead:
//   - The clone gets a brand-new synthetic FName (ComparisonIndex=0 i.e.
//     "None"'s index, Number from a private 0x70000000+ range) that nothing
//     real in the game will ever produce. FName equality is just
//     (ComparisonIndex, Number) -- no string-table lookup needed to mint one.
//   - UClass::FindFunctionByName itself is hooked (AOB pattern supplied
//     directly, see scan_patterns.h::UClass_FindFunctionByName). The detour
//     checks the incoming FName against our synthetic range FIRST (two
//     integer comparisons, no lock) before falling through to the real
//     implementation untouched. Only an exact match against one of our own
//     registered synthetic Numbers returns our clone; every other name --
//     which is everything else in the entire game -- takes the unmodified
//     original path.
//   - UClass::AddFunctionToFunctionMap (which would have let the engine's
//     own code perform a correct TMap insert for us) does not appear in IDA
//     at all (likely inlined) -- this is why we intercept resolution instead
//     of trying to insert a real TMap entry. Reimplementing TMap's hash-
//     bucket insert by hand was explicitly avoided in Phase 2 even for
//     overwriting an existing slot's VALUE; minting a new KEY would be
//     riskier still.
//
// Multiple concurrent hooks: g_hooks is keyed by delegatePtr (not a single
// global slot like Phase 1/2), so different delegates/plugins can each hold
// an independent hook at the same time. The FindFunctionByName detour itself
// is installed once, lazily, on first use, and stays installed even if the
// registry later becomes empty (cheap fallthrough for the non-matching case).
//
// PRIOR (2026-06-20/21) findings, still load-bearing and UNCHANGED by this
// redesign -- the delegate-splice mechanism (growing InvocationList,
// building a FWeakObjectPtr with a lazily-allocated serial number) was never
// the problem, only the name-resolution side was:
//   - FUObjectItem::SerialNumber is at offset 0x10 (kSerialNumberOffset).
//   - TMulticastScriptDelegate::InvocationList is at offset 0x8 within the
//     delegate struct (kInvocationListOffset).
//   - FWeakObjectPtr field order is {ObjectIndex, ObjectSerialNumber}, matching
//     this SDK's Basic.hpp.
//   - UFunction (real layout, confirmed via Local Types):
//       0xB0 FunctionFlags, 0xB4 NumParms, 0xB6 ParmsSize, 0xB8 ReturnValueOffset,
//       0xBA RPCId, 0xBC RPCResponseId, 0xC0 FirstPropertyToInit,
//       0xC8 EventGraphFunction, 0xD0 EventGraphCallOffset, 0xD8 Func (ExecFunction).
//     This SDK's Dumper-7 dump only names FunctionFlags (0xB0) and ExecFunction
//     (0xD8), swallowing the rest into Pad_B4[0x24] -- a flat memcpy of the
//     whole struct (sizeof(UFunction)) carries all of this over to the clone
//     without needing to name every field individually.
//   - Real UE FWeakObjectPtr::Get() treats ObjectSerialNumber == 0 as "never
//     assigned" and returns null WITHOUT consulting the array at all. A
//     freshly-seen UObject's FUObjectItem legitimately has SerialNumber == 0
//     until something lazily allocates one (the engine does this the first
//     time anything takes a weak pointer to that object, via
//     FUObjectArray::AllocateSerialNumber). We replicate that lazy-allocate-
//     and-write-back step in AllocateOrReadSerialNumber() below.
//
// Risk notes:
//   - UClass::FindFunctionByName is hot (called for most delegate broadcasts
//     and by-name ProcessEvent dispatch in the game generally), so the
//     non-matching fallthrough must stay cheap -- the synthetic-range check
//     is two integer comparisons before any lock or map lookup.
//   - The ComparisonIndex=0 ("None"), Number>=0x70000000 range is an
//     assumption that the engine will never legitimately produce, not a
//     guaranteed-impossible collision -- same caveat as the other
//     IDA-verified-offset assumptions in this file.
// ---------------------------------------------------------------------------

namespace Hooks::DelegateHook
{
	namespace
	{
		// Borrowed FName index ("None") + private Number range -- see file
		// header "Synthetic FName, without needing an FName-from-string
		// constructor".
		constexpr int32_t kSyntheticComparisonIndex = 0;
		constexpr uint32_t kSyntheticNumberBase = 0x70000000u;

		std::atomic<uint32_t> g_nextSyntheticNumber{ kSyntheticNumberBase };

		struct ActiveHook
		{
			void* delegatePtr = nullptr;
			SDK::UObject* hostObject = nullptr;
			SDK::UFunction* cloneFunction = nullptr;
			uint32_t syntheticNumber = 0;
			DelegateCallback callback = nullptr;
			void* userContext = nullptr; // opaque, handed back to callback unchanged on every fire

			// Identifies the exact FScriptDelegate we spliced in, so Unhook can
			// remove just that one entry by matching instead of reverting the
			// whole array to a snapshot. A blind snapshot-restore would silently
			// delete anything else (a Blueprint binding, another mod, etc.) that
			// got added to InvocationList while our hook was active -- this list
			// is shared engine state, not ours to overwrite wholesale.
			SDK::FScriptDelegate splicedEntry{};
		};

		std::mutex g_hooksMutex;
		std::unordered_map<HookHandle, ActiveHook> g_hooks;         // keyed by handle -- a delegatePtr may have several
		struct SyntheticEntry
		{
			SDK::UFunction* clone = nullptr;
			HookHandle handle = 0;
		};
		std::unordered_map<uint32_t, SyntheticEntry> g_byNumber;   // keyed by syntheticNumber
		std::atomic<HookHandle> g_nextHandle{ 1 }; // 0 is never valid

		// Sequencing assumption: TMulticastScriptDelegate::ProcessMulticastDelegate
		// resolves and calls each InvocationList entry one at a time, in order,
		// synchronously, on the same thread -- so the handle we stash here in
		// the FindFunctionByName detour is still the right one by the time the
		// resolved UFunction's ExecFunction (DispatchClonedExecFunction) runs
		// immediately afterward. This is what lets the dispatcher identify
		// exactly which hook fired instead of guessing from `context`, which
		// only ever carries the host UObject* (shared across every hook on
		// that object), not the UFunction* that was actually resolved.
		thread_local HookHandle tls_pendingHandle = 0;

		// UClass::FindFunctionByName detour -- installed lazily, once, on
		// first HookViaExistingUFunction call. EIncludeSuperFlag isn't in
		// this SDK dump, so the third parameter is a plain int; we never
		// inspect it, only forward it to the original.
		using FindFunctionByName_t = SDK::UFunction*(__fastcall*)(SDK::UClass*, SDK::FName, int);

		Hooks::Hook g_findFunctionByNameHook;
		FindFunctionByName_t g_originalFindFunctionByName = nullptr;

		SDK::UFunction* __fastcall Detour_FindFunctionByName(SDK::UClass* thisClass, SDK::FName inName, int includeSuper)
		{
			// Cheap range check first -- this function is hot, called for
			// essentially every delegate broadcast and by-name dispatch in
			// the entire game, not just ours. Only an exact match against a
			// currently-registered synthetic Number takes the lock.
			if (inName.ComparisonIndex == kSyntheticComparisonIndex && inName.Number >= kSyntheticNumberBase)
			{
				std::lock_guard<std::mutex> lock(g_hooksMutex);
				auto it = g_byNumber.find(inName.Number);
				if (it != g_byNumber.end())
				{
					tls_pendingHandle = it->second.handle;
					return it->second.clone;
				}
			}

			return g_originalFindFunctionByName(thisClass, inName, includeSuper);
		}

		bool EnsureFindFunctionByNameHookInstalled()
		{
			if (g_findFunctionByNameHook.installed)
				return true;

			uintptr_t addr = Scanner::FindPatternInMainModule(
				"UClass::FindFunctionByName", ScanPatterns::UClass_FindFunctionByName);
			if (!addr)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] Pattern scan for UClass::FindFunctionByName failed");
				return false;
			}

			bool hookOk = g_findFunctionByNameHook.Install(
				addr,
				reinterpret_cast<void*>(&Detour_FindFunctionByName),
				reinterpret_cast<void**>(&g_originalFindFunctionByName));

			if (hookOk)
				ModLoaderLogger::LogInfo(L"[DelegateHook] UClass::FindFunctionByName hook installed at 0x%llX", static_cast<unsigned long long>(addr));
			else
				ModLoaderLogger::LogError(L"[DelegateHook] UClass::FindFunctionByName hook installation failed");

			return hookOk;
		}

		// Offset of SerialNumber within FUObjectItem; not exposed by this SDK
		// dump (Basic.hpp collapses it into Pad_8). Verified via IDA Local
		// Types against this game's binary -- see file header.
		constexpr size_t kSerialNumberOffset = 0x10;

		// Offset of InvocationList within the delegate struct (Basic.hpp:
		// Pad_0[0x8] then InvocationList). Computed by hand instead of
		// offsetof() because TArray's constructors make the type non-POD,
		// which offsetof() does not reliably support across templated args.
		// Verified via IDA Local Types against this game's binary -- see file header.
		constexpr size_t kInvocationListOffset = 0x8;

		int32_t FindObjectIndex(SDK::UObject* obj)
		{
			if (!obj)
				return -1;

			const int32_t num = SDK::UObject::GObjects->Num();
			for (int32_t i = 0; i < num; ++i)
			{
				if (SDK::UObject::GObjects->GetByIndex(i) == obj)
				{
					ModLoaderLogger::LogTrace(L"[DelegateHook] FindObjectIndex: %p -> GObjects[%d] (scanned %d entries)", obj, i, num);
					return i;
				}
			}

			ModLoaderLogger::LogDebug(L"[DelegateHook] FindObjectIndex: %p not found after scanning %d GObjects entries", obj, num);
			return -1;
		}

		// Mirrors TUObjectArray::GetByIndex's chunk math (Basic.hpp) but
		// returns the FUObjectItem* instead of ->Object, so we can reach the
		// raw SerialNumber bytes that aren't named in this SDK's struct.
		SDK::FUObjectItem* GetUObjectItem(int32_t index)
		{
			auto* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] GetUObjectItem: GObjects.GetTypedPtr() returned null");
				return nullptr;
			}
			if (index < 0 || index >= arr->NumElements)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] GetUObjectItem: index %d out of range (NumElements=%d)", index, arr->NumElements);
				return nullptr;
			}

			const int32_t chunkIndex = index / SDK::TUObjectArray::ElementsPerChunk;
			const int32_t inChunkIdx = index % SDK::TUObjectArray::ElementsPerChunk;
			if (chunkIndex >= arr->NumChunks)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] GetUObjectItem: chunkIndex %d >= NumChunks %d", chunkIndex, arr->NumChunks);
				return nullptr;
			}

			SDK::FUObjectItem* chunkPtr = arr->GetDecrytedObjPtr()[chunkIndex];
			if (!chunkPtr)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] GetUObjectItem: null chunk pointer at chunkIndex %d", chunkIndex);
				return nullptr;
			}

			return &chunkPtr[inChunkIdx];
		}

		// Real UE FWeakObjectPtr::Get() treats ObjectSerialNumber == 0 as "this
		// weak pointer was never assigned" and returns null WITHOUT checking the
		// array -- confirmed live: the engine's own Broadcast() never reached our
		// spliced FScriptDelegate because the host object's FUObjectItem had
		// never had a serial number lazily allocated (nothing had taken a weak
		// ptr to it before). The real engine's FUObjectArray::AllocateSerialNumber
		// allocates a fresh nonzero serial ON FIRST USE and writes it back into
		// the live FUObjectItem -- we replicate that here with our own counter.
		// Uniqueness only needs to hold per-slot (the engine matches by
		// {ObjectIndex, ObjectSerialNumber} together), so a separate counter
		// pool from the engine's own is fine as long as we never hand out the
		// same value twice for the same slot.
		int32_t AllocateOrReadSerialNumber(SDK::FUObjectItem* item)
		{
			static std::atomic<int32_t> s_nextSerial{ 0x10000000 }; // pool well clear of the engine's own small serials

			auto* serialSlot = reinterpret_cast<volatile LONG*>(reinterpret_cast<uint8_t*>(item) + kSerialNumberOffset);

			LONG current = *serialSlot;
			if (current != 0)
				return current;

			LONG candidate = s_nextSerial.fetch_add(1);
			LONG prev = InterlockedCompareExchange(serialSlot, candidate, 0);
			return (prev == 0) ? candidate : prev;
		}

		bool BuildWeakObjectPtr(SDK::UObject* obj, SDK::FWeakObjectPtr& out)
		{
			const int32_t index = FindObjectIndex(obj);
			if (index < 0)
				return false;

			SDK::FUObjectItem* item = GetUObjectItem(index);
			if (!item)
				return false;

			const int32_t serial = AllocateOrReadSerialNumber(item);

			out.ObjectIndex = index;
			out.ObjectSerialNumber = serial;

			ModLoaderLogger::LogDebug(L"[DelegateHook] BuildWeakObjectPtr: %p -> index=%d serial=%d", obj, index, serial);
			return true;
		}

		// Grows InvocationList by one element using the engine's own allocator
		// (so any later engine-side TArray operation on this buffer -- shrink,
		// free, re-grow via a real Add() -- stays consistent with how it was
		// allocated), then immediately frees the buffer it replaced. The old
		// buffer is never kept around for a later "restore" -- see
		// RemoveScriptDelegate, which removes only our own entry by matching
		// instead of reverting the array to a snapshot.
		bool AppendScriptDelegate(SDK::TArray<SDK::FScriptDelegate>* list, const SDK::FScriptDelegate& entry)
		{
			if (!list || !EngineAllocator::IsAvailable())
				return false;

			SDK::FScriptDelegate* oldData = const_cast<SDK::FScriptDelegate*>(list->GetDataPtr());
			const int32_t oldNum = list->Num();

			const int32_t newNum = oldNum + 1;
			const int32_t newMax = newNum; // exact-fit; no slack growth needed for a one-shot append

			auto* newData = static_cast<SDK::FScriptDelegate*>(
				EngineAllocator::Alloc(static_cast<size_t>(newMax) * sizeof(SDK::FScriptDelegate), alignof(SDK::FScriptDelegate)));
			if (!newData)
			{
				ModLoaderLogger::LogError(
					L"[DelegateHook] AppendScriptDelegate: EngineAllocator::Alloc(%zu bytes) failed",
					static_cast<size_t>(newMax) * sizeof(SDK::FScriptDelegate));
				return false;
			}

			if (oldNum > 0 && oldData)
				memcpy(newData, oldData, static_cast<size_t>(oldNum) * sizeof(SDK::FScriptDelegate));
			newData[oldNum] = entry;

			ModLoaderLogger::LogDebug(
				L"[DelegateHook] AppendScriptDelegate: list=%p oldData=%p oldNum=%d -> newData=%p newNum=%d (entry.ObjectIndex=%d entry.ObjectSerial=%d)",
				list, oldData, oldNum, newData, newNum, entry.Object.ObjectIndex, entry.Object.ObjectSerialNumber);

			*list = SDK::TArray<SDK::FScriptDelegate>(newData, newNum, newMax);

			if (oldNum > 0 && oldData)
				EngineAllocator::Free(oldData);

			return true;
		}

		// Removes exactly the one FScriptDelegate matching `target` (by Object
		// index/serial + FunctionName) from the CURRENT InvocationList, leaving
		// every other entry untouched -- including any added by the engine
		// (Blueprint bindings) or anything else after we spliced in. Rebuilds
		// the array via the engine allocator rather than shifting in place,
		// for the same allocator-consistency reason AppendScriptDelegate does.
		bool RemoveScriptDelegate(SDK::TArray<SDK::FScriptDelegate>* list, const SDK::FScriptDelegate& target)
		{
			if (!list || !EngineAllocator::IsAvailable())
				return false;

			const SDK::FScriptDelegate* oldData = list->GetDataPtr();
			const int32_t oldNum = list->Num();

			int32_t foundIdx = -1;
			for (int32_t i = 0; i < oldNum; ++i)
			{
				if (oldData[i].Object.ObjectIndex == target.Object.ObjectIndex &&
					oldData[i].Object.ObjectSerialNumber == target.Object.ObjectSerialNumber &&
					oldData[i].FunctionName == target.FunctionName)
				{
					foundIdx = i;
					break;
				}
			}

			if (foundIdx < 0)
			{
				ModLoaderLogger::LogWarn(
					L"[DelegateHook] RemoveScriptDelegate: spliced entry (index=%d serial=%d) not found in current list (oldNum=%d) -- already removed by something else?",
					target.Object.ObjectIndex, target.Object.ObjectSerialNumber, oldNum);
				return false;
			}

			const int32_t newNum = oldNum - 1;
			SDK::FScriptDelegate* newData = nullptr;
			if (newNum > 0)
			{
				newData = static_cast<SDK::FScriptDelegate*>(
					EngineAllocator::Alloc(static_cast<size_t>(newNum) * sizeof(SDK::FScriptDelegate), alignof(SDK::FScriptDelegate)));
				if (!newData)
				{
					ModLoaderLogger::LogError(L"[DelegateHook] RemoveScriptDelegate: EngineAllocator::Alloc failed");
					return false;
				}

				int32_t w = 0;
				for (int32_t i = 0; i < oldNum; ++i)
				{
					if (i != foundIdx)
						newData[w++] = oldData[i];
				}
			}

			auto* oldDataMutable = const_cast<SDK::FScriptDelegate*>(oldData);
			*list = SDK::TArray<SDK::FScriptDelegate>(newData, newNum, newNum);

			ModLoaderLogger::LogDebug(
				L"[DelegateHook] RemoveScriptDelegate: removed index %d of %d, list=%p -> newData=%p newNum=%d (freeing oldData=%p)",
				foundIdx, oldNum, list, newData, newNum, oldDataMutable);

			if (oldDataMutable)
				EngineAllocator::Free(oldDataMutable);

			return true;
		}

		// ExecFunction for our synthetic clone. Only reachable through our own
		// FindFunctionByName detour matching one of our private synthetic
		// names -- nothing else in the game will ever resolve to this name,
		// so unlike Phase 1/2 there is no "real" ExecFunction to forward to
		// afterward.
		//
		// All clones share this single static dispatcher (cheap, no per-hook
		// JIT trampoline). `context` only ever carries the host UObject*
		// (ProcessEvent's `this`), never the UFunction* that was actually
		// resolved, so identifying which hook fired can't come from context
		// alone when several hooks share a host object -- confirmed live:
		// with 5 hooks on one delegate, every one of the 5 ExecFunction calls
		// reported callbacks=5 instead of 1 each. Fixed via tls_pendingHandle
		// (see Detour_FindFunctionByName) instead of matching by hostObject.
		void __fastcall DispatchClonedExecFunction(void* context, void* /*theStack*/, void* /*result*/)
		{
			const HookHandle handle = tls_pendingHandle;
			tls_pendingHandle = 0;

			DelegateCallback cb = nullptr;
			void* userContext = nullptr;
			{
				std::lock_guard<std::mutex> lock(g_hooksMutex);
				auto it = g_hooks.find(handle);
				if (it != g_hooks.end())
				{
					cb = it->second.callback;
					userContext = it->second.userContext;
				}
			}

			if (!cb)
			{
				ModLoaderLogger::LogWarn(
					L"[DelegateHook] DispatchClonedExecFunction: context=%p handle=%llu matched no live hook (stale tls_pendingHandle or already unhooked?)",
					context, static_cast<unsigned long long>(handle));
				return;
			}

			ModLoaderLogger::LogTrace(
				L"[DelegateHook] DispatchClonedExecFunction firing: context=%p handle=%llu userContext=%p caller=%S",
				context, static_cast<unsigned long long>(handle), userContext, Hooks::GetCallerModuleName(_ReturnAddress()).c_str());

			try
			{
				cb(userContext);
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[DelegateHook] Unknown exception in callback");
			}
		}
	}

	HookHandle HookViaExistingUFunction(
		void* delegatePtr,
		SDK::UObject* hostObject,
		const char* hostClassName,
		const char* hostFuncName,
		DelegateCallback callback,
		void* userContext)
	{
		if (!delegatePtr || !hostObject || !hostClassName || !hostFuncName || !callback)
		{
			ModLoaderLogger::LogError(L"[DelegateHook] HookViaExistingUFunction: invalid arguments");
			return 0;
		}

		ModLoaderLogger::LogDebug(
			L"[DelegateHook] HookViaExistingUFunction: delegatePtr=%p hostObject=%p template=%S::%S",
			delegatePtr, hostObject, hostClassName, hostFuncName);

		SDK::UFunction* templateFn = Hooks::GObjectWalk::ResolveUFunction(hostClassName, hostFuncName);
		if (!templateFn)
		{
			ModLoaderLogger::LogError(L"[DelegateHook] Could not resolve template %S::%S", hostClassName, hostFuncName);
			return 0;
		}

		if (!EnsureFindFunctionByNameHookInstalled())
		{
			ModLoaderLogger::LogError(L"[DelegateHook] FindFunctionByName hook unavailable -- aborting");
			return 0;
		}

		// Clone the template UFunction (flat memcpy, NOT a GC-tracked UObject --
		// see file header). Only Name and ExecFunction are overwritten;
		// NumParms/ParmsSize/ReturnValueOffset/FunctionFlags are copied as-is
		// so ProcessEvent's marshaling matches the template's signature. The
		// template itself is never modified.
		auto* clone = static_cast<SDK::UFunction*>(EngineAllocator::Alloc(sizeof(SDK::UFunction), alignof(SDK::UFunction)));
		if (!clone)
		{
			ModLoaderLogger::LogError(L"[DelegateHook] EngineAllocator::Alloc(%zu bytes) failed for UFunction clone", sizeof(SDK::UFunction));
			return 0;
		}
		memcpy(clone, templateFn, sizeof(SDK::UFunction));

		const uint32_t syntheticNumber = g_nextSyntheticNumber.fetch_add(1);
		clone->Name = SDK::FName(kSyntheticComparisonIndex, syntheticNumber);
		clone->ExecFunction = reinterpret_cast<SDK::UFunction::FNativeFuncPtr>(&DispatchClonedExecFunction);

		ModLoaderLogger::LogDebug(
			L"[DelegateHook] Cloned %S::%S: template=%p clone=%p (sizeof(UFunction)=%zu), synthetic Number=0x%08X",
			hostClassName, hostFuncName, templateFn, clone, sizeof(SDK::UFunction), syntheticNumber);

		SDK::FWeakObjectPtr weakPtr{};
		if (!BuildWeakObjectPtr(hostObject, weakPtr))
		{
			ModLoaderLogger::LogError(L"[DelegateHook] hostObject not found in GObjects");
			EngineAllocator::Free(clone);
			return 0;
		}

		auto* list = reinterpret_cast<SDK::TArray<SDK::FScriptDelegate>*>(
			reinterpret_cast<uint8_t*>(delegatePtr) + kInvocationListOffset);

		SDK::FScriptDelegate entry{};
		entry.Object = weakPtr;
		entry.FunctionName = clone->Name;

		if (!AppendScriptDelegate(list, entry))
		{
			ModLoaderLogger::LogError(L"[DelegateHook] Failed to grow InvocationList (engine allocator unavailable?)");
			EngineAllocator::Free(clone);
			return 0;
		}

		const HookHandle handle = g_nextHandle.fetch_add(1);
		{
			std::lock_guard<std::mutex> lock(g_hooksMutex);

			ActiveHook hook;
			hook.delegatePtr = delegatePtr;
			hook.hostObject = hostObject;
			hook.cloneFunction = clone;
			hook.syntheticNumber = syntheticNumber;
			hook.callback = callback;
			hook.userContext = userContext;
			hook.splicedEntry = entry;

			g_hooks.emplace(handle, std::move(hook));
			g_byNumber.emplace(syntheticNumber, SyntheticEntry{ clone, handle });
		}

		ModLoaderLogger::LogInfo(
			L"[DelegateHook] Hooked delegate at %p via synthetic clone of %S::%S on object %p (handle=%llu, Phase 3 synthetic-name path)",
			delegatePtr, hostClassName, hostFuncName, hostObject, static_cast<unsigned long long>(handle));
		return handle;
	}

	bool Unhook(HookHandle handle)
	{
		ActiveHook removed;
		{
			std::lock_guard<std::mutex> lock(g_hooksMutex);
			auto it = g_hooks.find(handle);
			if (it == g_hooks.end())
			{
				ModLoaderLogger::LogDebug(L"[DelegateHook] Unhook(%llu): no-op (not hooked)", static_cast<unsigned long long>(handle));
				return false;
			}

			removed = std::move(it->second);
			g_hooks.erase(it);
			g_byNumber.erase(removed.syntheticNumber);
		}

		auto* list = reinterpret_cast<SDK::TArray<SDK::FScriptDelegate>*>(
			reinterpret_cast<uint8_t*>(removed.delegatePtr) + kInvocationListOffset);

		RemoveScriptDelegate(list, removed.splicedEntry);

		if (removed.cloneFunction)
		{
			EngineAllocator::Free(removed.cloneFunction);
			ModLoaderLogger::LogTrace(L"[DelegateHook] Unhook: freed synthetic clone UFunction %p", removed.cloneFunction);
		}

		ModLoaderLogger::LogInfo(L"[DelegateHook] Unhooked delegate at %p (handle=%llu)", removed.delegatePtr, static_cast<unsigned long long>(handle));
		return true;
	}

	bool IsHooked(HookHandle handle)
	{
		std::lock_guard<std::mutex> lock(g_hooksMutex);
		return g_hooks.find(handle) != g_hooks.end();
	}
}
