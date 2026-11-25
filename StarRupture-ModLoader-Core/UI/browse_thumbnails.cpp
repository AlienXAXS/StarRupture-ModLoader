#include "pch.h"
#include "browse_thumbnails.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui_backend.h"
#include "auto_update/mod_install.h"
#include "auto_update/thumbnail_fetcher.h"
#include "core/startup_utils.h"
#include "utils/game_thread_dispatch.h"

#include <set>
#include <string>

namespace UI::BrowseThumbnails
{
	// The decode and the texture upload run on the game thread (the same hop the
	// RELOAD button uses); freeing is safe from any thread.
	ModIndex::TextureOps HostTextureOps()
	{
		ModIndex::TextureOps ops;
		ops.load = [](const void* data, size_t size, const char* name) -> void*
		{
			IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
			return (api && api->LoadFromMemory) ? api->LoadFromMemory(data, size, name) : nullptr;
		};
		ops.free = [](void* handle)
		{
			IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
			if (api && api->FreeTexture)
				api->FreeTexture(handle);
		};
		ops.size = [](void* handle, int* width, int* height)
		{
			IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
			if (api && api->GetSize)
				api->GetSize(handle, width, height);
		};
		ops.post = [](std::function<void()> job) { GameThreadDispatch::PostVoid(std::move(job)); };
		return ops;
	}

	namespace
	{
		ModIndex::ThumbnailFetcherConfig FetcherConfig()
		{
			ModIndex::ThumbnailFetcherConfig config;
			config.cacheDir = GetModLoaderDirPath(L"thumbs");
			config.hop = ModInstall::HttpHop();
			config.paused = [] { return ModInstall::Instance().Busy(); };
			return config;
		}

		// Made on first use and kept to the end of the process; Shutdown ends it.
		struct Impl
		{
			ModIndex::ThumbnailFetcher fetcher{ FetcherConfig() };
			ModIndex::ThumbnailTextures textures{ HostTextureOps(), MakeHooks() };

			ModIndex::ThumbnailTexturesHooks MakeHooks()
			{
				ModIndex::ThumbnailTexturesHooks hooks;
				hooks.take = [this](const std::string& id) { return fetcher.TakeBytes(id); };
				hooks.released = [this](const std::string& id) { fetcher.Forget(id); };
				return hooks;
			}
		};

		Impl& Instance()
		{
			static Impl* const impl = new Impl();
			return *impl;
		}

		// Render thread only.
		std::shared_ptr<const ModIndex::Snapshot> s_synced;
	}

	void Sync(const std::shared_ptr<const ModIndex::Snapshot>& snap)
	{
		if (snap == s_synced)
			return;
		s_synced = snap;
		std::set<std::string> hashes;
		if (snap)
			for (const ModIndex::Entry& entry : snap->entries)
				if (!entry.thumbnail.url.empty())
					hashes.insert(entry.thumbnail.sha256);
		Instance().fetcher.SetList(hashes, snap && snap->signatureVerified);
	}

	Thumb Get(const ModIndex::Entry& entry)
	{
		Thumb thumb;
		if (entry.thumbnail.url.empty())
			return thumb;

		Impl& impl = Instance();
		impl.fetcher.Want(entry.id, entry.thumbnail);
		switch (impl.fetcher.State(entry.id))
		{
		case ModIndex::ThumbnailState::None:
			return thumb;
		case ModIndex::ThumbnailState::Failed:
			thumb.view = View::Failed;
			return thumb;
		case ModIndex::ThumbnailState::Mismatch:
			thumb.view = View::Mismatch;
			return thumb;
		case ModIndex::ThumbnailState::Bytes:
			impl.textures.Request(entry.id);
			break;
		case ModIndex::ThumbnailState::Loading:
		case ModIndex::ThumbnailState::Handed:
			break;
		}

		thumb.view = View::Loading;
		const ModIndex::Picture pic = impl.textures.Get(entry.id);
		if (pic.state == ModIndex::PictureState::Ready)
		{
			thumb.view = View::Ready;
			thumb.handle = pic.handle;
			thumb.width = pic.width;
			thumb.height = pic.height;
		}
		else if (pic.state == ModIndex::PictureState::Failed)
		{
			thumb.view = View::Failed;
		}
		return thumb;
	}

	void Retry()
	{
		Impl& impl = Instance();
		impl.fetcher.ClearFailures();
		impl.textures.ClearFailures();
	}

	void Shutdown()
	{
		Impl& impl = Instance();
		impl.textures.Shutdown(2000);
		impl.fetcher.Shutdown(2000);
	}
}

#endif // MODLOADER_CLIENT_BUILD
