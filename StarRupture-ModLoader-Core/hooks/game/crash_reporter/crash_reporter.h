#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Crash Reporter Suppression Hook (client only)
//
// Purpose: UE5's FCrashReportingThread::HandleCrashInternal spawns
//          CrashReportClient.exe (via anonymous_namespace_::
//          ReportCrashUsingCrashReportClient) on a fatal engine crash, which
//          uploads a report to the developers. Since the ModLoader hooks
//          native engine functions, any crash while it's installed is not
//          representative of the vanilla game and shouldn't be sent upstream.
//
//          ReportCrashUsingCrashReportClient is also called from two other,
//          non-fatal paths (ReportContinuableEventUsingCrashReportClient for
//          ensure() failures, and ReportHang for hang detection) -- those are
//          left completely untouched. This hook only intervenes when called
//          from the one specific call site inside HandleCrashInternal that
//          handles a real fatal crash, identified via the return address left
//          on the stack by that call site (see Install()).
//
// On a real fatal crash the detour shows the crash dialog (crash_dialog.h)
// with the exception details and a copyable stack trace, then returns
// without ever spawning CrashReportClient.exe.
// ---------------------------------------------------------------------------

namespace Hooks::CrashReporter
{
	// Install the hook
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Returns the trampoline address of the original
	// anonymous_namespace_::ReportCrashUsingCrashReportClient, or 0 if not yet installed.
	// Cast to: int64_t(__fastcall*)(void* InContext, void* ExceptionInfo, int32_t ReportUI)
	uintptr_t GetOriginalPtr();
}

#endif // MODLOADER_CLIENT_BUILD
