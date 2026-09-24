#include "memory_scanner/scan_validation.h"
#include "memory_scanner/scanner.h"
#include "memory_scanner/scan_cache.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/patch_overlay.h"
#include "hooks/symbol_resolver.h"
#include "core/version_check.h"
#include "logging/logger.h"

#include <cstring>
#include <vector>

namespace
{
	// Hooks::Hook writes a 14-byte absolute JMP. A function shorter than that
	// cannot be detoured without overwriting whatever follows it, which is a
	// corruption that shows up somewhere else entirely -- so it is a resolve
	// failure here rather than a surprise later.
	constexpr size_t kMinHookableFunctionBytes = 14;

	// How many matches the failure report lists before it stops. A pattern with
	// 900 matches is a pattern with no concrete bytes in it; the first handful
	// says that just as well as all of them, and the count says the rest.
	constexpr size_t kMaxReportedMatches = 12;

	// Bound on the full enumeration run for a failed resolve. Without it, a
	// degenerate pattern ("48" on its own) would build a vector of millions of
	// addresses while the game main thread is parked.
	constexpr size_t kMaxEnumeratedMatches = 4096;

	const char* NL = "\r\n";

	std::string Hex(unsigned long long v)
	{
		char buf[32]{};
		sprintf_s(buf, "0x%llX", v);
		return buf;
	}

	std::string ModuleBaseName(HMODULE module)
	{
		char path[MAX_PATH]{};
		if (!GetModuleFileNameA(module, path, MAX_PATH))
			return "<module>";
		const char* slash = strrchr(path, '\\');
		return slash ? slash + 1 : path;
	}

	HMODULE ResolveModule(HMODULE requested)
	{
		return requested ? requested : GetModuleHandleW(nullptr);
	}

	bool ReadPointer(const Scanner::ImageInfo& image, uintptr_t addr, uintptr_t& out)
	{
		if (!image.valid) return false;
		if (addr < image.base) return false;
		if (addr + sizeof(uintptr_t) > image.base + image.size) return false;
		memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(uintptr_t));
		return true;
	}
}

namespace ScanValidation
{
	const char* KindName(Kind kind)
	{
		switch (kind)
		{
		case Kind::FunctionStart: return "function start";
		case Kind::InFunction:    return "inside a function";
		case Kind::Code:          return "code";
		case Kind::Data:          return "data";
		case Kind::VTable:        return "vtable";
		case Kind::Any:           return "any (unvalidated)";
		case Kind::Unspecified:
		default:                  return "unspecified";
		}
	}

	Kind ParseKind(const char* name)
	{
		if (!name || !name[0]) return Kind::Unspecified;

		if (_stricmp(name, "function") == 0 || _stricmp(name, "functionstart") == 0)
			return Kind::FunctionStart;
		if (_stricmp(name, "infunction") == 0)
			return Kind::InFunction;
		if (_stricmp(name, "code") == 0)
			return Kind::Code;
		if (_stricmp(name, "data") == 0)
			return Kind::Data;
		if (_stricmp(name, "vtable") == 0)
			return Kind::VTable;
		if (_stricmp(name, "any") == 0)
			return Kind::Any;

		return Kind::Unspecified;
	}

	std::string DescribeAddress(HMODULE module, uintptr_t address)
	{
		module = ResolveModule(module);

		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		std::string out = ModuleBaseName(module);

		if (image.valid && address >= image.base && address < image.base + image.size)
			out += "+" + Hex(address - image.base);
		else
			out = "at " + Hex(address) + " (outside " + out + ")";

		const Scanner::SectionInfo section = Scanner::GetSectionForAddress(module, address);
		out += "  ";
		out += section.valid ? section.name : "<no section>";

		const Scanner::FunctionInfo fn = Scanner::DescribeFunction(address);
		if (fn.valid)
		{
			if (fn.start == address && !fn.chainedChunk)
			{
				out += "  function start (" + Hex(fn.end - fn.start) + " bytes)";
			}
			else if (fn.chainedChunk)
			{
				out += "  separated chunk of function ";
				out += image.valid ? "+" + Hex(fn.start - image.base) : Hex(fn.start);
			}
			else
			{
				out += "  inside function ";
				out += image.valid ? "+" + Hex(fn.start - image.base) : Hex(fn.start);
				out += " (+" + Hex(address - fn.start) + ")";
			}
		}
		else if (section.valid && section.executable)
		{
			out += "  no unwind info";
		}

		// A PDB is not normally present next to a shipping build, so this stays
		// silent almost always. When someone has dropped one in, it turns the
		// whole report from "paste this RVA into IDA" into a name, so it is
		// worth the lookup on a path that only runs when something failed.
		uintptr_t displacement = 0;
		const std::string symbol = Hooks::SymbolResolver::Resolve(address, &displacement);
		if (!symbol.empty())
		{
			out += "  [" + symbol;
			if (displacement)
				out += "+" + Hex(displacement);
			out += "]";
		}

		return out;
	}

