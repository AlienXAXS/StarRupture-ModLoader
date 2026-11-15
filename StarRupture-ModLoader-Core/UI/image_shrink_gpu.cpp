#include "image_shrink_gpu.h"

#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler")

using Microsoft::WRL::ComPtr;

namespace ImageShrink
{
	namespace
	{
		// Each stored pixel is the average of the source pixels under it: ratio is source
		// pixels per stored pixel, and the footprint is read as a grid of single pixels,
		// one per source pixel it covers (at most 16 a side). Each pixel is premultiplied
		// before it is averaged, so a transparent pixel's colour does not tint its
		// neighbours; a filtered read would average the colours first.
		const char* const kShader = R"(
cbuffer Constants : register(b0)
{
	float2 ratio;      // source size / stored size
};
Texture2D<float4> source : register(t0);

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VSOut vs(uint id : SV_VertexID)
{
	VSOut o;
	o.uv  = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}

float4 ps(VSOut i) : SV_Target
{
	uint width, height;
	source.GetDimensions(width, height);
	int2 last = int2(width, height) - 1;
	int2 reads = clamp((int2)ceil(ratio), 1, 16);
	float4 sum = 0;
	for (int y = 0; y < reads.y; ++y)
	{
		for (int x = 0; x < reads.x; ++x)
		{
			float2 at = (floor(i.pos.xy) + (float2(x, y) + 0.5) / reads) * ratio;
			float4 c = source.Load(int3(min((int2)at, last), 0));
			sum += float4(c.rgb * c.a, c.a);
		}
	}
	sum /= (reads.x * reads.y);
	return float4(sum.a > 0 ? sum.rgb / sum.a : 0, sum.a);
}
)";

		struct Constants
		{
			float ratio[2];
		};

		bool Step(Failure& failure, const char* step, HRESULT hr)
		{
			if (SUCCEEDED(hr))
				return true;
			failure = { step, hr };
			return false;
		}

