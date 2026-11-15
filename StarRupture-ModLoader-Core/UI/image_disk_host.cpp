#include "pch.h"
#include "UI/image_disk.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "UI/image_cache_care.h"
#include "UI/image_cache_reader.h"
#include "UI/imgui_backend.h"
#include "core/startup_utils.h"
#include "core/version_check.h"
#include "logging/log.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_cache.h"
#include "plugins/pak_registry.h"

#include <atomic>
#include <mutex>

namespace ImageDisk
{
	namespace
	{
		// Where every game build's folder goes, under the loader's own data folder. Worker only.
		std::wstring CacheRoot()
		{
			return GetModLoaderDirPath(L"image-cache\\");
		}

		void LogLine(const std::string& line)
		{
			LogToFile::Info("%s", line.c_str());
		}

		// The folder the files of this game build go in:
		// image-cache\<build, pak set, revision, stored size>. Made by the worker.
		std::wstring FolderFor(uint64_t pakHash)
		{
			// Reads the exe's version resource, so it runs on the worker only.
			static std::wstring section;
			static std::once_flag once;
			std::call_once(once, []
			{
				const Scanner::ImageInfo image = Scanner::GetImageInfo(GetModuleHandleW(nullptr));
				section = ScanCache::SectionName(GetGameVersionString(), image.stamp, static_cast<uint32_t>(image.size));
			});

			ImageCache::FolderKey key;
			key.section    = section;
			key.pakHash    = pakHash;
			key.storedSize = ImageLibrary::kStoredSize;
			return CacheRoot() + ImageCache::FolderName(key);
		}

		// The mounted paks as the loader lists them: memory only, no file access.
		bool ListMountedPaks(std::vector<ImageCache::PakEntry>& paks)
		{
			IPluginPak* const pak = PakRegistry::GetInterface();
			if (!pak || !pak->IsAvailable())
				return false;

			// One call with room to spare (a buffer kept between calls: this runs in every tick that files
			// an image); a second only when the list is longer than that.
			static std::vector<PluginPakInfo> infos(64);
			int total = pak->GetMountedInto(nullptr, infos.data(), static_cast<int>(infos.size()));
			if (total > static_cast<int>(infos.size()))
			{
				infos.resize(static_cast<size_t>(total) + 16);
				total = pak->GetMountedInto(nullptr, infos.data(), static_cast<int>(infos.size()));
			}
			if (total <= 0 || total > static_cast<int>(infos.size()))
				return false;

			for (int i = 0; i < total; ++i)
			{
				const PluginPakInfo& info = infos[static_cast<size_t>(i)];
				ImageCache::PakEntry entry;
				entry.path       = info.pakPath;
				entry.mountPoint = info.mountPoint;
				entry.order      = info.order;
				entry.numFiles   = info.numFiles;
				paks.push_back(std::move(entry));
			}
			return true;
		}

		// The size and last-write time of a file. Worker only.
		bool StampFile(const std::string& utf8Path, ImageCache::FileStamp& stamp)
		{
			const int size = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
			if (size <= 1)
				return false;
			std::wstring path(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, path.data(), size);
			path.resize(static_cast<size_t>(size) - 1);

			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				return false;
			stamp.size      = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			stamp.writeTime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
			return true;
		}

		// The folder of this session. Its key is the mounted pak set, taken once, when the disk part is
		// first used (the pump's first tick, which is after every plugin's PluginInit has mounted its
		// paks: the main thread is held until then); every file read, written or tidied goes by it. The
		// set is looked at again at most every 250 ms by the reader, and once in each tick that files an image. If it has
		// changed, the disk part goes off for the rest of the session: images read from the game after a
		// pak was mounted might differ from what the key stands for, and must never be saved under it.
		PakSession& TheSession()
		{
			static PakSession* const session = new PakSession(&ListMountedPaks, &StampFile, &FolderFor,
				[](const std::string& line) { LogToFile::Warn("%s", line.c_str()); });
			return *session;
		}

		std::atomic<uint32_t> g_cleared{ 0 };

		class EngineUploader final : public ImageCache::Uploader
		{
		public:
			bool Upload(uint32_t width, uint32_t height, uint32_t levels, const uint8_t* pixels, size_t bytes, PluginTextureHandle& texture) override
			{
				ImGuiHostUploadOut out = {};
				if (UI::ImGuiBackend::UploadTexture(width, height, levels, pixels, bytes, &out) != ImGuiHostUpload_Started)
					return false;
				texture = out.texture;
				return true;
			}
		};

		class EngineHost final : public Host
		{
		public:
			Readback Take(uint32_t ticket, uint8_t* dest, size_t bytes) override
			{
				switch (UI::ImGuiBackend::TakeReadback(ticket, dest, bytes))
				{
				case 1:  return Readback::Done;
				case 0:  return Readback::Pending;
				default: return Readback::Gone;
				}
			}

			// Looked at again every time it is called: once in each tick that files an image.
			std::function<std::wstring()> CacheFolder() override
			{
				return TheSession().Folder(GetTickCount64(), true);
			}

			uint32_t ClearCount() const override
			{
				return g_cleared.load();
			}
		};
	}

	namespace
	{
		// Never destroyed: the pump can still be asked at process exit.
		ImageCache::Writer& EngineWriter()
		{
			static ImageCache::Writer* const writer = new ImageCache::Writer([](const std::string& line) { LogToFile::Warn("%s", line.c_str()); });
			return *writer;
		}
	}

	Saver* EngineSaver()
	{
		static EngineHost* const host = new EngineHost();
		static DiskSaver* const saver = [&]
		{
			ImageCache::Writer& writer = EngineWriter();
			// The session's key is taken here if it has not been, so the tidy job below has a folder to keep.
			TheSession().Folder(GetTickCount64(), false);
			// Old folders, leftovers and the size cap are looked after once, as the first job. It asks the
			// session for the folder when it runs, and does nothing when the disk part is off by then.
			writer.Submit(ImageCache::Care::TidyJob(&CacheRoot,
				[&writer] { return writer.Off() ? std::wstring() : TheSession().CurrentFolder(); }, &LogLine));
			return new DiskSaver(ImageLibrary::Instance(), *host, writer);
		}();
		return saver;
	}

	Source* EngineSource()
	{
		static EngineUploader* const uploader = new EngineUploader();
		static ImageCache::Reader* const reader = new ImageCache::Reader(
			[] { return TheSession().Folder(GetTickCount64(), false); }, *uploader,
			[](const std::string& line) { LogToFile::Info("%s", line.c_str()); });
		return reader;
	}

	bool ClearCache()
	{
		if (EngineWriter().Submit(ImageCache::Care::ClearJob(&CacheRoot, &LogLine)))
		{
			++g_cleared;
			return true;
		}
		LogToFile::Info("[Images] Image cache not cleared: the job could not be queued");
		return false;
	}
}

#endif // MODLOADER_CLIENT_BUILD
