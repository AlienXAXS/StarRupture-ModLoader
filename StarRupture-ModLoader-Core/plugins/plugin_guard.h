#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// PluginGuard
//
// Keeps a plugin that crashes in a callback from taking the game down.
//
// Every callback the loader calls into a plugin (tick, panel and widget
// render, world/save/actor events, input, network, console and HTTP handlers)
// goes through Invoke or Call. A native fault inside it -- an access violation
// and the like, which a C++ catch does not see under /EHsc -- is caught there
// by __try/__except, the plugin is switched off, and the call returns false.
// From then on Invoke and Call skip every callback whose address is inside
// that plugin's DLL, on every thread, until it is unloaded or reloaded. The
// Plugins tab shows it as Crashed, with the exception code and the callback.
//
// Only a fault raised in the plugin's own code is caught: the callback lives
// in a plugin DLL and the faulting instruction is in that same DLL. A fault
// in engine or loader code that the callback called into is not caught, since
// unwinding past those frames would leave their locks held and their state
// half-written: it is logged once, naming the plugin and the callback it ran
// under, and goes on to the crash handler as it would without the guard. So
// does a fault in a callback of the loader's own. A call into an address that
// is no longer code at all (a callback left behind by a plugin that was
// unloaded) is caught and logged. C++ exceptions are not touched: they reach
// the caller's catch.
//
// Not covered: detours a plugin installs on game functions, the plugin's own
// threads, and its DllMain. Those run outside any loader call.
//
// Cost: one relaxed atomic load per call while no plugin has crashed, plus
// the x64 table-based __try, which costs nothing until something throws.
// ---------------------------------------------------------------------------

namespace PluginGuard
{
	// What the Plugins tab and the log say about a plugin that was switched off.
	struct Fault
	{
		uint32_t    code     = 0;        // exception code, e.g. 0xC0000005
		const char* callback = nullptr;  // the callback it crashed in (a string literal)
	};

	// What the reporter is told, once per plugin that is switched off.
	// `frames` is the faulting thread's call stack, walked in the filter while
	// it was still intact, innermost first.
	struct Report
	{
		HMODULE          module;
		const char*      pluginName;
		const char*      callback;
		uint32_t         code;
		uintptr_t        faultAddress;
		const uintptr_t* frames;
		size_t           frameCount;
	};
	using Reporter = void (*)(const Report& report);

	// Set once by the plugin manager. Called on the thread that faulted, after
	// the fault is contained, never more than once per plugin load.
	void SetReporter(Reporter reporter);

	// The plugin manager tells the guard which DLLs are plugins: Track once
	// LoadLibrary has returned, Untrack just before FreeLibrary. Untrack also
	// forgets a fault, so a reloaded plugin starts out running. Any thread.
	void Track(HMODULE module, const char* pluginName);
	void Untrack(HMODULE module);

	// True when the plugin `address` lives in has been switched off after a
	// fault. One relaxed atomic load while no plugin has crashed.
	bool IsOff(const void* address);

	// The fault that switched a plugin off; false while it is running.
	bool GetFault(HMODULE module, Fault* out);

	// The module an address lives in, or null when it is in none.
	HMODULE ModuleOf(const void* address);

	// Drops every element of `v` whose callback lives in `module`; `addressOf`
	// maps an element to that callback. Returns how many were dropped. The
	// caller holds whatever lock guards `v`.
	template <class Vec, class AddressOf>
	size_t ForgetModule(Vec& v, HMODULE module, AddressOf addressOf)
	{
		if (!module) return 0;
		size_t dropped = 0;
		for (auto it = v.begin(); it != v.end(); )
		{
			if (ModuleOf(reinterpret_cast<const void*>(addressOf(*it))) == module) { it = v.erase(it); ++dropped; }
			else ++it;
		}
		return dropped;
	}

	// Same, for a list of bare callback pointers.
	template <class Vec>
	size_t ForgetModule(Vec& v, HMODULE module)
	{
		return ForgetModule(v, module, [](const auto& fn) { return fn; });
	}

	namespace Detail
	{
		using Thunk = void (*)(void* body);
		bool Run(const void* callback, const char* what, Thunk thunk, void* body);
	}

	// Runs `body`, which calls the plugin callback `fn`, under the guard.
	// False when fn's plugin is switched off (body is not run) or when body
	// faulted (the plugin is now switched off). `what` names the callback for
	// the log and the Plugins tab and must be a string literal.
	//
	// Keep objects with destructors (locks, watchdog scopes) out of body: a
	// fault skips them under /EHsc. Put them around the Call instead.
	template <class Fn, class Body>
	bool Call(Fn fn, const char* what, Body&& body)
	{
		const void* address = reinterpret_cast<const void*>(fn);
		if (IsOff(address)) return false;
		using B = std::remove_reference_t<Body>;
		return Detail::Run(address, what, [](void* b) { (*static_cast<B*>(b))(); }, &body);
	}

	// Calls a void callback with `args`.
	template <class Fn, class... Args>
	bool Invoke(Fn fn, const char* what, Args... args)
	{
		return Call(fn, what, [&] { fn(args...); });
	}

	// Calls a callback that returns a value, into `*result`. On false,
	// `*result` is left as it was, so set it to the value a skipped plugin
	// should count as first.
	template <class R, class Fn, class... Args>
	bool InvokeInto(R* result, Fn fn, const char* what, Args... args)
	{
		return Call(fn, what, [&] { *result = fn(args...); });
	}
}