	namespace
	{
		// Applies resultOffset / followRel32At. Returns false with a reason when
		// the transform cannot be done inside the image.
		bool ApplyTransforms(const Request& req, HMODULE module, uintptr_t rawMatch,
			uintptr_t& outAddress, std::string& outError)
		{
			const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
			uintptr_t addr = rawMatch;

			if (req.followRel32At >= 0)
			{
				const uintptr_t site = rawMatch + static_cast<uintptr_t>(req.followRel32At);
				if (!image.valid || site + 5 > image.base + image.size)
				{
					outError = "followRel32At " + std::to_string(req.followRel32At)
					         + " points outside the image";
					return false;
				}

				const auto opcode = *reinterpret_cast<const uint8_t*>(site);
				if (opcode != 0xE8 && opcode != 0xE9)
				{
					outError = "followRel32At " + std::to_string(req.followRel32At)
					         + " expected a CALL (E8) or JMP (E9) opcode, found "
					         + Hex(opcode);
					return false;
				}

				int32_t rel32 = 0;
				memcpy(&rel32, reinterpret_cast<const void*>(site + 1), sizeof(rel32));
				addr = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));

				if (!image.valid || addr < image.base || addr >= image.base + image.size)
				{
					outError = "the rel32 at +" + std::to_string(req.followRel32At)
					         + " resolves to " + Hex(addr) + ", which is outside the image";
					return false;
				}
			}

			addr = static_cast<uintptr_t>(static_cast<intptr_t>(addr) + req.resultOffset);

			if (image.valid && (addr < image.base || addr >= image.base + image.size))
			{
				outError = "resultOffset " + std::to_string(req.resultOffset)
				         + " moves the result to " + Hex(addr) + ", which is outside the image";
				return false;
			}

