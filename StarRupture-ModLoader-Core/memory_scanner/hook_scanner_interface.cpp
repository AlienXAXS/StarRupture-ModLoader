#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/scanner.h"
#include "plugins/plugin_hook_report.h"

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

		// "<what>: <pattern>", the detail line the report and clipboard text show
		// under the hook name.
		std::string MissDetail(const char* what, const char* pattern)
		{
			std::string s = what;
			s += ": ";
			s += pattern ? pattern : "(null)";
			return s;
		}

		std::string ModuleName(HMODULE module)
		{
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(module, path, MAX_PATH))
				return "(unknown module)";
			const char* slash = strrchr(path, '\\');
			return slash ? slash + 1 : path;
		}

		// Shared body of the four single-pattern resolves. module == nullptr
		// means the main module, which is also the only form that gets the scan
		// cache (it is keyed on the main module's base and the game version).
		//
		// `required` is recorded as a label only: a miss refuses the plugin
		// either way (see plugin_hook_report.h). Optional still means something
		// to the plugin -- it is the resolve whose null return it is expected to
		// handle -- but it does not buy it a load.
		uintptr_t Resolve(const IPluginSelf* self, const char* hookName, const char* pattern,
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

			const uintptr_t addr = module
				? Scanner::FindPatternInModule(module, std::string(pattern))
				: Scanner::FindPatternInMainModule(std::string(hookName), std::string(pattern));

			if (addr)
			{
				PluginHookReport::RecordResolved(self);
				return addr;
			}

			std::string detail = module
				? MissDetail(("pattern not found in " + ModuleName(module)).c_str(), pattern)
				: MissDetail("pattern not found", pattern);
			PluginHookReport::RecordFailure(self, hookName, detail.c_str(), required);
			return 0;
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

			std::vector<std::string> patternVec;
			patternVec.reserve(static_cast<size_t>(patternCount));
			for (int i = 0; i < patternCount; ++i)
				if (patterns[i]) patternVec.push_back(std::string(patterns[i]));

			const uintptr_t addr = Scanner::FindUniquePattern(patternVec, outPatternIndex);
			if (addr)
			{
				PluginHookReport::RecordResolved(self);
				return addr;
			}

			// Which candidate failed is not knowable -- FindUniquePattern only
			// says that none of them matched exactly once -- so list them all.
			// That is also what an author needs to see to fix it.
			std::string detail = "no unique match among " + std::to_string(patternVec.size()) + " candidate pattern(s):";
			for (const std::string& p : patternVec)
			{
				detail += "\r\n          ";
				detail += p;
			}
			PluginHookReport::RecordFailure(self, hookName, detail.c_str(), required);
			return 0;
		}

		// --- IPluginHookScanner entry points -----------------------------------

		uintptr_t HookResolveRequired(const IPluginSelf* self, const char* hookName, const char* pattern)
		{
			return Resolve(self, hookName, pattern, nullptr, true);
		}

		uintptr_t HookResolveOptional(const IPluginSelf* self, const char* hookName, const char* pattern)
		{
			return Resolve(self, hookName, pattern, nullptr, false);
		}

		uintptr_t HookResolveRequiredInModule(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern)
		{
			return Resolve(self, hookName, pattern, module, true);
		}

		uintptr_t HookResolveOptionalInModule(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern)
		{
			return Resolve(self, hookName, pattern, module, false);
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
			HookHasFailures
		};
	}

	IPluginHookScanner* GetPluginHookScanner()
	{
		return &g_hookScanner;
	}
}
