#include "pch.h"
#include "text_input_focus.h"

// Client-only feature -- entire translation unit is a no-op on server/generic builds.
#ifdef MODLOADER_CLIENT_BUILD

#include "hooks/game/gobject_walk/gobject_walk.h"
#include "logging/logger.h"
#include "UMG_classes.hpp"

#include <windows.h>

namespace Hooks::TextInputFocus
{
	static bool s_suppressEnabled = true;

	void LoadConfig(const wchar_t* iniPath)
	{
		int val = GetPrivateProfileIntW(L"Input", L"SuppressKeybindsWhileTyping", -1, iniPath);
		if (val == -1)
		{
			WritePrivateProfileStringW(L"Input", L"SuppressKeybindsWhileTyping", L"1", iniPath);
			val = 1;
		}
		s_suppressEnabled = (val != 0);
		ModLoaderLogger::LogInfo(L"[TextInputFocus] SuppressKeybindsWhileTyping=%d", val);
	}

	// -----------------------------------------------------------------------
	// SEH-protected scan.
	//
	// Deliberately does NOT reuse GObjectWalk::FindObjectsByClassNameInto --
	// that builds a std::string class name for every live object per walk,
	// which is far too slow to run per keypress.  Comparing UClass pointers
	// keeps the pass sub-millisecond.
	//
	// Same residual risk as the GObjectWalk plugin API: an object mid-GC can
	// slip past the flag filter (EInternalObjectFlags is not in this SDK
	// dump), so the walk and the ProcessEvent focus query are wrapped in SEH.
	// POD locals only in this function -- SEH forbids unwindable objects.
	// -----------------------------------------------------------------------
	static bool ScanForFocusedTextWidget(SDK::UClass* c0, SDK::UClass* c1,
	                                     SDK::UClass* c2, SDK::UClass* c3)
	{
		__try
		{
			const int32_t num = SDK::UObject::GObjects->Num();
			for (int32_t i = 0; i < num; ++i)
			{
				SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
				if (!obj || !obj->Class)
					continue;
				if (obj->Flags & SDK::EObjectFlags::BeginDestroyed)
					continue;
				if (obj->Flags & SDK::EObjectFlags::FinishDestroyed)
					continue;
				if (obj->Flags & SDK::EObjectFlags::ClassDefaultObject)
					continue;
				if (obj->Flags & SDK::EObjectFlags::ArchetypeObject)
					continue;

				SDK::UClass* cls = obj->Class;
				if (cls != c0 && cls != c1 && cls != c2 && cls != c3)
					continue;

				if (static_cast<SDK::UWidget*>(obj)->HasKeyboardFocus())
					return true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		return false;
	}

	bool IsGameTextInputFocused()
	{
		if (!s_suppressEnabled)
			return false;
		if (!Hooks::GObjectWalk::IsReady())
			return false;

		// StaticClass() caches its lookup in a function-local static, so
		// calling these every time is cheap.  A null result (class not found)
		// can never match obj->Class because null-Class objects are skipped.
		SDK::UClass* c0 = SDK::UEditableText::StaticClass();
		SDK::UClass* c1 = SDK::UEditableTextBox::StaticClass();
		SDK::UClass* c2 = SDK::UMultiLineEditableText::StaticClass();
		SDK::UClass* c3 = SDK::UMultiLineEditableTextBox::StaticClass();

		return ScanForFocusedTextWidget(c0, c1, c2, c3);
	}

} // namespace Hooks::TextInputFocus

#endif // MODLOADER_CLIENT_BUILD
