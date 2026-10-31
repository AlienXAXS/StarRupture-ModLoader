#pragma once
// sr-aobscan --suggest: proposes a replacement for a pattern that no longer
// resolves (MISSING or AMBIGUOUS) on the scanned build. See suggest.cpp.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "memory_scanner/scan_validation.h"

namespace AobScan::Suggest
{
	// The failed request, as the loader or the plugin made it.
	struct Request
	{
		std::string              name;          // hook name, e.g. "UCrBuildingComponent::GetPlacementResourceConditionResult"
		std::vector<std::string> patterns;      // one, or a candidate list
		ScanValidation::Kind     kind = ScanValidation::Kind::Unspecified;
		int32_t                  resultOffset    = 0;
		int32_t                  followRel32At   = -1;
		int32_t                  followRipDispAt = -1;
	};

	struct Proposal
	{
		std::string pattern;
		int32_t     followRel32At     = -1;  // >= 0: the E8/E9 to follow, as in the request
		bool        uniqueOnTarget    = false;
		int         uniqueOnReference = -1;  // 1 yes, 0 no, -1 not known (no reference function)
	};

	struct Result
	{
		bool                  attempted  = false;
		const char*           located    = "none";  // reference, name, renamed, bytes, none
		const char*           confidence = "";      // high, medium, low
		bool                  hasRva          = false;
		uint64_t              rva             = 0;    // the function on the scanned build
		bool                  hasReferenceRva = false;
		uint64_t              referenceRva    = 0;    // the same function on the reference build
		int                   hookNameMatches = -1;   // 1/0: the symbol there is the hook's name; -1 unknown
		bool                  referenceKeepsCurrent = false;  // the current pattern still resolves on the reference
		std::vector<Proposal> proposals;
		std::string           note;
	};

	// One per process. The reference image and both builds' symbols are loaded
	// on the first Propose, so a run with nothing to repair costs nothing.
	class Session
	{
	public:
		Session(HMODULE target, const wchar_t* targetPath, const wchar_t* referencePath);
		~Session();
		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		Result Propose(const Request& request);

		// Top-level JSON object describing what the proposals could use.
		std::string EnvJson() const;

		struct State;  // suggest.cpp

	private:
		std::unique_ptr<State> state_;
	};

	void        PrintText(FILE* out, const Result& result);
	std::string Json(const Result& result);
}
