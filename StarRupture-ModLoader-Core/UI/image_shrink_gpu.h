#pragma once

// The shrink pass of the image library: draws a texture we own into a new
// 8-bit RGBA texture at the stored size, then builds every smaller level from
// the one above it. It reads only textures the loader made (the temporary copy
// of a game image) and writes only the texture it creates, so it touches no
// engine resource.
//
// Compiled into the host DLL. The pipeline is modelled on the ImGui DX12
// backend's own: one root signature, a full-screen triangle, a pixel shader
// compiled at start-up. The shader reads single pixels (no sampler) so it can
// average premultiplied colour.
//
// Not thread-safe: the caller serialises Create, Record and Release.

#include "image_shrink.h"

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

namespace ImageShrink
{
	// The most levels one stored texture can have: images up to 32768 on a side.
	inline constexpr uint32_t kMaxLevels = 16;

	inline constexpr DXGI_FORMAT kStoredFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	struct Failure
	{
		const char* step = nullptr;
		HRESULT     hr   = S_OK;
	};

	// The format a source is viewed as for the shrink. The pass averages and stores raw values, as
	// it does for a typeless source, so an sRGB source is viewed as its UNORM twin; an sRGB view
	// would decode to linear first and the stored image would come out darker.
	inline DXGI_FORMAT RawViewFormat(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_BC1_UNORM_SRGB:      return DXGI_FORMAT_BC1_UNORM;
		case DXGI_FORMAT_BC2_UNORM_SRGB:      return DXGI_FORMAT_BC2_UNORM;
		case DXGI_FORMAT_BC3_UNORM_SRGB:      return DXGI_FORMAT_BC3_UNORM;
		case DXGI_FORMAT_BC7_UNORM_SRGB:      return DXGI_FORMAT_BC7_UNORM;
		default:                              return format;
		}
	}

	class Gpu
	{
	public:
		// Makes the pipeline and the descriptor heaps for `slots` shrinks recorded at
		// once. A shader-visible descriptor is read when the GPU runs the list, so each
		// shrink that may still be on the GPU needs a slot of its own.
		bool Create(ID3D12Device* device, uint32_t slots, Failure& failure);
		void Release();
		// The GPU may still be reading the descriptors: leave everything to process exit.
		void Abandon();
		bool Ready() const { return m_pipeline != nullptr; }

		// Records the shrink of `source` (a 2D texture in the pixel-shader-resource
		// state, one level read, viewed as `sourceFormat`) into a new texture of
		// `target` size with the full chain of levels, on the open list. `slot` picks the
		// descriptors, one of Create's `slots`.
		//
		// Returns the new texture (one reference, every level in the pixel-shader-resource
		// state once the list has run), or nullptr with `failure` set. The source must
		// stay alive until the list has run.
		ID3D12Resource* Record(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* source,
			DXGI_FORMAT sourceFormat, Size target, Failure& failure);

	private:
		Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
		Microsoft::WRL::ComPtr<ID3D12RootSignature>       m_rootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_pipeline;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_srvHeap;   // shader-visible, kMaxLevels per slot
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;   // kMaxLevels, read when recorded
		UINT                                              m_srvSize = 0;
		UINT                                              m_rtvSize = 0;
		uint32_t                                          m_slots   = 0;
	};
}
