#include "LogisticsFragmentFixer.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"
#include <Windows.h>

namespace RailJunctionFixer
{
	uintptr_t* LogisticsFragmentFixer::s_newChain      = nullptr;
	uintptr_t  LogisticsFragmentFixer::s_socketsStruct  = 0;
	uintptr_t* LogisticsFragmentFixer::s_origChain      = nullptr;
	int32_t    LogisticsFragmentFixer::s_origDepth      = 0;
	uintptr_t  LogisticsFragmentFixer::s_origSuperStruct = 0;

	bool LogisticsFragmentFixer::PatchHierarchyChain(uintptr_t socketsStruct, uintptr_t savableStruct)
	{
		// Read current chain info for sockets fragment
		int32_t sockDepth = ReadAt<int32_t>(socketsStruct, UStructOff::HierarchyDepth);
		uintptr_t* sockChain = ReadAt<uintptr_t*>(socketsStruct, UStructOff::InheritanceChain);

		// Read savable fragment's info
		int32_t savDepth = ReadAt<int32_t>(savableStruct, UStructOff::HierarchyDepth);
		uintptr_t savIdentity = savableStruct + UStructOff::InheritanceChain;
		uintptr_t sockIdentity = socketsStruct + UStructOff::InheritanceChain;

		LOG_DEBUG("PatchHierarchyChain:");
		LOG_DEBUG("  sockets depth=%d, chain=0x%llX, identity=0x%llX",
			sockDepth, (unsigned long long)(uintptr_t)sockChain,
			(unsigned long long)sockIdentity);
		LOG_DEBUG("  savable depth=%d, identity=0x%llX",
			savDepth, (unsigned long long)savIdentity);

		if (!sockChain || sockDepth < 0 || sockDepth > 30)
		{
			LOG_ERROR("Invalid sockets hierarchy data");
			return false;
		}

		// Verify the chain's self-entry is correct
		if (sockChain[sockDepth] != sockIdentity)
		{
			LOG_ERROR("chain[self_depth] (0x%llX) != self identity (0x%llX)",
				(unsigned long long)sockChain[sockDepth],
				(unsigned long long)sockIdentity);
			return false;
		}

		// Check if savable is already in the chain (patch already applied)
		if (savDepth <= sockDepth && sockChain[savDepth] == savIdentity)
		{
			LOG_INFO("Hierarchy chain already contains CrMassSavableFragment");
			return true;
		}

		// Build new chain: insert savable at index savDepth, shift rest up by 1
		// Old chain: [root, ..., FMassFragment, socketsFragment]  (sockDepth+1 entries)
		// New chain: [root, ..., FMassFragment, savableFragment, socketsFragment]  (sockDepth+2 entries)
		int newSize = sockDepth + 2;
		LOG_DEBUG("Building new chain: %d -> %d entries", sockDepth + 1, newSize);

		// Allocate permanent memory for the new chain
		s_newChain = (uintptr_t*)VirtualAlloc(
			nullptr, newSize * sizeof(uintptr_t),
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!s_newChain)
		{
			LOG_ERROR("VirtualAlloc failed for new chain");
			return false;
		}

		// Copy entries before the insertion point
		for (int i = 0; i < savDepth; ++i)
		{
			s_newChain[i] = sockChain[i];
		}

		// Insert savable fragment's identity at its depth
		s_newChain[savDepth] = savIdentity;

		// Copy remaining entries (shifted by 1), including self
		for (int i = savDepth; i <= sockDepth; ++i)
		{
			s_newChain[i + 1] = sockChain[i];
		}

		// Log the new chain
		for (int i = 0; i < newSize; ++i)
		{
			LOG_DEBUG("  newChain[%d] = 0x%llX%s%s",
				i, (unsigned long long)s_newChain[i],
				(s_newChain[i] == savIdentity) ? " [SAVABLE]" : "",
				(s_newChain[i] == sockIdentity) ? " [SELF]" : "");
		}

		// Save originals for restoration on shutdown
		s_socketsStruct   = socketsStruct;
		s_origChain       = sockChain;
		s_origDepth       = sockDepth;
		s_origSuperStruct = ReadAt<uintptr_t>(socketsStruct, UStructOff::SuperStruct);

		// Apply the patch: update chain pointer, depth, and SuperStruct
		uintptr_t patchStart = socketsStruct + UStructOff::InheritanceChain;
		// Unprotect the entire region from +0x30 to +0x48 (InheritanceChain + HierarchyDepth + SuperStruct)
		DWORD oldProtect;
		if (!VirtualProtect((void*)patchStart, 0x18, PAGE_READWRITE, &oldProtect))
		{
			LOG_ERROR("VirtualProtect failed to unprotect memory");
			return false;
		}

		WriteAt<uintptr_t*>(socketsStruct, UStructOff::InheritanceChain, s_newChain);
		WriteAt<int32_t>(socketsStruct, UStructOff::HierarchyDepth, sockDepth + 1);
		WriteAt<uintptr_t>(socketsStruct, UStructOff::SuperStruct, savableStruct);

		VirtualProtect((void*)patchStart, 0x18, oldProtect, &oldProtect);

		// Verify
		uintptr_t* verifyChain = ReadAt<uintptr_t*>(socketsStruct, UStructOff::InheritanceChain);
		int32_t verifyDepth = ReadAt<int32_t>(socketsStruct, UStructOff::HierarchyDepth);
		uintptr_t verifySuper = ReadAt<uintptr_t>(socketsStruct, UStructOff::SuperStruct);

		bool ok = (verifyChain == s_newChain) &&
			(verifyDepth == sockDepth + 1) &&
			(verifySuper == savableStruct);

		if (ok)
		{
			// Test the IsChildOf logic ourselves
			uintptr_t testEntry = s_newChain[savDepth];
			bool isChildOf = (savDepth <= verifyDepth) && (testEntry == savIdentity);
			LOG_INFO("  IsChildOf(CrMassSavableFragment) = %s", isChildOf ? "TRUE" : "FALSE");
		}
		else
		{
			LOG_ERROR("Verification failed after patching hierarchy chain");
		}

		return ok;
	}

