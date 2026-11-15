#include "image_readback_gpu.h"

#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ImageReadback
{
	namespace
	{
		constexpr uint32_t kSlotBits = 4;
		static_assert(kSlots <= (1u << kSlotBits), "a ticket holds the slot in its low bits");

		D3D12_RESOURCE_DESC StoredDesc(uint32_t width, uint32_t height)
		{
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width            = width;
			desc.Height           = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels        = static_cast<UINT16>(ImageShrink::LevelCount(width, height));
			desc.Format           = ImageShrink::kStoredFormat;
			desc.SampleDesc.Count = 1;
			return desc;
		}
	}

	bool Ring::Create(ID3D12Device* device, uint32_t maxSize)
	{
		Release();
		if (!device || maxSize == 0)
			return false;

		const D3D12_RESOURCE_DESC desc = StoredDesc(maxSize, maxSize);
		UINT64 total = 0;
		device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, nullptr, nullptr, nullptr, &total);

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bufferDesc = {};
		bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width            = total;
		bufferDesc.Height           = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels        = 1;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		for (Slot& slot : m_slots)
		{
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.buffer))))
			{
				Release();
				return false;
			}
		}
		m_device     = device;
		m_bufferSize = total;
		return true;
	}

	void Ring::Release()
	{
		for (Slot& slot : m_slots)
			slot = {};
		m_device.Reset();
		m_bufferSize = 0;
	}

	void Ring::Abandon()
	{
		for (Slot& slot : m_slots)
			slot.buffer.Detach();
		m_device.Detach();
		Release();
	}

	Ring::Slot* Ring::Find(uint32_t ticket)
	{
		const uint32_t index = ticket & ((1u << kSlotBits) - 1);
		if (ticket == 0 || index >= kSlots)
			return nullptr;
		Slot& slot = m_slots[index];
		return slot.serial == (ticket >> kSlotBits) ? &slot : nullptr;
	}

	uint32_t Ring::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* stored, UINT64 value)
	{
		if (!Ready() || !stored)
			return 0;

		uint32_t index = 0;
		while (index < kSlots && m_slots[index].serial != 0)
			++index;
		if (index == kSlots)
			return 0;
		Slot& slot = m_slots[index];

		const D3D12_RESOURCE_DESC desc = stored->GetDesc();
		const uint32_t levels = desc.MipLevels;
		if (levels == 0 || levels > ImageShrink::kMaxLevels || desc.Format != ImageShrink::kStoredFormat)
			return 0;

		UINT64 total = 0;
		m_device->GetCopyableFootprints(&desc, 0, levels, 0, slot.footprints, slot.rows, slot.rowBytes, &total);
		if (total > m_bufferSize)
			return 0;

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource   = stored;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
		list->ResourceBarrier(1, &barrier);

		for (UINT level = 0; level < levels; ++level)
		{
			D3D12_TEXTURE_COPY_LOCATION from = {};
			from.pResource        = stored;
			from.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			from.SubresourceIndex = level;
			D3D12_TEXTURE_COPY_LOCATION to = {};
			to.pResource       = slot.buffer.Get();
			to.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			to.PlacedFootprint = slot.footprints[level];
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		}

		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(1, &barrier);

		m_serial = (m_serial + 1) & ((1u << (32 - kSlotBits)) - 1);
		if (m_serial == 0)
			m_serial = 1;
		slot.serial = m_serial;
		slot.value  = value;
		slot.levels = levels;
		return (slot.serial << kSlotBits) | index;
	}

	void Ring::Cancel(uint32_t ticket)
	{
		if (Slot* slot = Find(ticket))
			slot->serial = 0;
	}

	Fetched Ring::Fetch(uint32_t ticket, UINT64 completed, uint8_t* dest, size_t capacity)
	{
		Slot* const slot = Find(ticket);
		if (!slot)
			return Fetched::Gone;
		if (completed < slot->value)
			return Fetched::Pending;

		uint64_t bytes = 0;
		for (uint32_t level = 0; level < slot->levels; ++level)
			bytes += slot->rowBytes[level] * slot->rows[level];

		const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(m_bufferSize) };
		const uint8_t* mapped = nullptr;
		Fetched result = Fetched::Gone;
		if (dest && bytes == capacity && SUCCEEDED(slot->buffer->Map(0, &readRange, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped)))))
		{
			size_t at = 0;
			for (uint32_t level = 0; level < slot->levels; ++level)
			{
				const size_t rowBytes = static_cast<size_t>(slot->rowBytes[level]);
				for (UINT row = 0; row < slot->rows[level]; ++row)
				{
					memcpy(dest + at, mapped + slot->footprints[level].Offset + static_cast<size_t>(row) * slot->footprints[level].Footprint.RowPitch, rowBytes);
					at += rowBytes;
				}
			}
			const D3D12_RANGE written = { 0, 0 };
			slot->buffer->Unmap(0, &written);
			result = Fetched::Done;
		}
		slot->serial = 0;
		return result;
	}
}