		bool Compile(Failure& failure, const char* entry, const char* target, ComPtr<ID3DBlob>& out)
		{
			ComPtr<ID3DBlob> errors;
			return Step(failure, entry, D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
				entry, target, 0, 0, &out, &errors));
		}
	}

	bool Gpu::Create(ID3D12Device* device, uint32_t slots, Failure& failure)
	{
		Release();
		if (!device || slots == 0)
			return Step(failure, "Create arguments", E_INVALIDARG);

		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors                    = 1;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
		params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
		params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges   = &range;
		params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
		rootDesc.NumParameters     = _countof(params);
		rootDesc.pParameters       = params;
		rootDesc.Flags            = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
		                             D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		                             D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		                             D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		ComPtr<ID3DBlob> rootBlob, rootErrors, vs, ps;
		if (!Step(failure, "D3D12SerializeRootSignature", D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors)) ||
			!Step(failure, "CreateRootSignature", device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))) ||
			!Compile(failure, "vs", "vs_5_0", vs) ||
			!Compile(failure, "ps", "ps_5_0", ps))
		{
			Release();
			return false;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
		pso.pRootSignature                      = m_rootSignature.Get();
		pso.VS                                  = { vs->GetBufferPointer(), vs->GetBufferSize() };
		pso.PS                                  = { ps->GetBufferPointer(), ps->GetBufferSize() };
		pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask                          = UINT_MAX;
		pso.RasterizerState.FillMode            = D3D12_FILL_MODE_SOLID;
		pso.RasterizerState.CullMode            = D3D12_CULL_MODE_NONE;
		pso.RasterizerState.DepthClipEnable     = TRUE;
		pso.PrimitiveTopologyType               = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets                    = 1;
		pso.RTVFormats[0]                       = kStoredFormat;
		pso.SampleDesc.Count                    = 1;

		D3D12_DESCRIPTOR_HEAP_DESC srvHeap = {};
		srvHeap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeap.NumDescriptors = slots * kMaxLevels;
		srvHeap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeap = {};
		rtvHeap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeap.NumDescriptors = kMaxLevels;

		if (!Step(failure, "CreateGraphicsPipelineState", device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline))) ||
			!Step(failure, "CreateDescriptorHeap (SRV)", device->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&m_srvHeap))) ||
			!Step(failure, "CreateDescriptorHeap (RTV)", device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&m_rtvHeap))))
		{
			Release();
			return false;
		}

		m_device  = device;
		m_srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		m_slots   = slots;
		return true;
	}

	void Gpu::Abandon()
	{
		m_rtvHeap.Detach();
		m_srvHeap.Detach();
		m_pipeline.Detach();
		m_rootSignature.Detach();
		m_device.Detach();
		Release();
	}

	void Gpu::Release()
	{
		m_rtvHeap.Reset();
		m_srvHeap.Reset();
		m_pipeline.Reset();
		m_rootSignature.Reset();
		m_device.Reset();
		m_slots = 0;
	}

	ID3D12Resource* Gpu::Record(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* source,
		DXGI_FORMAT sourceFormat, Size target, Failure& failure)
	{
		const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
		const uint32_t levels = LevelCount(target.width, target.height);
		if (!Ready() || slot >= m_slots || levels == 0 || levels > kMaxLevels || sourceDesc.Width == 0 || sourceDesc.Height == 0)
		{
			failure = { "Record arguments", E_INVALIDARG };
			return nullptr;
		}

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = target.width;
		desc.Height           = target.height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = static_cast<UINT16>(levels);
		desc.Format           = kStoredFormat;
		desc.SampleDesc.Count = 1;
		desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		ComPtr<ID3D12Resource> stored;
		if (!Step(failure, "CreateCommittedResource", m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&stored))))
			return nullptr;

		// Descriptor 0 of the slot views the source; descriptor 1 + k views level k of the
		// new texture, the input of the pass that makes level k + 1.
		const D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
		const D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
		const D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		const UINT first = slot * kMaxLevels;

		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Format                  = sourceFormat;
		srv.Texture2D.MipLevels     = 1;
		m_device->CreateShaderResourceView(source, &srv, { srvCpu.ptr + static_cast<SIZE_T>(first) * m_srvSize });

		srv.Format = kStoredFormat;
		for (uint32_t k = 0; k + 1 < levels; ++k)
		{
			srv.Texture2D.MostDetailedMip = k;
			m_device->CreateShaderResourceView(stored.Get(), &srv,
				{ srvCpu.ptr + static_cast<SIZE_T>(first + 1 + k) * m_srvSize });
		}

		ID3D12DescriptorHeap* const heaps[] = { m_srvHeap.Get() };
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootSignature(m_rootSignature.Get());
		list->SetPipelineState(m_pipeline.Get());
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		Size from = { static_cast<uint32_t>(sourceDesc.Width), sourceDesc.Height };
		for (uint32_t k = 0; k < levels; ++k)
		{
			const Size to = LevelSize(target, k);

			D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
			rtv.Format             = kStoredFormat;
			rtv.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = k;
			const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = { rtvCpu.ptr + static_cast<SIZE_T>(k) * m_rtvSize };
			m_device->CreateRenderTargetView(stored.Get(), &rtv, rtvHandle);
			list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

			const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(to.width), static_cast<float>(to.height), 0.0f, 1.0f };
			const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(to.width), static_cast<LONG>(to.height) };
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);

			const Constants constants = {
				{ static_cast<float>(from.width) / to.width, static_cast<float>(from.height) / to.height } };
			list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
			list->SetGraphicsRootDescriptorTable(1, { srvGpu.ptr + static_cast<UINT64>(first + k) * m_srvSize });
			list->DrawInstanced(3, 1, 0, 0);

			// Done as a target; the next level reads it, and the finished texture is only sampled.
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource   = stored.Get();
			barrier.Transition.Subresource = k;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			list->ResourceBarrier(1, &barrier);

			from = to;
		}

		return stored.Detach();
	}
}
