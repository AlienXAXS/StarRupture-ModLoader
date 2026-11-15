#include "pch.h"
#include "UI/image_pump.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "UI/global_settings.h"
#include "UI/image_lookup_host.h"
#include "UI/imgui_backend.h"
#include "hooks/game/object_lookup/object_lookup.h"
#include "hooks/game/scan_patterns.h"
#include "logging/log.h"
#include "memory_scanner/scanner.h"
#include "utils/engine_call_utils.h"
#include "Engine_classes.hpp"

#include <atomic>

namespace ImagePump
{
	namespace
	{
		using FindObject_t = SDK::UObject* (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using LoadObject_t = SDK::UObject* (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, const wchar_t*,
		                                                 uint32_t, void*, bool, const void*);

		bool FindSEH(FindObject_t fn, SDK::UClass* cls, const wchar_t* path, SDK::UObject** out, DWORD* code)
		{
			__try
			{
				*out = fn(cls, nullptr, path, false);
				return true;
			}
			__except (EngineCall::SehFilter(GetExceptionCode(), code))
			{
				return false;
			}
		}

		bool LoadSEH(LoadObject_t fn, SDK::UClass* cls, const wchar_t* path, SDK::UObject** out, DWORD* code)
		{
			__try
			{
				*out = fn(cls, nullptr, path, nullptr, 0, nullptr, false, nullptr);
				return true;
			}
			__except (EngineCall::SehFilter(GetExceptionCode(), code))
			{
				return false;
			}
		}

		bool CopySEH(void* texture, const char* name, unsigned minSize, ImGuiHostCopyOut* out, int* result, DWORD* code)
		{
			__try
			{
				*result = UI::ImGuiBackend::TryCopyTextureLevel(texture, name, minSize, out);
				return true;
			}
			__except (EngineCall::SehFilter(GetExceptionCode(), code))
			{
				return false;
			}
		}

		class EngineHost final : public Host
		{
		public:
			bool GpuReady() override { return UI::ImGuiBackend::GpuReady(); }

			uint64_t LocalVideoBudget() override { return UI::ImGuiBackend::LocalVideoBudget(); }

			bool PreloadEnabled() override { return UI::GlobalSettings::GetPreloadImages(); }

			bool VideoMemory(uint64_t& usage, uint64_t& budget) override
			{
				unsigned long long used = 0, total = 0;
				if (!UI::ImGuiBackend::LocalVideoMemory(&used, &total))
					return false;
				usage  = used;
				budget = total;
				return true;
			}

			uint64_t NowUs() override
			{
				static const LONGLONG frequency = []{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				// Whole seconds and the remainder apart: the product with 1e6 overflows after days of uptime.
				const uint64_t counts = static_cast<uint64_t>(now.QuadPart);
				const uint64_t perSecond = static_cast<uint64_t>(frequency);
				return counts / perSecond * 1000000 + counts % perSecond * 1000000 / perSecond;
			}

			ReadResult Read(const std::string& objectPath, Copy& out) override
			{
				// After a caught fault the engine may be left holding a lock: it is not called again.
				if (ImageLookup::EngineFaulted())
					return ReadResult::Fault;

				const FindObject_t find = reinterpret_cast<FindObject_t>(Hooks::ObjectLookup::GetStaticFindObject_ByNameAddress());
				SDK::UClass* const textureClass = SDK::UTexture2D::StaticClass();
				if (!find || !textureClass)
					return ReadResult::Unavailable;

				const std::wstring path = EngineCall::Widen(objectPath);
				if (path.empty())
					return ReadResult::Failed;

				// The texture pointer lives in this call and no longer.
				SDK::UObject* texture = nullptr;
				DWORD code = 0;
				if (!FindSEH(find, textureClass, path.c_str(), &texture, &code))
				{
					LogToFile::Warn("[Images] Exception 0x%08lX finding %s", code, objectPath.c_str());
					ImageLookup::NoteEngineFault();
					return ReadResult::Fault;
				}
				if (!texture)
					return ReadResult::NotLoaded;

				ImGuiHostCopyOut copy = {};
				int result = ImGuiHostCopy_Unavailable;
				if (!CopySEH(texture, nullptr, ImageLibrary::kStoredSize, &copy, &result, &code))
				{
					LogToFile::Warn("[Images] Exception 0x%08lX reading %s", code, objectPath.c_str());
					ImageLookup::NoteEngineFault();
					return ReadResult::Fault;
				}

				switch (result)
				{
				case ImGuiHostCopy_Started:
					out.texture = copy.texture;
					out.width   = copy.width;
					out.height  = copy.height;
					out.sourceWidth  = copy.sourceWidth;
					out.sourceHeight = copy.sourceHeight;
					out.sourceFormat = copy.sourceFormat;
					return ReadResult::Started;
				case ImGuiHostCopy_NotYet:      return ReadResult::NotYet;
				case ImGuiHostCopy_Busy:        return ReadResult::Busy;
				case ImGuiHostCopy_Unavailable: return ReadResult::Unavailable;
				default:                        return ReadResult::Failed;
				}
			}

			// Synchronous: the game thread waits while the package loads. This is
			// the one function to replace to load in the background.
			LoadResult Load(const std::string& objectPath) override
			{
				if (ImageLookup::EngineFaulted())
					return LoadResult::Fault;

				static const LoadObject_t load = reinterpret_cast<LoadObject_t>(
					Scanner::FindPatternInMainModule("StaticLoadObject", ScanPatterns::StaticLoadObject));
				static bool s_loggedMissing = false;
				if (!load && !s_loggedMissing)
				{
					s_loggedMissing = true;
					LogToFile::Warn("[Images] StaticLoadObject was not found: images whose texture is not in memory cannot be read here");
				}
				SDK::UClass* const textureClass = SDK::UTexture2D::StaticClass();
				if (!load || !textureClass)
					return LoadResult::Unsupported;

				const std::wstring path = EngineCall::Widen(objectPath);
				if (path.empty())
					return LoadResult::Missing;

				SDK::UObject* texture = nullptr;
				DWORD code = 0;
				if (!LoadSEH(load, textureClass, path.c_str(), &texture, &code))
				{
					LogToFile::Warn("[Images] Exception 0x%08lX loading %s", code, objectPath.c_str());
					ImageLookup::NoteEngineFault();
					return LoadResult::Fault;
				}
				return texture ? LoadResult::Loaded : LoadResult::Missing;
			}

			CopyState StateOf(PluginTextureHandle texture) override
			{
				switch (UI::ImGuiBackend::CopyState(texture))
				{
				case 1:  return CopyState::Done;
				case 0:  return CopyState::Pending;
				default: return CopyState::Gone;
				}
			}

			void PrepareShrink() override { UI::ImGuiBackend::PrepareShrink(ImageLibrary::kStoredSize); }

			ShrinkResult Shrink(PluginTextureHandle source, Shrunk& out) override
			{
				ImGuiHostShrinkOut shrunk = {};
				switch (UI::ImGuiBackend::ShrinkTexture(source, ImageLibrary::kStoredSize, &shrunk))
				{
				case ImGuiHostShrink_Started:
					out.texture = shrunk.texture;
					out.width   = shrunk.width;
					out.height  = shrunk.height;
					out.levels  = shrunk.levels;
					out.readback = shrunk.readback;
					return ShrinkResult::Started;
				case ImGuiHostShrink_Busy: return ShrinkResult::Busy;
				default:                   return ShrinkResult::Failed;
				}
			}

			void Free(PluginTextureHandle texture) override
			{
				IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
				if (api && api->FreeTexture)
					api->FreeTexture(texture);
			}

			void Log(const std::string& line) override
			{
				LogToFile::Info("%s", line.c_str());
			}
		};

		// Set by the first tick, so a shutdown before it builds nothing.
		std::atomic<Pump*> s_pump{ nullptr };

		// Never destroyed: the pump can still be asked at process exit.
		Pump& TheInstance()
		{
			static EngineHost* const host = new EngineHost();
			static Pump* const pump = []
			{
				Pump* const made = new Pump(ImageLibrary::Instance(), *host, ImageLookup::EngineItems(), ImageLookup::EngineBuildings());
				made->SetSaver(ImageDisk::EngineSaver());
				made->SetSource(ImageDisk::EngineSource());
				return made;
			}();
			return *pump;
		}
	}

	void Tick()
	{
		Pump& pump = TheInstance();
		s_pump.store(&pump, std::memory_order_release);
		pump.Tick();
	}

	void Shutdown()
	{
		// The engine can exit before the first tick: there is nothing to shut down then.
		Pump* const pump = s_pump.load(std::memory_order_acquire);
		if (pump)
			pump->Shutdown();
	}
}

#endif // MODLOADER_CLIENT_BUILD
