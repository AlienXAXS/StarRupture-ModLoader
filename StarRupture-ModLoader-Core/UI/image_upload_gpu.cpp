#include "image_upload_gpu.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ImageUpload
{
	namespace
	{
		bool Step(ImageShrink::Failure& failure, const char* step, HRESULT hr)
		{
			if (SUCCEEDED(hr))
				return true;
			failure.step = step;
			failure.hr   = hr;
			return false;
		}
	}

	bool Gpu::Create(ID3D12Device* device, ImageShrink::Failure& failure)
	{
		if (!device)
			return false;

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		ComPtr<ID3D12CommandQueue>        queue;
		ComPtr<ID3D12CommandAllocator>    allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence>               fence;
		if (!Step(failure, "CreateCommandQueue", device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
			!Step(failure, "CreateCommandAllocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator))) ||
			!Step(failure, "CreateCommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
			!Step(failure, "CommandList::Close", list->Close()) ||
			!Step(failure, "CreateFence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			return false;

		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!event)
		{
			failure.step = "CreateEvent";
			failure.hr   = HRESULT_FROM_WIN32(GetLastError());
			return false;
		}

		m_device    = device;
		m_queue     = std::move(queue);
		m_allocator = std::move(allocator);
		m_list      = std::move(list);
		m_fence     = std::move(fence);
		m_event     = event;
		m_value     = 0;
		return true;
	}

	void Gpu::Release()
	{
		m_list.Reset();
		m_allocator.Reset();
		m_fence.Reset();
		m_queue.Reset();
		m_device.Reset();
		if (m_event)
		{
			CloseHandle(m_event);
			m_event = nullptr;
		}
	}

	void Gpu::Abandon()
	{
		m_list.Detach();
		m_allocator.Detach();
		m_fence.Detach();
		m_queue.Detach();
		m_device.Detach();
		m_event = nullptr;
	}

	ID3D12Resource* Gpu::Upload(uint32_t width, uint32_t height, uint32_t levels, const uint8_t* pixels, size_t bytes,
		const std::function<bool()>& stop, ImageShrink::Failure& failure)
	{
		if (!Ready() || !pixels || width == 0 || height == 0 || levels == 0 || levels > ImageShrink::kMaxLevels ||
			bytes != ImageShrink::Bytes({ width, height }, levels))
		{
			failure.step = "arguments";
			failure.hr   = E_INVALIDARG;
			return nullptr;
		}

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = width;
		desc.Height           = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = static_cast<UINT16>(levels);
		desc.Format           = ImageShrink::kStoredFormat;
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		ComPtr<ID3D12Resource> texture;
		if (!Step(failure, "CreateCommittedResource(texture)",
			m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture))))
			return nullptr;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[ImageShrink::kMaxLevels] = {};
		UINT   rows[ImageShrink::kMaxLevels] = {};
		UINT64 rowBytes[ImageShrink::kMaxLevels] = {};
		UINT64 total = 0;
		m_device->GetCopyableFootprints(&desc, 0, levels, 0, footprints, rows, rowBytes, &total);

		D3D12_RESOURCE_DESC bufferDesc = {};
		bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width            = total;
		bufferDesc.Height           = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels        = 1;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		ComPtr<ID3D12Resource> staging;
		if (!Step(failure, "CreateCommittedResource(upload)",
			m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))))
			return nullptr;

		uint8_t* mapped = nullptr;
		const D3D12_RANGE noRead = { 0, 0 };
		if (!Step(failure, "Map", staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
			return nullptr;
		size_t from = 0;
		for (uint32_t level = 0; level < levels; ++level)
		{
			for (UINT row = 0; row < rows[level]; ++row)
				memcpy(mapped + footprints[level].Offset + static_cast<size_t>(row) * footprints[level].Footprint.RowPitch,
					pixels + from + static_cast<size_t>(row) * rowBytes[level], static_cast<size_t>(rowBytes[level]));
			from += static_cast<size_t>(rowBytes[level]) * rows[level];
		}
		staging->Unmap(0, nullptr);

		if (!Step(failure, "CommandAllocator::Reset", m_allocator->Reset()) ||
			!Step(failure, "CommandList::Reset", m_list->Reset(m_allocator.Get(), nullptr)))
			return nullptr;
		for (uint32_t level = 0; level < levels; ++level)
		{
			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource        = texture.Get();
			dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = level;
			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource       = staging.Get();
			src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = footprints[level];
			m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}
		if (!Step(failure, "CommandList::Close", m_list->Close()))
			return nullptr;

		ID3D12CommandList* const lists[] = { m_list.Get() };
		m_queue->ExecuteCommandLists(1, lists);
		const UINT64 value = ++m_value;
		HANDLE event = static_cast<HANDLE>(m_event);
		if (!Step(failure, "Signal", m_queue->Signal(m_fence.Get(), value)) ||
			!Step(failure, "SetEventOnCompletion", m_fence->SetEventOnCompletion(value, event)))
		{
			// The list is on the queue and no fence will say when it ends.
			texture.Detach();
			staging.Detach();
			Abandon();
			return nullptr;
		}

		unsigned waited = 0;
		while (WaitForSingleObject(event, 50) != WAIT_OBJECT_0)
		{
			waited += 50;
			if (waited >= kWaitMs || (stop && stop()))
			{
				failure.step = "wait for the copy";
				failure.hr   = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
				texture.Detach();
				staging.Detach();
				Abandon();
				return nullptr;
			}
		}
		return texture.Detach();
	}
}
