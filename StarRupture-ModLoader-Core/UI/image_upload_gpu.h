#pragma once

// The upload of the image library's saved files: the pixels of a stored copy (8-bit RGBA,
// every level tightly packed, as image_cache_file.h holds them) are copied into a new texture
// on a copy queue of its own. It shares no queue, list or fence with the game or with the
// overlay, and it waits only on its own fence, so only the thread that calls it waits, and that
// is the cache's worker, never the game thread or the render thread.
//
// The texture is made with simultaneous access and left in the common state, so the overlay's
// queue can promote it for sampling without a barrier the copy queue could not record.
//
// Compiled into the host DLL next to the shrink pass. Not thread-safe: one thread at a time.

#include "image_shrink_gpu.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <functional>

namespace ImageUpload
{
	// How long Upload waits for its list, in milliseconds, polling `stop` between short waits.
	inline constexpr unsigned kWaitMs = 3000;

	class Gpu
	{
	public:
		bool Create(ID3D12Device* device, ImageShrink::Failure& failure);
		// The GPU has finished with everything: releases the queue and its objects.
		void Release();
		// A list may still be running: leave everything to process exit.
		void Abandon();
		bool Ready() const { return m_queue != nullptr; }

		// Makes a texture of width x height with `levels` levels (ImageShrink::Bytes of them is
		// `bytes`) from `pixels`, and waits until the copy has run. Returns it with one reference,
		// or nullptr with `failure` set. `stop` is asked while waiting: when it says true the wait
		// ends, the texture and the upload buffer are left to process exit, and the context is
		// abandoned (every later call fails).
		ID3D12Resource* Upload(uint32_t width, uint32_t height, uint32_t levels, const uint8_t* pixels, size_t bytes,
			const std::function<bool()>& stop, ImageShrink::Failure& failure);

	private:
		Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_queue;
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_allocator;
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
		Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
		void*                                             m_event = nullptr;
		UINT64                                            m_value = 0;
	};
}
