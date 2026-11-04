#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/scanner.h"
#include "memory_scanner/scan_validation.h"
#include "memory_scanner/version_map.h"
#include "logging/logger.h"
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
		uintptr_t RecordResult(const IPluginSelf* self, const ScanValidation::Request& request,
			const ScanValidation::Result& result, bool required)
		{
			if (result.Succeeded())
			{
				PluginHookReport::RecordResolved(self, request.name.c_str());
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

		uintptr_t RunAndRecord(const IPluginSelf* self, const ScanValidation::Request& request, bool required)
		{
			if (!InSession(self))
				return 0;
			return RecordResult(self, request, ScanValidation::Resolve(request), required);
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
					"ResolveRequired/ResolveOptional called with an empty name or pattern", required,
					PluginHookReport::Cause::Request);
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
					"ResolveUnique called with an empty name or no patterns", required,
					PluginHookReport::Cause::Request);
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
					PluginHookReport::RecordResolved(self, hookName);
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

		// A map carries a few builds per key; anything near this is not a map.
		constexpr uint32_t kMaxCandidates = 64;

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
					"Resolve called with a null request", true, PluginHookReport::Cause::Request);
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
					"Initialise it with PLUGIN_SCAN_REQUEST_INIT.", true, PluginHookReport::Cause::Request);
				return 0;
			}

			memcpy(&local, request, given < sizeof(PluginScanRequest) ? given : sizeof(PluginScanRequest));

			const bool required = (local.flags & PLUGIN_SCAN_FLAG_OPTIONAL) == 0;

			// v72. With PLUGIN_SCAN_FLAG_MAP_KEY, hookName is a key in the loader's
			// version map, and pattern and candidates are the plugin's own, each
			// optional (VersionMap::ResolveMapKey).
			const bool mapKey     = (local.flags & PLUGIN_SCAN_FLAG_MAP_KEY) != 0;
			const bool hasPattern = local.pattern && local.pattern[0];
			if (!local.hookName || !local.hookName[0] || (!hasPattern && !mapKey))
			{
				PluginHookReport::RecordFailure(self, local.hookName ? local.hookName : "(unnamed)",
					"Resolve called with an empty hookName or pattern", required,
					PluginHookReport::Cause::Request);
				return 0;
			}

			ScanValidation::Request validation;
			validation.name         = local.hookName;
			validation.pattern      = hasPattern ? local.pattern : "";
			validation.kind         = ToValidationKind(local.kind);
			validation.module       = local.module;
			validation.resultOffset = local.resultOffset;
			validation.vtableSlots  = local.vtableSlots;
			validation.followRel32At =
				(local.flags & PLUGIN_SCAN_FLAG_FOLLOW_REL32) ? local.followRel32At : -1;

			const uint32_t rowCount = local.candidates ? local.candidateCount : 0;
			if (!mapKey && rowCount == 0)
				return RunAndRecord(self, validation, required);

			// v72: locators for other game builds. The loader picks the one for
			// this build (memory_scanner/version_map.h); kind and the rest of the
			// request stay what the plugin was built with.
			if (local.module && local.module != GetModuleHandleW(nullptr))
			{
				PluginHookReport::RecordFailure(self, local.hookName, mapKey
					? "Resolve called with a map key for a module other than the game exe; the map is for the game exe only"
					: "Resolve called with candidates for a module other than the game exe; "
					  "locators are for the game exe only", required, PluginHookReport::Cause::Request);
				return 0;
			}
			if (rowCount > kMaxCandidates)
			{
				PluginHookReport::RecordFailure(self, local.hookName,
					("Resolve called with " + std::to_string(local.candidateCount) + " candidates; at most "
					 + std::to_string(kMaxCandidates) + " are read").c_str(), required,
					PluginHookReport::Cause::Request);
				return 0;
			}

			std::vector<VersionMap::Candidate> candidates;
			candidates.reserve(rowCount);
			for (uint32_t i = 0; i < rowCount; ++i)
			{
				const PluginScanCandidate& c = local.candidates[i];
				VersionMap::Candidate v;
				v.gameCL        = c.gameCL;
				v.imageStamp    = c.imageStamp;
				v.imageSize     = c.imageSize;
				v.pattern       = c.pattern ? c.pattern : "";
				v.resultOffset  = c.resultOffset;
				v.followRel32At = (c.flags & PLUGIN_SCAN_FLAG_FOLLOW_REL32) ? c.followRel32At : -1;
				v.rva           = c.rva;
				candidates.push_back(std::move(v));
			}

			const ScanValidation::Result result = mapKey
				? VersionMap::ResolveMapKey(self->name, validation, candidates, VersionMap::RunningExe(),
					PluginHookReport::SessionNameShared(self))
				: VersionMap::Select(self->name, validation, candidates, VersionMap::RunningExe());
			return RecordResult(self, validation, result, required);
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
			PluginHookReport::RecordFailure(self, hookName, detail, true, PluginHookReport::Cause::Plugin);
		}

		// Same verdict as ReportFailure -- the only difference is the label the
		// report shows. A plugin that has decided something is wrong enough to
		// tell the loader about does not get to also keep loading.
		void HookReportWarning(const IPluginSelf* self, const char* hookName, const char* detail)
		{
			PluginHookReport::RecordFailure(self, hookName, detail, false, PluginHookReport::Cause::Plugin);
		}

		bool HookHasFailures(const IPluginSelf* self)
		{
			return PluginHookReport::SessionHasFailures(self);
		}

		// v72. Reads the request no further than the plugin's structSize, as
		// Resolve does, and records any failure like a missed pattern.
		bool HookResolveField(const IPluginSelf* self, const PluginFieldRequest* request, PluginFieldResult* result)
		{
			if (!InSession(self))
				return false;

			PluginFieldRequest local = PLUGIN_FIELD_REQUEST_INIT;
			if (request && request->structSize >= offsetof(PluginFieldRequest, className))
				memcpy(&local, request, request->structSize < sizeof(local) ? request->structSize : sizeof(local));
			const char* key = local.key && local.key[0] ? local.key : "(unnamed)";

			if (!request || !result || request->structSize < sizeof(PluginFieldRequest) || !local.key || !local.key[0]
				|| !local.className || !local.className[0] || (local.candidateCount && !local.candidates)
				|| local.candidateCount > kMaxCandidates)
			{
				PluginHookReport::RecordFailure(self, key, "bad field request", true, PluginHookReport::Cause::Request);
				return false;
			}
			// The map's ModLoader/ keys are the loader's own, as for a scan.
			if (self->name && strcmp(self->name, "ModLoader") == 0)
			{
				PluginHookReport::RecordFailure(self, key, "a plugin may not be named ModLoader", true, PluginHookReport::Cause::Request);
				return false;
			}

			VersionMap::FieldRequest field;
			field.key         = local.key;
			field.className   = local.className;
			field.member      = local.member ? local.member : "";
			field.flags       = local.flags;
			field.size        = local.size;
			field.builtCL     = local.builtCL;
			field.builtOffset = local.builtOffset;
			for (uint32_t i = 0; i < local.candidateCount; ++i)
			{
				const PluginFieldCandidate& c = local.candidates[i];
				field.candidates.push_back(VersionMap::FieldCandidate{ c.gameCL, c.offset, c.size, c.boolMask,
					c.member ? c.member : "" });
			}

			VersionMap::FieldValue value;
			std::string error;
			if (!VersionMap::SelectField(self->name, field, VersionMap::RunningExe(), value, error))
			{
				PluginHookReport::RecordFailure(self, key, error.c_str(), true);
				return false;
			}
			PluginHookReport::RecordFieldResolved(self, key);
			result->offset   = value.offset;
			result->size     = value.size;
			result->boolMask = value.boolMask;
			return true;
		}

		// v72. Read as far as the plugin's own structSize, as the requests are.
		// A malformed description declares nothing, so the keys it listed stay
		// core and a miss on one still refuses the plugin.
		bool HookDeclareFeature(const IPluginSelf* self, const PluginFeatureDesc* desc)
		{
			if (!InSession(self))
				return false;

			constexpr size_t kMinSize = offsetof(PluginFeatureDesc, configKeys);
			if (!desc || desc->structSize < kMinSize)
				return false;

			PluginFeatureDesc local = PLUGIN_FEATURE_DESC_INIT;
			memcpy(&local, desc, desc->structSize < sizeof(local) ? desc->structSize : sizeof(local));

			// A map holds a few dozen keys per feature; anything near this is not a feature.
			constexpr uint32_t kMaxKeys = 256;
			if (!local.id || !local.id[0] || !local.title || !local.title[0]
				|| !local.keys || local.keyCount == 0 || local.keyCount > kMaxKeys
				|| (local.configKeyCount && !local.configKeys) || local.configKeyCount > kMaxKeys)
			{
				ModLoaderLogger::LogWarn(L"[Map] %S: DeclareFeature(%S) refused: bad description",
					self->name ? self->name : "?", local.id ? local.id : "(no id)");
				return false;
			}

			std::vector<std::string> keys;
			for (uint32_t i = 0; i < local.keyCount; ++i)
			{
				if (!local.keys[i] || !local.keys[i][0])
				{
					ModLoaderLogger::LogWarn(L"[Map] %S: DeclareFeature(%S) refused: an empty key",
						self->name ? self->name : "?", local.id);
					return false;
				}
				keys.push_back(local.keys[i]);
			}
			std::vector<std::string> configKeys;
			for (uint32_t i = 0; i < local.configKeyCount; ++i)
				if (local.configKeys[i] && local.configKeys[i][0])
					configKeys.push_back(local.configKeys[i]);

			return PluginHookReport::DeclareFeature(self, local.id, local.title, keys, configKeys, local.enabled);
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
			HookResolve,
			HookResolveField,
			HookDeclareFeature
		};
	}

	IPluginHookScanner* GetPluginHookScanner()
	{
		return &g_hookScanner;
	}
}