			outAddress = addr;
			return true;
		}

		// The kind check itself. outWhy is filled only on failure.
		bool CheckKind(const Request& req, HMODULE module, uintptr_t address, std::string& outWhy)
		{
			switch (req.kind)
			{
			case Kind::Any:
				return true;

			case Kind::FunctionStart:
			{
				const Scanner::FunctionInfo fn = Scanner::DescribeFunction(address);
				if (!fn.valid)
				{
					outWhy = "no unwind entry covers this address, so it is not the start of a "
					         "compiled function (hand-written thunks legitimately have none -- "
					         "use kind 'code' for those)";
					return false;
				}
				if (fn.chainedChunk)
				{
					outWhy = "this is a separated (cold) chunk of a function whose real entry is at "
					       + Hex(fn.start) + " -- no caller ever enters here";
					return false;
				}
				if (fn.start != address)
				{
					outWhy = "this is " + Hex(address - fn.start) + " bytes INSIDE a function that starts at "
					       + Hex(fn.start) + "; either fix the pattern or set resultOffset to -"
					       + Hex(address - fn.start);
					return false;
				}

				const size_t length = fn.end > fn.start ? static_cast<size_t>(fn.end - fn.start) : 0;
				if (length < kMinHookableFunctionBytes)
				{
					outWhy = "the function is only " + std::to_string(length)
					       + " bytes long; a detour writes a " + std::to_string(kMinHookableFunctionBytes)
					       + "-byte JMP and would overwrite whatever follows it";
					return false;
				}
				return true;
			}

			case Kind::InFunction:
			{
				const Scanner::FunctionInfo fn = Scanner::DescribeFunction(address);
				if (!fn.valid)
				{
					outWhy = "no unwind entry covers this address -- it is not inside any compiled function";
					return false;
				}
				return true;
			}

			case Kind::Code:
			{
				const Scanner::SectionInfo s = Scanner::GetSectionForAddress(module, address);
				if (!s.valid)
				{
					outWhy = "the address is not inside any section of the module";
					return false;
				}
				if (!s.executable)
				{
					outWhy = std::string("the address is in '") + s.name + "', which is not executable";
					return false;
				}
				return true;
			}

			case Kind::Data:
			{
				const Scanner::SectionInfo s = Scanner::GetSectionForAddress(module, address);
				if (!s.valid)
				{
					outWhy = "the address is not inside any section of the module";
					return false;
				}
				if (s.executable)
				{
					outWhy = std::string("the address is in '") + s.name + "', which is executable -- "
					         "declare kind 'code' if that is what you meant";
					return false;
				}
				if (!s.initialized)
				{
					outWhy = std::string("the address is in '") + s.name + "', which holds no initialised data";
					return false;
				}
				return true;
			}

			case Kind::VTable:
			{
				const Scanner::SectionInfo s = Scanner::GetSectionForAddress(module, address);
				if (!s.valid || s.executable)
				{
					outWhy = "a vtable must live in a non-executable data section; this address is in "
					       + std::string(s.valid ? s.name : "<no section>");
					return false;
				}

				const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
				const uint32_t slots = req.vtableSlots ? req.vtableSlots : 1;
				for (uint32_t i = 0; i < slots; ++i)
				{
					uintptr_t slot = 0;
					if (!ReadPointer(image, address + i * sizeof(uintptr_t), slot))
					{
						outWhy = "slot " + std::to_string(i) + " runs past the end of the image";
						return false;
					}
					if (!Scanner::IsFunctionStart(slot))
					{
						outWhy = "slot " + std::to_string(i) + " holds " + Hex(slot)
						       + ", which is not the start of a function";
						return false;
					}
				}
				return true;
			}

			case Kind::Unspecified:
			default:
				outWhy = "no scan kind was declared";
				return false;
			}
		}

		// Builds the match list for a failed resolve. Runs a second, full pass --
		// affordable precisely because it only happens when the launch is already
		// going to refuse something.
		std::string EnumerateMatchDetail(HMODULE module, const std::string& pattern,
			const PatchOverlay::Snapshot& overlay, size_t& outTotal)
		{
			std::string out;
			outTotal = 0;

			const auto parsed = Scanner::ParsePattern(pattern);
			const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
			if (parsed.empty() || !image.valid)
				return out;

			std::vector<uintptr_t> matches;
			outTotal = overlay.Collect(image.base, image.size, parsed,
				kMaxReportedMatches, kMaxEnumeratedMatches, matches);

			for (size_t i = 0; i < matches.size(); ++i)
			{
				out += NL;
				out += "            #" + std::to_string(i + 1) + "  " + DescribeAddress(module, matches[i]);
			}

			if (outTotal > matches.size())
			{
				out += NL;
				out += "            ... and " + std::to_string(outTotal - matches.size()) + " more";
			}

			return out;
		}
	}

	Result Resolve(const Request& request)
	{
		Result result;

		if (request.name.empty() || request.pattern.empty())
		{
			result.outcome = Outcome::BadRequest;
			result.detail  = "resolve called with an empty name or pattern";
			return result;
		}

		if (request.kind == Kind::Unspecified)
		{
			result.outcome = Outcome::BadRequest;
			result.detail  = "no scan kind declared for this pattern -- say what the address is "
			                 "(function start, code, data, vtable), or PLUGIN_SCAN_ANY to opt out "
			                 "of the structural check";
			return result;
		}

		HMODULE module = ResolveModule(request.module);
		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		if (!image.valid)
		{
			result.outcome = Outcome::BadRequest;
			result.detail  = "the target module has no valid PE headers";
			return result;
		}

		const auto parsed = Scanner::ParsePattern(request.pattern);
		if (parsed.empty())
		{
			result.outcome = Outcome::BadRequest;
			result.detail  = "the pattern is empty or contains a token that is neither a hex byte "
			                 "nor a wildcard: " + request.pattern;
			return result;
		}

		bool allWildcards = true;
		for (const auto& b : parsed)
			if (!b.wildcard) { allWildcards = false; break; }
		if (allWildcards)
		{
			result.outcome = Outcome::BadRequest;
			result.detail  = "the pattern is nothing but wildcards, which matches at every offset";
			return result;
		}

		// --- Find it, and find out whether it is alone ------------------------
		//
		// Cache fast path applies only to the main module: scan_cache.ini is
		// keyed on the game version and stores offsets from the main module
		// base. A cached count of exactly 1 is the only count that can be
		// trusted here -- see scan_cache.h.
		const bool     isMainModule = (module == GetModuleHandleW(nullptr));
		const std::wstring gameVersion = isMainModule ? GetGameVersionString() : std::wstring();

		// Scan the image as it SHIPPED, not as we have left it. By the time a
		// plugin resolves, the loader has stamped a 14-byte JMP over the entry of
		// ProcessEvent, BeginPlay, Tick and two dozen more -- which is exactly the
		// set a mod wants. Without this, a pattern anchored on one of those stops
		// matching (or worse, matches somewhere else and passes the uniqueness
		// check), and a pattern containing FF 25 00 00 00 00 starts matching our
		// own stubs. See memory_scanner/patch_overlay.h.
		//
		// Free when nothing is hooked, which is the case for the loader's own
		// preflight -- the expensive 60-pattern pass runs before any of them.
		const PatchOverlay::Snapshot overlay;

		uintptr_t rawMatch = 0;
		size_t    count    = 0;
		bool      fromCache = false;

		if (!gameVersion.empty())
		{
			ScanCache::Entry cached;
			if (ScanCache::TryGet(gameVersion, request.pattern, cached) && cached.matchCount == 1)
			{
				const uintptr_t candidate = image.base + cached.offset;
				if (cached.offset + parsed.size() <= image.size &&
					overlay.MatchesAt(candidate, parsed))
				{
					rawMatch  = candidate;
					count     = 1;
					fromCache = true;
				}
			}
		}

		if (!fromCache)
		{
			if (overlay.Empty())
			{
				// Nothing is hooked, so the live image is the shipped image and
				// the scanner's own early exit applies: stop at the second match,
				// because "exactly one or not" is the only question the verdict
				// needs.
				count = Scanner::CountMatches(image.base, image.size, parsed, 2, &rawMatch);
			}
			else
			{
				// With patches in play a match has to be classified before it can
				// be counted, so there is no early exit to take. The full list is
				// bounded instead.
				std::vector<uintptr_t> matches;
				count = overlay.Collect(image.base, image.size, parsed, 1, kMaxEnumeratedMatches, matches);
				rawMatch = matches.empty() ? 0 : matches[0];
			}
		}

		if (count == 0)
		{
			result.outcome    = Outcome::NoMatch;
			result.matchCount = 0;
			result.detail     = "pattern not found in " + ModuleBaseName(module) + ": " + request.pattern;
			return result;
		}

		if (count > 1)
		{
			// Only now is the full enumeration worth paying for: the resolve has
			// already failed, so the only thing left to do with this launch is
			// tell whoever has to fix the pattern exactly where else it matched.
			size_t total = 0;
			const std::string matchList = EnumerateMatchDetail(module, request.pattern, overlay, total);

			result.outcome    = Outcome::MultipleMatches;
			result.rawMatch   = rawMatch;
			result.matchCount = total ? total : count;
			result.detail     = "pattern is not unique -- it matched " + std::to_string(result.matchCount)
			                  + " times and an AOB must resolve to exactly one address."
			                  + NL + "            Pattern: " + request.pattern;
			result.detail    += matchList;
			return result;
		}

		// Exactly one match. Worth caching that fact before anything else can
		// fail: the kind check is about what the author declared, not about the
		// image, so a WrongKind result does not make the count wrong.
		if (!gameVersion.empty() && !fromCache)
		{
			ScanCache::Entry entry;
			entry.offset     = rawMatch - image.base;
			entry.matchCount = 1;
			ScanCache::Store(gameVersion, request.pattern, entry);
		}

		result.rawMatch   = rawMatch;
		result.matchCount = 1;

		// --- Transform, then check what it landed on --------------------------
		uintptr_t finalAddress = rawMatch;
		std::string transformError;
		if (!ApplyTransforms(request, module, rawMatch, finalAddress, transformError))
		{
			result.outcome = Outcome::WrongKind;
			result.detail  = "matched once at " + DescribeAddress(module, rawMatch)
			               + std::string(NL) + "            but " + transformError;
			return result;
		}

		std::string why;
		if (!CheckKind(request, module, finalAddress, why))
		{
			result.outcome = Outcome::WrongKind;
			result.address = finalAddress;
			result.detail  = std::string("declared as '") + KindName(request.kind) + "', but it is not."
			               + NL + "            Resolved to: " + DescribeAddress(module, finalAddress)
			               + NL + "            Reason: " + why
			               + NL + "            Pattern: " + request.pattern;
			return result;
		}

		result.outcome = Outcome::Ok;
		result.address = finalAddress;

		ModLoaderLogger::LogDebug(L"[ScanValidation] %S -> 0x%llX (%S, %S)",
			request.name.c_str(),
			static_cast<unsigned long long>(finalAddress),
			KindName(request.kind),
			fromCache ? "cached" : "scanned");

		return result;
	}
}
