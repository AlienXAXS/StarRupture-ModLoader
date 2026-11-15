#pragma once

// The readback of the image library's stored copy: after the shrink has drawn a copy, every
// level of it is copied into a buffer in readback memory, in the same command list. Nothing
// waits on the GPU: the caller asks later with the ticket Record gave, and the answer is
// "still on the GPU" until the list's fence has passed.
//
// A small ring of buffers. When all are taken, Record gives no ticket and that copy is simply
// not saved this session. A slot is free again once Fetch has taken its pixels.
//
// Compiled into the host DLL next to the shrink pass. Not thread-safe: the caller serialises
// every call (the texture mutex).

#include "image_shrink_gpu.h"

#include <d3d12.h>
#include <wrl/client.h>


namespace ImageReadback
{
	inline constexpr uint32_t kSlots = 4;

	enum class Fetched
	{
		Pending,   // the list has not finished on the GPU
		Done,      // dest holds every level, tightly packed, level 0 first
		Gone,      // no such ticket (already taken, never given, or the copy is not the size asked)
	};

	class Ring
	{
	public:
		// Makes the buffers, each big enough for a copy of maxSize on the larger side with the full
		// chain of levels.
		bool Create(ID3D12Device* device, uint32_t maxSize);
		// The GPU has finished with every slot.
		void Release();
		// The GPU may still be writing a slot: leave the buffers to process exit.
		void Abandon();
		bool Ready() const { return m_device != nullptr; }

		// Records the copy of every level of `stored` (8-bit RGBA, every level in the
		// pixel-shader-resource state, left in it) on the open list. `value` is the fence value the
		// list will be signalled with. Returns the ticket, or 0 when every slot is taken or the
		// copy does not fit.
		uint32_t Record(ID3D12GraphicsCommandList* list, ID3D12Resource* stored, UINT64 value);

		// Gives a slot back whose list was never run.
		void Cancel(uint32_t ticket);

		// Takes the pixels of a copy whose fence value `completed` has reached: `capacity` is
		// ImageShrink::Bytes of the stored size and levels, which Done fills exactly. Done and
		// Gone free the slot.
		Fetched Fetch(uint32_t ticket, UINT64 completed, uint8_t* dest, size_t capacity);

	private:
		struct Slot
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
			uint32_t serial = 0;   // 0: free
			UINT64   value  = 0;
			uint32_t levels = 0;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[ImageShrink::kMaxLevels] = {};
			UINT     rows[ImageShrink::kMaxLevels] = {};
			UINT64   rowBytes[ImageShrink::kMaxLevels] = {};
		};

		Slot* Find(uint32_t ticket);

		Microsoft::WRL::ComPtr<ID3D12Device> m_device;
		Slot     m_slots[kSlots];
		UINT64   m_bufferSize = 0;
		uint32_t m_serial = 0;
	};
}
