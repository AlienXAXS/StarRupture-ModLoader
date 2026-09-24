#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/scanner.h"
#include "memory_scanner/scan_validation.h"
#include "plugins/plugin_hook_report.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace ModLoaderLogger
{
	namespace
	{
		// Every entry point starts here. A call with no open session is a plugin
		// using a stashed table pointer from PluginInit or later -- the whole
		// reason scanning was moved into an event -- so it is refused rather than
		// quietly served. PluginHookReport logs which plugin did it.
		bool InSession(const IPluginSelf* self)
		{
			return PluginHookReport::HasSession(self);
		}

		std::string ModuleName(HMODULE module)
		{
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(module, path, MAX_PATH))
				return "(unknown module)";
			const char* slash = strrchr(path, '\\');
			return slash ? slash + 1 : path;
		}

		// The one place a validated resolve is recorded. Everything the plugin
		// can call funnels through here so that success, failure and the wording
		// of the failure are decided once.
		//
		// `required` is a label on the report line, not a verdict: any failure
		// refuses the plugin (see plugin_hook_report.h). What the label buys is
		// a readable report -- it says which addresses the author expected to be
		// able to lose.
		uintptr_t RunAndRecord(const IPluginSelf* self, const ScanValidation::Request& request, bool required)
		{
			if (!InSession(self))
				return 0;

			const ScanValidation::Result result = ScanValidation::Resolve(request);

			if (result.Succeeded())
			{
				PluginHookReport::RecordResolved(self);
				return result.address;
			}

			// The kind is worth carrying into the report even when the kind is
			// not what failed: "declared as function start" is the first thing
			// that tells a reader whether the author opted out of the check.
			std::string detail = result.detail;
			if (request.kind == ScanValidation::Kind::Any)
				detail += "\r\n            (declared PLUGIN_SCAN_ANY -- no structural check was requested)";
			else
			{
				detail += "\r\n            (declared kind: ";
				detail += ScanValidation::KindName(request.kind);
				detail += ")";
			}

			if (request.module)
			{
				detail += "\r\n            (scanned module: " + ModuleName(request.module) + ")";
			}

			PluginHookReport::RecordFailure(self, request.name.c_str(), detail.c_str(), required);
			return 0;
		}

		// Shared body of the four legacy single-pattern resolves.
		//
		// These predate scan kinds, so they scan with Kind::Any: uniqueness is
		// enforced (it is a property of the pattern, not of what the caller
		// declared) but the structural check is not, because a plugin built
		// against an older header never got the chance to say what it wanted.
		// The report labels them unvalidated so that is visible to whoever reads
		// it.
		uintptr_t ResolveLegacy(const IPluginSelf* self, const char* hookName, const char* pattern,
			HMODULE module, bool required)
		{
			if (!InSession(self))
				return 0;

			if (!hookName || !hookName[0] || !pattern || !pattern[0])
			{
				PluginHookReport::RecordFailure(self, hookName ? hookName : "(unnamed)",
					"ResolveRequired/ResolveOptional called with an empty name or pattern", required);
				return 0;
			}

			ScanValidation::Request request;
			request.name    = hookName;
			request.pattern = pattern;
			request.kind    = ScanValidation::Kind::Any;
			request.module  = module;

			return RunAndRecord(self, request, required);
		}

		uintptr_t ResolveUnique(const IPluginSelf* self, const char* hookName,
			const char** patterns, int patternCount, int* outPatternIndex, bool required)
		{
			if (!InSession(self))
				return 0;

			if (!hookName || !hookName[0] || !patterns || patternCount <= 0)
			{
				PluginHookReport::RecordFailure(self, hookName ? hookName : "(unnamed)",
					"ResolveUnique called with an empty name or no patterns", required);
				return 0;
			}

			// Each candidate gets the full uniqueness treatment, and the first
			// one that resolves cleanly wins. Nothing is recorded per candidate:
			// a candidate list is explicitly "try these in order", so a candidate
			// that missed is not a failure, only the list running out is.
			std::string tried;
			for (int i = 0; i < patternCount; ++i)
			{
				if (!patterns[i] || !patterns[i][0])
					continue;

				ScanValidation::Request request;
				request.name    = hookName;
				request.pattern = patterns[i];
				request.kind    = ScanValidation::Kind::Any;

				const ScanValidation::Result result = ScanValidation::Resolve(request);
				if (result.Succeeded())
				{
					if (outPatternIndex)
						*outPatternIndex = i;
					PluginHookReport::RecordResolved(self);
					return result.address;
				}

				tried += "\r\n          ";
				tried += patterns[i];
				tried += "\r\n            -> ";
				tried += result.detail;
			}

			std::string detail = "no candidate pattern resolved to exactly one address:";
			detail += tried;
			PluginHookReport::RecordFailure(self, hookName, detail.c_str(), required);
			return 0;
		}

		// --- IPluginHookScanner entry points -----------------------------------

		uintptr_t HookResolveRequired(const IPluginSelf* self, const char* hookName, const char* pattern)
		{
			return ResolveLegacy(self, hookName, pattern, nullptr, true);
		}

		uintptr_t HookResolveOptional(const IPluginSelf* self, const char* hookName, const char* pattern)
		{
			return ResolveLegacy(self, hookName, pattern, nullptr, false);
		}

		uintptr_t HookResolveRequiredInModule(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern)
		{
			return ResolveLegacy(self, hookName, pattern, module, true);
		}

		uintptr_t HookResolveOptionalInModule(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern)
		{
			return ResolveLegacy(self, hookName, pattern, module, false);
		}

		uintptr_t HookResolveRequiredUnique(const IPluginSelf* self, const char* hookName,
			const char** patterns, int patternCount, int* outPatternIndex)
		{
			return ResolveUnique(self, hookName, patterns, patternCount, outPatternIndex, true);
		}

		uintptr_t HookResolveOptionalUnique(const IPluginSelf* self, const char* hookName,
			const char** patterns, int patternCount, int* outPatternIndex)
		{
			return ResolveUnique(self, hookName, patterns, patternCount, outPatternIndex, false);
		}

		ScanValidation::Kind ToValidationKind(int pluginKind)
		{
			switch (pluginKind)
			{
			case PLUGIN_SCAN_FUNCTION_START: return ScanValidation::Kind::FunctionStart;
			case PLUGIN_SCAN_IN_FUNCTION:    return ScanValidation::Kind::InFunction;
			case PLUGIN_SCAN_CODE:           return ScanValidation::Kind::Code;
			case PLUGIN_SCAN_DATA:           return ScanValidation::Kind::Data;
			case PLUGIN_SCAN_VTABLE:         return ScanValidation::Kind::VTable;
			case PLUGIN_SCAN_ANY:            return ScanValidation::Kind::Any;
			case PLUGIN_SCAN_UNSPECIFIED:
			default:                         return ScanValidation::Kind::Unspecified;
			}
		}

		// v68. The request is read only as far as the plugin's own structSize,
		// so a field appended to PluginScanRequest later costs a plugin built
		// against this header nothing -- it simply keeps the default.
		//
		// A structSize of 0 is a request that was never initialised (the
		// PLUGIN_SCAN_REQUEST_INIT macro sets it), and is refused rather than
		// guessed at: every other field in it is equally untrustworthy.
		uintptr_t HookResolve(const IPluginSelf* self, const PluginScanRequest* request)
		{
			if (!InSession(self))
				return 0;

			if (!request)
			{
				PluginHookReport::RecordFailure(self, "(unnamed)",
					"Resolve called with a null request", true);
				return 0;
			}

			constexpr size_t kMinSize = offsetof(PluginScanRequest, kind) + sizeof(int);

			PluginScanRequest local{};
			local.followRel32At = -1;

			const size_t given = request->structSize;
			if (given < kMinSize)
			{
				PluginHookReport::RecordFailure(self, "(unnamed)",
					"Resolve called with an uninitialised PluginScanRequest (structSize too small). "
					"Initialise it with PLUGIN_SCAN_REQUEST_INIT.", true);
				return 0;
			}

			memcpy(&local, request, given < sizeof(PluginScanRequest) ? given : sizeof(PluginScanRequest));

			const bool required = (local.flags & PLUGIN_SCAN_FLAG_OPTIONAL) == 0;

			if (!local.hookName || !local.hookName[0] || !local.pattern || !local.pattern[0])
			{
				PluginHookReport::RecordFailure(self, local.hookName ? local.hookName : "(unnamed)",
					"Resolve called with an empty hookName or pattern", required);
				return 0;
			}

			ScanValidation::Request validation;
			validation.name         = local.hookName;
			validation.pattern      = local.pattern;
			validation.kind         = ToValidationKind(local.kind);
			validation.module       = local.module;
			validation.resultOffset = local.resultOffset;
			validation.vtableSlots  = local.vtableSlots;
			validation.followRel32At =
				(local.flags & PLUGIN_SCAN_FLAG_FOLLOW_REL32) ? local.followRel32At : -1;

			return RunAndRecord(self, validation, required);
		}

		int CopyAddresses(const std::vector<uintptr_t>& results, uintptr_t* out, int maxResults)
		{
			const int count = static_cast<int>(results.size());
			if (out && maxResults > 0)
			{
				const int toCopy = count < maxResults ? count : maxResults;
				for (int i = 0; i < toCopy; ++i)
					out[i] = results[i];
			}
			return count;
		}

		int CopyXRefs(const std::vector<Scanner::XRef>& xrefs, PluginXRef* out, int maxResults)
		{
			const int count = static_cast<int>(xrefs.size());
			if (out && maxResults > 0)
			{
				const int toCopy = count < maxResults ? count : maxResults;
				for (int i = 0; i < toCopy; ++i)
				{
					out[i].address    = xrefs[i].address;
					out[i].isRelative = xrefs[i].isRelative;
				}
			}
			return count;
		}

		int HookFindAllPatternsInMainModule(const IPluginSelf* self, const char* pattern,
			uintptr_t* outAddresses, int maxResults)
		{
			if (!InSession(self) || !pattern) return 0;
			return CopyAddresses(Scanner::FindAllPatternsInMainModule(std::string(pattern)), outAddresses, maxResults);
		}

		int HookFindAllPatternsInModule(const IPluginSelf* self, HMODULE module, const char* pattern,
			uintptr_t* outAddresses, int maxResults)
		{
			if (!InSession(self) || !pattern) return 0;
			return CopyAddresses(Scanner::FindAllPatternsInModule(module, std::string(pattern)), outAddresses, maxResults);
		}

		int HookFindXrefsToAddress(const IPluginSelf* self, uintptr_t targetAddress, uintptr_t start, size_t size,
			PluginXRef* outXRefs, int maxResults)
		{
			if (!InSession(self)) return 0;
			return CopyXRefs(Scanner::FindXrefsToAddress(targetAddress, start, size), outXRefs, maxResults);
		}

		int HookFindXrefsToAddressInModule(const IPluginSelf* self, uintptr_t targetAddress, HMODULE module,
			PluginXRef* outXRefs, int maxResults)
		{
			if (!InSession(self)) return 0;
			return CopyXRefs(Scanner::FindXrefsToAddressInModule(targetAddress, module), outXRefs, maxResults);
		}

		int HookFindXrefsToAddressInMainModule(const IPluginSelf* self, uintptr_t targetAddress,
			PluginXRef* outXRefs, int maxResults)
		{
			if (!InSession(self)) return 0;
			return CopyXRefs(Scanner::FindXrefsToAddressInMainModule(targetAddress), outXRefs, maxResults);
		}

		void HookReportFailure(const IPluginSelf* self, const char* hookName, const char* detail)
		{
			PluginHookReport::RecordFailure(self, hookName, detail, true);
		}

		// Same verdict as ReportFailure -- the only difference is the label the
		// report shows. A plugin that has decided something is wrong enough to
		// tell the loader about does not get to also keep loading.
		void HookReportWarning(const IPluginSelf* self, const char* hookName, const char* detail)
		{
			PluginHookReport::RecordFailure(self, hookName, detail, false);
		}

		bool HookHasFailures(const IPluginSelf* self)
		{
			return PluginHookReport::SessionHasFailures(self);
		}

		IPluginHookScanner g_hookScanner = {
			HookResolveRequired,
			HookResolveOptional,
			HookResolveRequiredInModule,
			HookResolveOptionalInModule,
			HookResolveRequiredUnique,
			HookResolveOptionalUnique,
			HookFindAllPatternsInMainModule,
			HookFindAllPatternsInModule,
			HookFindXrefsToAddress,
			HookFindXrefsToAddressInModule,
			HookFindXrefsToAddressInMainModule,
			HookReportFailure,
			HookReportWarning,
			HookHasFailures,
			HookResolve
		};
	}

	IPluginHookScanner* GetPluginHookScanner()
	{
		return &g_hookScanner;
	}
}
