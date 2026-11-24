#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// readme_textures.h -- the pictures of the README page that is open, as GPU
// textures. Pure logic: the texture calls and the hop to the game thread are
// passed in (TextureOps), so no ImGui and no game is needed to test it.
//
// Who does what:
//   render thread  Request(slot) once per frame for a picture whose bytes are
//                  ready, Get(slot) to draw. It never decodes and never loads.
//   game thread    the posted job: takes the bytes, loads the texture (WIC
//                  decode), checks its size, publishes the result.
// One job runs at a time, so a request while one is in flight does nothing; the
// next frame asks again.
//
// A page is one generation. BeginPage and EndPage start a new one and free every
// texture of the old one. A job that finishes after its page ended sees a newer
// generation and frees its own texture instead of showing it, so each Load is
// paired with a FreeTexture on every path. Shutdown frees what is left and waits
// for a load that is running right now.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	constexpr int    kMaxTextureSide = 4096;   // a decoded picture wider or taller is freed and marked Failed
	constexpr size_t kMaxPictures    = 8;      // = kMaxImages (readme_fetcher.h)

	enum class PictureState { None, Decoding, Ready, Failed };

	struct Picture
	{
		PictureState state  = PictureState::None;
		void*        handle = nullptr;   // Ready only
		int          width  = 0;
		int          height = 0;
	};

	// The size a picture is drawn at: its own size, shrunk (never enlarged) to fit
	// maxWidth and maxHeight, aspect kept. A size that is not positive gives 0 x 0.
	inline void FitPicture(int width, int height, float maxWidth, float maxHeight, float* outWidth, float* outHeight)
	{
		float w = static_cast<float>(width);
		float h = static_cast<float>(height);
		if (w <= 0.0f || h <= 0.0f || maxWidth <= 0.0f || maxHeight <= 0.0f)
		{
			*outWidth = 0.0f;
			*outHeight = 0.0f;
			return;
		}
		float scale = 1.0f;
		if (w > maxWidth)
			scale = maxWidth / w;
		if (h * scale > maxHeight)
			scale = maxHeight / h;
		*outWidth = w * scale;
		*outHeight = h * scale;
	}

	struct TextureOps
	{
		// Game thread only. Decodes and uploads; null on failure. May throw.
		std::function<void*(const void* data, size_t size, const char* name)> load;
		// Any thread. Safe with null.
		std::function<void(void* handle)> free;
		// Any thread: the texture's size.
		std::function<void(void* handle, int* width, int* height)> size;
		// Runs the job later on the game thread. Any thread may call it.
		std::function<void(std::function<void()> job)> post;
		// The encoded bytes of one picture, handed out once (ReadmeFetcher::TakeImageBytes).
		std::function<std::string(size_t slot)> take;
	};

	class ReadmeTextures
	{
	public:
		explicit ReadmeTextures(TextureOps ops);
		~ReadmeTextures();
		ReadmeTextures(const ReadmeTextures&) = delete;
		ReadmeTextures& operator=(const ReadmeTextures&) = delete;

		// A new page: frees the old page's textures; a job still in flight for it
		// will free its own result.
		void BeginPage();

		// The page is over: same as BeginPage.
		void EndPage();

		// Asks for the picture to be made, when none is in flight and the slot has
		// not been tried. Render thread, once per frame per picture with bytes ready.
		void Request(size_t slot);

		// What to draw. Render thread.
		Picture Get(size_t slot) const;

		// Textures held right now (Ready pictures).
		size_t Held() const;

		// No more jobs; frees what is held and waits up to waitMs for a load that is
		// running. Returns whether none is running.
		bool Shutdown(unsigned waitMs);

		struct State;   // shared with the posted job; defined in the .cpp

	private:
		void NewGeneration();

		TextureOps m_ops;
		std::shared_ptr<State> m_state;
	};
}
