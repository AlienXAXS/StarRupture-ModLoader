#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Common hooking infrastructure
// Shared by all hook modules
// ---------------------------------------------------------------------------

namespace Hooks
{
	// ---------------------------------------------------------------------------
	// Lightweight x64 instruction length decoder
	// Returns the length of the instruction at the given address
	// ---------------------------------------------------------------------------
	size_t GetInstructionLength(const uint8_t* code);

	// ---------------------------------------------------------------------------
	// Calculate minimum bytes to steal for a hook
	// Returns total bytes needed to cover at least minBytes worth of complete instructions
	// ---------------------------------------------------------------------------
	size_t CalculateStolenBytes(const uint8_t* code, size_t minBytes);

	struct Hook
	{
		uintptr_t target = 0; // Address we hooked
		uintptr_t detour = 0; // Our replacement function
		uint8_t* trampoline = nullptr; // Raw path only -- the chain owns this otherwise
		uint8_t originalBytes[64]{}; // Raw path only
		size_t patchSize = 0; // Raw path only
		uint64_t linkId = 0; // Hooks::Broker link, when installed through Install()
		bool installed = false;

		// Install a hook at `target`. `originalFunc` receives a pointer to call
		// the original code -- cast it to the right function pointer type.
		//
		// Goes through Hooks::Broker (hooks/hook_broker.h), so several callers can
		// hook ONE address and each gets an `originalFunc` that runs the next one.
		// Two Hook objects on the same address used to corrupt each other: the
		// second Install decoded the first one's JMP stub as if it were the
		// function prologue and copied its 8-byte address literal into a
		// trampoline as though it were code.
		//
		// The pointer handed back is a broker thunk rather than the trampoline
		// itself, so the chain can be rewired underneath you when a link in front
		// of yours is removed. Call it, do not inspect it.
		//
		// owner/name are recorded for diagnostics only (the `hooks` console
		// command); both may be omitted by loader-internal callers.
		bool Install(uintptr_t target, void* detour, void** originalFunc,
			     const char* owner = nullptr, const char* name = nullptr);

		// Remove this hook's link. The original prologue is restored only when
		// the LAST link on that address goes -- one caller unhooking does not
		// unhook the others.
		void Remove();

		// --- Broker-internal ---------------------------------------------------
		//
		// The raw prologue patch, with no chain around it. Hooks::Broker owns one
		// of these per hooked address and is the only thing that may call them;
		// everything else uses Install/Remove above. Calling InstallRaw directly
		// on an address that is already hooked is exactly the corruption the
		// broker exists to prevent.
		bool InstallRaw(uintptr_t target, void* jumpTo, void** outTrampoline);
		void RemoveRaw();
	};

	// ---------------------------------------------------------------------------
	// Caller module identification
	//
	// Returns the base filename (e.g. "StarRupture-Win64-Shipping.exe") of the
	// module that contains `addr`.  Pass _ReturnAddress() from inside a detour
	// to find out which DLL/EXE called into the hooked function.
	//
	// IMPORTANT: call _ReturnAddress() in the detour itself, NOT inside this
	// function -- the intrinsic reads the current stack frame's return address,
	// so it must be called at the site where you want the caller identified.
	//
	// Typical usage in a detour:
	//   ModLoaderLogger::LogTrace(L"[MyHook] Caller: %S",
	//       Hooks::GetCallerModuleName(_ReturnAddress()).c_str());
	// ---------------------------------------------------------------------------
	inline std::string GetCallerModuleName(void* addr)
	{
		if (!addr)
			return "<null>";

		HMODULE hMod = nullptr;
		if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(addr),
			&hMod) || !hMod)
			return "<unknown>";

		wchar_t path[MAX_PATH]{};
		if (!GetModuleFileNameW(hMod, path, MAX_PATH))
			return "<unknown>";

		// Strip directory — only keep the filename portion
		const wchar_t* slash = wcsrchr(path, L'\\');
		const wchar_t* name  = slash ? slash + 1 : path;

		char buf[MAX_PATH]{};
		WideCharToMultiByte(CP_ACP, 0, name, -1, buf, MAX_PATH, nullptr, nullptr);
		return buf;
	}

	// Simple memory patching utilities
	bool Patch(uintptr_t address, const uint8_t* data, size_t size);
	bool Nop(uintptr_t address, size_t size);

	// Read memory safely
	bool ReadMemory(uintptr_t address, void* buffer, size_t size);

	// ---------------------------------------------------------------------------
	// VTable Hook
	//
	// Hooks a virtual function by patching the vtable entry directly.
	// This avoids the need for unique byte patterns (which virtual functions
	// often don't have since they're short stubs).
	//
	// Usage:
	//   1. Get a UObject instance (e.g., via StaticClass()->GetDefaultObject())
	//   2. Read the vtable pointer from offset 0x00 of the object
	//   3. Specify the vtable slot index for the virtual function
	//   4. The hook replaces the function pointer in the vtable
	//
	// To find vtable slot indices:
	//   - In IDA, find the vtable for the class
	//   - Count the function pointer entries (each is 8 bytes on x64)
	//   - The slot index is the position (0-based) in the vtable
	// ---------------------------------------------------------------------------
	struct VTableHook
	{
		uintptr_t vtableAddr = 0; // Address of the vtable
		size_t slotIndex = 0; // Which vtable slot we hooked
		uintptr_t originalFunc = 0; // Original function pointer (saved)
		bool installed = false;

		// Install by patching a vtable slot.
		// `objectInstance` - pointer to any instance of the class (for reading vtable ptr)
		// `vtableSlotIndex` - 0-based index into the vtable
		// `detour` - your replacement function
		// `outOriginal` - receives the original function pointer
		bool Install(void* objectInstance, size_t vtableSlotIndex, void* detour, void** outOriginal);

		// Install by directly specifying the vtable address (if you already know it)
		bool InstallByVTableAddr(uintptr_t vtableAddress, size_t vtableSlotIndex, void* detour, void** outOriginal);

		// Remove the hook, restoring the original vtable entry
		void Remove();
	};
}