	bool LogisticsFragmentFixer::Initialize()
	{
		LOG_INFO("Initializing LogisticsFragmentFixer...");

		try
		{
			// Find both UScriptStruct objects via Unreal reflection
			auto* socketsStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
				"CrLogisticsSocketsFragment", SDK::EClassCastFlags::ScriptStruct);

			auto* savableStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
				"CrMassSavableFragment", SDK::EClassCastFlags::ScriptStruct);

			if (!socketsStruct)
			{
				LOG_ERROR("Could not find UScriptStruct for CrLogisticsSocketsFragment");
				return false;
			}

			if (!savableStruct)
			{
				LOG_ERROR("Could not find UScriptStruct for CrMassSavableFragment");
				return false;
			}

			// Log current state
			auto* currentSuper = socketsStruct->SuperStruct;
			LOG_INFO("CrLogisticsSocketsFragment at 0x%llX", reinterpret_cast<uintptr_t>(socketsStruct));
			LOG_INFO("Current SuperStruct at 0x%llX (%s)",
				reinterpret_cast<uintptr_t>(currentSuper),
				currentSuper ? currentSuper->GetName().c_str() : "nullptr");
			LOG_INFO("CrMassSavableFragment at 0x%llX", reinterpret_cast<uintptr_t>(savableStruct));

			// Verify we're patching what we expect (SuperStruct should be FMassFragment)
			if (currentSuper && currentSuper == savableStruct)
			{
				LOG_INFO("Already reparented - nothing to do");
				return true;
			}

			// Patch the hierarchy chain to fix IsChildOf checks
			bool success = PatchHierarchyChain(
				reinterpret_cast<uintptr_t>(socketsStruct),
				reinterpret_cast<uintptr_t>(savableStruct));

			if (success)
			{
				LOG_INFO("SUCCESS - CrLogisticsSocketsFragment now inherits from CrMassSavableFragment");
				LOG_INFO("         IsChildOf checks will now work correctly for save system");
				return true;
			}
			else
			{
				LOG_ERROR("Failed to patch hierarchy chain");
				return false;
			}
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Exception during initialization: %s", e.what());
			return false;
		}
	}

	void LogisticsFragmentFixer::Shutdown()
	{
		LOG_INFO("Shutting down LogisticsFragmentFixer...");

		// Restore the original UScriptStruct data so the engine doesn't encounter
		// our VirtualAlloc'd chain pointer during its own teardown, which would
		// cause MallocBinned2 corruption when it tries to free non-UE memory.
		if (s_socketsStruct != 0 && s_origChain != nullptr)
		{
			DWORD oldProtect;
			uintptr_t patchStart = s_socketsStruct + UStructOff::InheritanceChain;
			if (VirtualProtect((void*)patchStart, 0x18, PAGE_READWRITE, &oldProtect))
			{
				WriteAt<uintptr_t*>(s_socketsStruct, UStructOff::InheritanceChain, s_origChain);
				WriteAt<int32_t>   (s_socketsStruct, UStructOff::HierarchyDepth,   s_origDepth);
				WriteAt<uintptr_t> (s_socketsStruct, UStructOff::SuperStruct,      s_origSuperStruct);
				VirtualProtect((void*)patchStart, 0x18, oldProtect, &oldProtect);
				LOG_INFO("Original UScriptStruct hierarchy restored");
			}
			else
			{
				LOG_ERROR("VirtualProtect failed during restore - original hierarchy NOT restored");
			}

			s_socketsStruct   = 0;
			s_origChain       = nullptr;
			s_origDepth       = 0;
			s_origSuperStruct = 0;
		}

		if (s_newChain)
		{
			VirtualFree(s_newChain, 0, MEM_RELEASE);
			s_newChain = nullptr;
		}
	}
}
