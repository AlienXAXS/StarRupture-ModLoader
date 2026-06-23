#pragma once

#include "CoreUObject_classes.hpp"
#include "Basic.hpp"
#include <cstdint>

// ---------------------------------------------------------------------------
// EXPERIMENTAL -- generic multicast-delegate hooking, Phase 3.
//
// Goal: let plugins react to UE5 dynamic multicast delegates (e.g.
// SDK::UCrSaveSubsystem::OnAfterSave, a TMulticastInlineDelegate<void()>)
// without the maintainer hand-writing an AOB-scanned hooks/game/<event>/
// module for every delegate someone might want.
//
// Why this isn't a simple "register a function pointer":
//   A TMulticastInlineDelegate's InvocationList is TArray<FScriptDelegate>,
//   and FScriptDelegate is just {FWeakObjectPtr Object; FName FunctionName}.
//   There is no native function-pointer slot. The engine's real Broadcast()
//   resolves FunctionName via UClass::FindFunctionByName() on Object's class
//   every time it fires, then calls Object->ProcessEvent(ResolvedFunction,
//   Params). So our "hook" has to be a real UFunction discoverable by name --
//   it can't just be a bare function pointer we hand the engine.
//
// Phase 1/2 (retired) resolved this by borrowing an EXISTING UFunction's name
// and overwriting that real UFunction's slot in the owning UClass's FuncMap/
// AllFunctionsCache (the hash maps UClass::FindFunctionByName reads) to point
// at our clone instead. That's a hijack of shared engine state: anything else
// that resolves the same function name via FindFunctionByName -- another
// delegate, a Blueprint binding -- silently got redirected into our clone
// too. This caused a real bug (CloseSessionConfirmationWidget firing the
// plugin callback when a player legitimately closed that dialog) and is the
// reason Phase 3 exists.
//
// How Phase 3 works instead -- "synthetic name, private resolution":
//   1. Caller supplies an existing, already-alive UObject plus the class/
//      function name of one of its existing native UFunctions, used PURELY
//      as a safe field-layout template (FunctionFlags/NumParms/ParmsSize/
//      ReturnValueOffset) -- resolved via Hooks::GObjectWalk::ResolveUFunction,
//      same path ufunction_resolve.h uses. This template is never modified;
//      nothing about the real engine's resolution of it changes.
//   2. We clone it (flat memcpy via EngineAllocator::Alloc, NOT a real
//      GC-tracked UObject -- its lifetime is entirely ours), give the clone
//      a brand-new synthetic FName nothing else in the game will ever
//      produce (see "Synthetic FName" below), and point its ExecFunction at
//      our dispatcher.
//   3. We hook UClass::FindFunctionByName itself (AOB-scanned, see
//      scan_patterns.h) so that when called with one of our synthetic names,
//      it returns our clone directly -- no UClass/FuncMap/AllFunctionsCache
//      raw-memory access at all. Every other name falls through to the real
//      implementation completely untouched.
//   4. We append FScriptDelegate{WeakObjectPtr(hostObject), clone->Name} to
//      the target delegate's InvocationList by growing the TArray with the
//      engine's own allocator -- unchanged from Phase 1/2, this part was
//      never the problem.
//
// Synthetic FName, without needing an FName-from-string constructor:
//   FName equality is just (ComparisonIndex, Number) -- no string-table
//   lookup (Basic.hpp FName::operator==). So a synthetic name doesn't need to
//   be a real interned string: we borrow ComparisonIndex 0 (the index "None"
//   uses) and hand out Number values from a private range starting at
//   0x70000000 via a process-wide atomic counter -- a range the engine will
//   essentially never legitimately produce for the literal "None" name. This
//   is an assumption, not a hard guarantee.
//
// Multiple concurrent hooks are supported, including more than one hook on
// the SAME delegatePtr (each HookViaExistingUFunction call splices in its
// own independent synthetic clone -- same as real UE AddDynamic supporting
// multiple binds to one delegate). Each call returns its own opaque handle;
// Unhook/IsHooked operate on that handle, not the delegatePtr, since a given
// delegatePtr may now have several live hooks at once.
//
// Status: struct offsets (FUObjectItem::SerialNumber, delegate InvocationList,
// FWeakObjectPtr field order, UFunction layout) verified against this game's
// binary via IDA Local Types -- see delegate_hook.cpp file header. UClass::
// FindFunctionByName's AOB pattern/signature supplied directly (also via IDA).
// Confirmed live (2026-06-21) to survive a real Broadcast() round trip,
// including multiple concurrent hooks on the same delegate. Wired into
// plugins/plugin_interface.h as IPluginDelegateHook (hooks->Delegate).
//
// Deliberately parameterless: callbacks only receive userContext, not the
// delegate's broadcast arguments. Reading those would require interpreting
// FFrame::Locals, whose offset isn't covered by this SDK dump and wasn't
// worth the risk for what is meant to be a simple "something happened"
// signal -- every supported use case so far (OnAfterSave, OnSaveLoaded, etc.)
// is a TMulticastInlineDelegate<void()> anyway.
// ---------------------------------------------------------------------------

namespace Hooks::DelegateHook
{
	// userContext is whatever opaque pointer the caller passed to
	// HookViaExistingUFunction -- handed back unchanged on every fire, so a
	// plugin can tell apart multiple registrations sharing the same plain
	// callback instead of needing a distinct function per registration.
	// No delegate-argument marshaling -- see file header.
	using DelegateCallback = void(*)(void* userContext);

	// Opaque handle identifying one HookViaExistingUFunction call. 0 is never
	// a valid handle.
	using HookHandle = uint64_t;

	// Hooks delegatePtr (must point at a TMulticastInlineDelegate<...> --
	// any arity; InvocationList is always TArray<FScriptDelegate>) by
	// splicing in a synthetic UFunction cloned from a template UFunction on
	// hostObject's class (used only as a field-layout template -- never
	// modified; see file header). See file header for the mechanism and its
	// caveats. Safe to call again with the same OR a different delegatePtr
	// while other hooks are active -- each call gets its own independent
	// synthetic clone and handle.
	//
	// hostClassName/hostFuncName are OPTIONAL. Since the template's only job
	// is to provide a safe, native FunctionFlags/layout to copy -- its actual
	// behavior is irrelevant and is never invoked -- there's no reason every
	// caller should need to find and supply one themselves. Leave both null
	// (the default) to use the modloader's own built-in template, resolved
	// once and cached. Only pass your own if you have a specific reason to
	// (e.g. avoiding a dependency on the built-in default's continued
	// existence across game updates).
	//
	// Returns 0 if the template UFunction can't be resolved, the
	// FindFunctionByName detour fails to install, or InvocationList can't be
	// grown (engine allocator unavailable). Otherwise returns a handle for
	// Unhook/IsHooked.
	HookHandle HookViaExistingUFunction(
		void* delegatePtr,
		SDK::UObject* hostObject,
		DelegateCallback callback,
		void* userContext = nullptr,
		const char* hostClassName = nullptr,
		const char* hostFuncName = nullptr);

	// Removes the FScriptDelegate entry this handle spliced in and frees its
	// synthetic clone. False if handle is not (or no longer) active.
	bool Unhook(HookHandle handle);

	// True if handle currently has an active hook installed via this module.
	bool IsHooked(HookHandle handle);
}
