#include "pch.h"
#include "function_symbols.h"
#include "hooks/game/engine_init/engine_init.h"
#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include <unordered_map>

namespace Hooks::EngineTick::FunctionSymbols
{
	static std::unordered_map<uintptr_t, std::string> s_map;
	static bool s_built = false;

	static void BuildMap()
	{
		s_map.clear();

		// Same cost class as GObjectWalk::ExportKnownClassesAndFunctions --
		// a full GObjects pass. Runs once, lazily, the first time a stutter
		// with stack sampling on needs a name resolved.
		const int32_t num = SDK::UObject::GObjects->Num();
		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (!obj || !obj->Class)
				continue;
			if (!obj->IsA(SDK::EClassCastFlags::Function))
				continue;

			auto* fn = static_cast<SDK::UFunction*>(obj);
			if (!(fn->FunctionFlags & static_cast<uint32_t>(SDK::EFunctionFlags::Native)))
				continue; // BlueprintImplementable/script functions have no native entry point
			if (!fn->ExecFunction)
				continue;

			s_map[reinterpret_cast<uintptr_t>(fn->ExecFunction)] = obj->Class->GetName() + "::" + fn->GetName();
		}

		s_built = true;
	}

	std::string Resolve(uintptr_t functionStartAddress)
	{
		if (!functionStartAddress || !Hooks::EngineInit::IsEngineInitialized())
			return {};

		if (!s_built)
			BuildMap();

		auto it = s_map.find(functionStartAddress);
		return it != s_map.end() ? it->second : std::string();
	}
}
