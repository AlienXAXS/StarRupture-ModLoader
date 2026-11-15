// Image shrink harness: runs the loader's shrink pass (UI/image_shrink_gpu.cpp) on a desktop
// D3D12 device with the debug layer on. No game, no window, no swap chain.
//
// Each case builds an image like the game's (compressed or BGRA, with or without smaller
// levels), picks the level the loader copies (IconCopy::PickCopyLevel), copies it to a
// one-level temporary texture, shrinks that, then reads the result back and checks it. The
// shrink is timed on the GPU with timestamp queries. The run ends with the debug layer's
// error count, which must be 0. The upload of a saved file (UI/image_upload_gpu.cpp) is checked
// too: pixels as a file holds them go in, and reading the texture back gives them on every level.
//
// With /sheet it draws real images at the sizes the UI uses instead; see the project file.
#include "UI/image_readback_gpu.h"
#include "UI/image_shrink_gpu.h"
#include "UI/image_upload_gpu.h"
#include "UI/icon_copy.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "windowscodecs")

using Microsoft::WRL::ComPtr;
using ImageShrink::Size;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

// A failed D3D call ends the run: nothing after it would mean anything.
static void Ok(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + " failed, hr=0x" + std::to_string(static_cast<unsigned long>(hr)));
}
#define OK(expr) Ok((expr), #expr)

// ---------------------------------------------------------------------------------------
// Device, queue, and the debug layer's messages

struct Context
{
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12InfoQueue>           info;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    UINT64                            value = 0;
    HANDLE                            event = nullptr;
    UINT64                            frequency = 0;
    std::string                       adapter;

    void Begin()
    {
        OK(allocator->Reset());
        OK(list->Reset(allocator.Get(), nullptr));
    }

    void Submit()
    {
        OK(list->Close());
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        OK(queue->Signal(fence.Get(), ++value));
        OK(fence->SetEventOnCompletion(value, event));
        WaitForSingleObject(event, INFINITE);
    }
};

static void Create(Context& c)
{
    // The layer comes with Windows' optional Graphics Tools feature. Without it the run goes
    // on, so the shrink itself can still be checked, but it says so and does not pass.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    else
        printf("debug layer: NOT AVAILABLE (the Graphics Tools feature is not installed)\n");

    ComPtr<IDXGIFactory6> factory;
    OK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0; !chosen; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device))))
        {
            chosen = adapter;
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            c.adapter = name;
        }
    }
    if (!chosen)
        throw std::runtime_error("no hardware D3D12 adapter");

    if (debug && SUCCEEDED(c.device.As(&c.info)))
        c.info->SetMessageCountLimit(100000);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    OK(c.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&c.queue)));
    OK(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator)));
    OK(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr, IID_PPV_ARGS(&c.list)));
    OK(c.list->Close());
    OK(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)));
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    OK(c.queue->GetTimestampFrequency(&c.frequency));
}

// Prints every error and corruption message the layer stored, and returns how many, or -1
// when there is no layer.
static int CountDebugErrors(Context& c)
{
    if (!c.info)
        return -1;
    int errors = 0;
    const UINT64 count = c.info->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T length = 0;
        c.info->GetMessage(i, nullptr, &length);
        std::vector<char> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        c.info->GetMessage(i, message, &length);
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            ++errors;
            printf("    debug layer: %s\n", message->pDescription);
        }
    }
    return errors;
}

// ---------------------------------------------------------------------------------------
// Resources

static ComPtr<ID3D12Resource> MakeTexture(Context& c, DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t mips,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = static_cast<UINT16>(mips);
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    desc.Flags            = flags;
    ComPtr<ID3D12Resource> texture;
    OK(c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&texture)));
    return texture;
}

static ComPtr<ID3D12Resource> MakeBuffer(Context& c, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    OK(c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)));
    return buffer;
}

static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT subresource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    list->ResourceBarrier(1, &barrier);
}

// Fills every level of `texture` (created in the copy-destination state) from tightly packed
// rows, then leaves it in the pixel-shader-resource state.
static void Upload(Context& c, ID3D12Resource* texture, const std::vector<std::vector<uint8_t>>& levels)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const UINT count = static_cast<UINT>(levels.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT>   rows(count);
    std::vector<UINT64> rowSize(count);
    UINT64 total = 0;
    c.device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), rowSize.data(), &total);

    ComPtr<ID3D12Resource> upload = MakeBuffer(c, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    OK(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < count; ++i)
        for (UINT r = 0; r < rows[i]; ++r)
            memcpy(mapped + footprints[i].Offset + static_cast<size_t>(r) * footprints[i].Footprint.RowPitch,
                levels[i].data() + static_cast<size_t>(r) * rowSize[i], static_cast<size_t>(rowSize[i]));
    upload->Unmap(0, nullptr);

    c.Begin();
    for (UINT i = 0; i < count; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource        = texture;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[i];
        c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(c.list.Get(), texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    c.Submit();
}

// One level of an 8-bit RGBA image, read back from the GPU.
struct Pixels
{
    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba;

    const uint8_t* At(uint32_t x, uint32_t y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

// Reads every level of an R8G8B8A8 texture in the pixel-shader-resource state.
static std::vector<Pixels> Readback(Context& c, ID3D12Resource* texture,
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const UINT count = desc.MipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT>   rows(count);
    std::vector<UINT64> rowSize(count);
    UINT64 total = 0;
    c.device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), rowSize.data(), &total);

    ComPtr<ID3D12Resource> readback = MakeBuffer(c, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    c.Begin();
    Transition(c.list.Get(), texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < count; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = texture;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = readback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprints[i];
        c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(c.list.Get(), texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    c.Submit();

    const uint8_t* mapped = nullptr;
    OK(readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))));
    std::vector<Pixels> levels(count);
    for (UINT i = 0; i < count; ++i)
    {
        levels[i].width  = footprints[i].Footprint.Width;
        levels[i].height = footprints[i].Footprint.Height;
        levels[i].rgba.resize(static_cast<size_t>(rowSize[i]) * rows[i]);
        for (UINT r = 0; r < rows[i]; ++r)
            memcpy(&levels[i].rgba[static_cast<size_t>(r) * rowSize[i]],
                mapped + footprints[i].Offset + static_cast<size_t>(r) * footprints[i].Footprint.RowPitch, static_cast<size_t>(rowSize[i]));
    }
    readback->Unmap(0, nullptr);
    return levels;
}

// ---------------------------------------------------------------------------------------
// Test images: made on the CPU as 8-bit RGBA, then encoded to the format under test

enum class Pattern
{
    Split,      // left half red, right half blue, opaque
    Premul,     // left half opaque red, right half white at alpha 0: colour that must not leak
    Grey,       // opaque mid grey (128), a value an sRGB decode would change
};

static std::vector<uint8_t> MakePattern(uint32_t width, uint32_t height, Pattern pattern)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            const bool left = x < width / 2;
            if (pattern == Pattern::Split)
            {
                p[0] = left ? 255 : 0; p[1] = 0; p[2] = left ? 0 : 255; p[3] = 255;
            }
            else if (pattern == Pattern::Grey)
            {
                p[0] = p[1] = p[2] = 128; p[3] = 255;
            }
            else
            {
                p[0] = 255; p[1] = left ? 0 : 255; p[2] = left ? 0 : 255; p[3] = left ? 255 : 0;
            }
        }
    return rgba;
}

// The next level down by a 2x2 box average.
static std::vector<uint8_t> HalveRgba(const std::vector<uint8_t>& from, uint32_t width, uint32_t height, uint32_t& outWidth, uint32_t& outHeight)
{
    outWidth  = std::max(width / 2, 1u);
    outHeight = std::max(height / 2, 1u);
    std::vector<uint8_t> to(static_cast<size_t>(outWidth) * outHeight * 4);
    for (uint32_t y = 0; y < outHeight; ++y)
        for (uint32_t x = 0; x < outWidth; ++x)
            for (int ch = 0; ch < 4; ++ch)
            {
                int sum = 0;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const uint32_t sx = std::min(x * 2 + dx, width - 1);
                        const uint32_t sy = std::min(y * 2 + dy, height - 1);
                        sum += from[(static_cast<size_t>(sy) * width + sx) * 4 + ch];
                    }
                to[(static_cast<size_t>(y) * outWidth + x) * 4 + ch] = static_cast<uint8_t>((sum + 2) / 4);
            }
    return to;
}

static uint16_t ToHalf(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127 + 15;
    if (value == 0.0f || exponent <= 0)
        return static_cast<uint16_t>(sign);
    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00);
    return static_cast<uint16_t>(sign | (exponent << 10) | ((bits & 0x7fffff) >> 13));
}

static uint16_t Pack565(const int rgb[3])
{
    return static_cast<uint16_t>(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}

static void Expand565(uint16_t value, int rgb[3])
{
    const int r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// One 4x4 BC3 block: alpha by two end points and eight steps, colour by the bounding
// box of the block's colours and four steps.
static void EncodeBc3Block(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0, uint8_t out[16])
{
    int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
    int alphaLo = 255, alphaHi = 0;
    const uint8_t* texel[16];
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t x = std::min(x0 + i % 4, width - 1);
        const uint32_t y = std::min(y0 + i / 4, height - 1);
        texel[i] = &rgba[(static_cast<size_t>(y) * width + x) * 4];
        for (int ch = 0; ch < 3; ++ch)
        {
            lo[ch] = std::min<int>(lo[ch], texel[i][ch]);
            hi[ch] = std::max<int>(hi[ch], texel[i][ch]);
        }
        alphaLo = std::min<int>(alphaLo, texel[i][3]);
        alphaHi = std::max<int>(alphaHi, texel[i][3]);
    }

    int alphaSteps[8] = { alphaHi, alphaLo };
    for (int i = 1; i <= 6; ++i)
        alphaSteps[1 + i] = ((7 - i) * alphaHi + i * alphaLo) / 7;
    uint64_t alphaBits = 0;
    for (int i = 0; i < 16; ++i)
    {
        int best = 0;
        for (int s = 1; s < 8; ++s)
            if (abs(alphaSteps[s] - texel[i][3]) < abs(alphaSteps[best] - texel[i][3]))
                best = s;
        alphaBits |= static_cast<uint64_t>(best) << (3 * i);
    }
    out[0] = static_cast<uint8_t>(alphaHi);
    out[1] = static_cast<uint8_t>(alphaLo);
    for (int i = 0; i < 6; ++i)
        out[2 + i] = static_cast<uint8_t>(alphaBits >> (8 * i));

    const uint16_t c0 = Pack565(hi), c1 = Pack565(lo);
    int colour[4][3];
    Expand565(c0, colour[0]);
    Expand565(c1, colour[1]);
    for (int ch = 0; ch < 3; ++ch)
    {
        colour[2][ch] = (2 * colour[0][ch] + colour[1][ch]) / 3;
        colour[3][ch] = (colour[0][ch] + 2 * colour[1][ch]) / 3;
    }
    uint32_t colourBits = 0;
    for (int i = 0; i < 16; ++i)
    {
        int best = 0, bestDistance = INT_MAX;
        for (int s = 0; s < 4; ++s)
        {
            int distance = 0;
            for (int ch = 0; ch < 3; ++ch)
                distance += (colour[s][ch] - texel[i][ch]) * (colour[s][ch] - texel[i][ch]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = s;
            }
        }
        colourBits |= static_cast<uint32_t>(best) << (2 * i);
    }
    memcpy(out + 8, &c0, 2);
    memcpy(out + 10, &c1, 2);
    memcpy(out + 12, &colourBits, 4);
}

static std::vector<uint8_t> Encode(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    std::vector<uint8_t> out;
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        out = rgba;
        for (size_t i = 0; i < out.size(); i += 4)
            std::swap(out[i], out[i + 2]);
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        out.resize(rgba.size() * 2);
        for (size_t i = 0; i < rgba.size(); ++i)
        {
            const uint16_t half = ToHalf(rgba[i] / 255.0f);
            memcpy(&out[i * 2], &half, 2);
        }
        break;
    case DXGI_FORMAT_BC3_UNORM:
    {
        const uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
        out.resize(static_cast<size_t>(blocksX) * blocksY * 16);
        for (uint32_t by = 0; by < blocksY; ++by)
            for (uint32_t bx = 0; bx < blocksX; ++bx)
                EncodeBc3Block(rgba, width, height, bx * 4, by * 4, &out[(static_cast<size_t>(by) * blocksX + bx) * 16]);
        break;
    }
    default:
        throw std::runtime_error("format not handled by the harness");
    }
    return out;
}

// An image as the game holds it: a texture in the pixel-shader-resource state.
struct Image
{
    ComPtr<ID3D12Resource> texture;
    DXGI_FORMAT            format = DXGI_FORMAT_UNKNOWN;
    uint32_t               width = 0;
    uint32_t               height = 0;
    uint32_t               mips = 1;
};

static Image MakeImage(Context& c, DXGI_FORMAT format, uint32_t width, uint32_t height, bool withMips, Pattern pattern)
{
    Image image;
    image.format = format;
    image.width  = width;
    image.height = height;
    image.mips   = withMips ? ImageShrink::LevelCount(width, height) : 1;

    std::vector<std::vector<uint8_t>> levels;
    std::vector<uint8_t> rgba = MakePattern(width, height, pattern);
    uint32_t w = width, h = height;
    for (uint32_t k = 0; k < image.mips; ++k)
    {
        levels.push_back(Encode(rgba, w, h, format));
        uint32_t nextW, nextH;
        rgba = HalveRgba(rgba, w, h, nextW, nextH);
        w = nextW;
        h = nextH;
    }
    image.texture = MakeTexture(c, format, width, height, image.mips, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    Upload(c, image.texture.Get(), levels);
    return image;
}

// What the loader does before the shrink: picks the level to copy and copies it to a texture
// with one level.
struct Picked
{
    ComPtr<ID3D12Resource> texture;
    Size                   size;
};

static Picked CopyPickedLevel(Context& c, const Image& image, uint32_t minSize)
{
    const bool compressed = image.format == DXGI_FORMAT_BC3_UNORM;
    const IconCopy::LevelPick pick = IconCopy::PickCopyLevel(image.width, image.height, image.mips, minSize, compressed ? 4 : 1);
    Picked picked;
    picked.size    = { pick.width, pick.height };
    picked.texture = MakeTexture(c, image.format, pick.width, pick.height, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    c.Begin();
    Transition(c.list.Get(), image.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = picked.texture.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource        = image.texture.Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = pick.level;
    c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(c.list.Get(), image.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(c.list.Get(), picked.texture.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    c.Submit();
    return picked;
}

// ---------------------------------------------------------------------------------------
// The checks

static bool Near(const uint8_t* pixel, int r, int g, int b, int a, int tolerance)
{
    return abs(pixel[0] - r) <= tolerance && abs(pixel[1] - g) <= tolerance &&
           abs(pixel[2] - b) <= tolerance && abs(pixel[3] - a) <= tolerance;
}

struct Case
{
    const char* name;
    DXGI_FORMAT format;
    uint32_t    width;
    uint32_t    height;
    bool        withMips;
    Pattern     pattern;
};

static const uint32_t kStoredSize = 256;      // ImageLibrary::kStoredSize; the shrink takes the size as an argument
static const int      kTimedRuns  = 10;

static void RunCase(Context& c, ImageShrink::Gpu& gpu, ImageReadback::Ring& ring, ID3D12QueryHeap* queries, ID3D12Resource* queryReadback, const Case& test)
{
    const Image image = MakeImage(c, test.format, test.width, test.height, test.withMips, test.pattern);
    const Picked picked = CopyPickedLevel(c, image, kStoredSize);
    const Size target = ImageShrink::StoredSize(picked.size.width, picked.size.height, kStoredSize);

    // One warm-up shrink, then the timed ones. Each is its own submission, as in the game.
    ComPtr<ID3D12Resource> stored;
    uint32_t ticket = 0;
    double totalMs = 0, minMs = 1e9, maxMs = 0;
    for (int run = 0; run <= kTimedRuns; ++run)
    {
        c.Begin();
        c.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        ImageShrink::Failure failure;
        ID3D12Resource* made = gpu.Record(c.list.Get(), 0, picked.texture.Get(), test.format, target, failure);
        c.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        c.list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback, 0);
        if (!made)
            throw std::runtime_error(std::string("Record failed at ") + failure.step);
        // The last shrink also records the readback of its result in the same list, as the loader does.
        if (run == kTimedRuns)
            ticket = ring.Record(c.list.Get(), made, c.value + 1);
        c.Submit();
        stored.Attach(made);

        const UINT64* ticks = nullptr;
        OK(queryReadback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<UINT64**>(&ticks))));
        const double ms = (ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(c.frequency);
        queryReadback->Unmap(0, nullptr);
        if (run > 0)
        {
            totalMs += ms;
            minMs = std::min(minMs, ms);
            maxMs = std::max(maxMs, ms);
        }
    }

    const int failedBefore = g_failed;
    const D3D12_RESOURCE_DESC desc = stored->GetDesc();
    const uint32_t levels = ImageShrink::LevelCount(target.width, target.height);
    CHECK(desc.Width == target.width && desc.Height == target.height);
    CHECK(desc.MipLevels == levels);
    CHECK(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    // A larger image goes to the stored size, a smaller one keeps its own.
    CHECK(std::max(target.width, target.height) <= kStoredSize);
    CHECK(std::max(picked.size.width, picked.size.height) > kStoredSize
        ? std::max(target.width, target.height) == kStoredSize
        : target.width == picked.size.width && target.height == picked.size.height);

    const std::vector<Pixels> read = Readback(c, stored.Get());
    CHECK(read.size() == levels);
    if (read.size() == levels)
    {
        // Every level was drawn: the left quarter shows the left half's colour.
        for (uint32_t k = 0; k < levels; ++k)
        {
            if (read[k].width < 4)
                continue;
            const uint8_t* left = read[k].At(read[k].width / 4, read[k].height / 2);
            CHECK(Near(left, 255, 0, 0, 255, k == 0 ? 6 : 24));
        }
        const Pixels& top = read[0];
        const uint8_t* right = top.At(top.width * 3 / 4, top.height / 2);
        const uint8_t* last = read[levels - 1].At(0, 0);
        if (test.pattern == Pattern::Split)
        {
            CHECK(Near(right, 0, 0, 255, 255, 6));
            CHECK(Near(last, 128, 0, 128, 255, 16));
        }
        else
        {
            // Half the image is transparent white: no white may reach the opaque half's colour.
            CHECK(right[3] <= 2);
            CHECK(Near(last, 255, 0, 0, 128, 12));        }
    }

    // The readback recorded in the same list holds every level, tightly packed, as the shrink made it.
    CHECK(ticket != 0);
    if (ticket != 0 && read.size() == levels)
    {
        std::vector<uint8_t> expected;
        for (const Pixels& level : read)
            expected.insert(expected.end(), level.rgba.begin(), level.rgba.end());
        CHECK(expected.size() == ImageShrink::Bytes(target, levels));

        std::vector<uint8_t> bytes(expected.size());
        CHECK(ring.Fetch(ticket, c.fence->GetCompletedValue(), bytes.data(), bytes.size()) == ImageReadback::Fetched::Done);
        CHECK(bytes == expected);
        // The ticket is spent.
        CHECK(ring.Fetch(ticket, c.fence->GetCompletedValue(), bytes.data(), bytes.size()) == ImageReadback::Fetched::Gone);
    }

    printf("%-34s %-10s %4ux%-4u -> %3ux%-3u %2u levels  %-4s  %.3f ms (min %.3f, max %.3f)\n", test.name,
        test.format == DXGI_FORMAT_BC3_UNORM ? "BC3" : test.format == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA" : "RGBA16F",
        picked.size.width, picked.size.height, target.width, target.height, levels,
        g_failed == failedBefore ? "ok" : "FAIL", totalMs / kTimedRuns, minMs, maxMs);
}

// The ring has a fixed number of slots: with all taken a copy gets no ticket, a ticket is pending
// until the list has run, and a slot comes back once it has been fetched or cancelled.
static void RunRingChecks(Context& c, ImageShrink::Gpu& gpu, ImageReadback::Ring& ring)
{
    const Image image = MakeImage(c, DXGI_FORMAT_B8G8R8A8_UNORM, 512, 512, false, Pattern::Split);
    const Picked picked = CopyPickedLevel(c, image, kStoredSize);
    const Size target = ImageShrink::StoredSize(picked.size.width, picked.size.height, kStoredSize);
    const uint32_t levels = ImageShrink::LevelCount(target.width, target.height);
    const size_t bytes = static_cast<size_t>(ImageShrink::Bytes(target, levels));

    ImageShrink::Failure failure;
    c.Begin();
    ID3D12Resource* made = gpu.Record(c.list.Get(), 0, picked.texture.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, target, failure);
    if (!made)
        throw std::runtime_error(std::string("Record failed at ") + failure.step);
    ComPtr<ID3D12Resource> stored;
    stored.Attach(made);

    uint32_t tickets[ImageReadback::kSlots] = {};
    for (uint32_t i = 0; i < ImageReadback::kSlots; ++i)
    {
        tickets[i] = ring.Record(c.list.Get(), stored.Get(), c.value + 1);
        CHECK(tickets[i] != 0);
    }
    // Every slot is taken: this image is not saved.
    CHECK(ring.Record(c.list.Get(), stored.Get(), c.value + 1) == 0);

    // The list has not run: the copy is pending.
    std::vector<uint8_t> dest(bytes);
    CHECK(ring.Fetch(tickets[0], c.fence->GetCompletedValue(), dest.data(), dest.size()) == ImageReadback::Fetched::Pending);
    c.Submit();

    // A copy asked for at the wrong size is refused; a fetched slot comes back, and only once.
    std::vector<uint8_t> wrong(bytes - 1);
    CHECK(ring.Fetch(tickets[1], c.fence->GetCompletedValue(), wrong.data(), wrong.size()) == ImageReadback::Fetched::Gone);
    CHECK(ring.Fetch(tickets[0], c.fence->GetCompletedValue(), dest.data(), dest.size()) == ImageReadback::Fetched::Done);
    CHECK(ring.Fetch(tickets[0], c.fence->GetCompletedValue(), dest.data(), dest.size()) == ImageReadback::Fetched::Gone);
    for (uint32_t i = 2; i < ImageReadback::kSlots; ++i)
        CHECK(ring.Fetch(tickets[i], c.fence->GetCompletedValue(), dest.data(), dest.size()) == ImageReadback::Fetched::Done);

    // A slot whose list has run is given back too (a signal that failed after the submit): all
    // slots can be taken again.
    c.Begin();
    uint32_t again[ImageReadback::kSlots] = {};
    for (uint32_t i = 0; i < ImageReadback::kSlots; ++i)
    {
        again[i] = ring.Record(c.list.Get(), stored.Get(), c.value + 1);
        CHECK(again[i] != 0);
    }
    CHECK(ring.Record(c.list.Get(), stored.Get(), c.value + 1) == 0);
    c.Submit();
    for (uint32_t i = 0; i < ImageReadback::kSlots; ++i)
        ring.Cancel(again[i]);
    c.Begin();
    for (uint32_t i = 0; i < ImageReadback::kSlots; ++i)
    {
        again[i] = ring.Record(c.list.Get(), stored.Get(), c.value + 1);
        CHECK(again[i] != 0);
    }
    for (uint32_t i = 0; i < ImageReadback::kSlots; ++i)
        ring.Cancel(again[i]);
    OK(c.list->Close());

    // A slot whose list was never run is given back.
    c.Begin();
    const uint32_t first = ring.Record(c.list.Get(), stored.Get(), c.value + 1);
    CHECK(first != 0);
    ring.Cancel(first);
    const uint32_t second = ring.Record(c.list.Get(), stored.Get(), c.value + 1);
    CHECK(second != 0);
    ring.Cancel(second);
    OK(c.list->Close());
}

// A source created as sRGB is stored with its raw values, like a typeless one: a mid grey of 128 stays
// 128. Viewed as sRGB the shrink would decode it first and store about 55.
static void RunSrgbChecks(Context& c, ImageShrink::Gpu& gpu)
{
    const DXGI_FORMAT srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    CHECK(ImageShrink::RawViewFormat(srgb) == DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(ImageShrink::RawViewFormat(DXGI_FORMAT_BC3_UNORM_SRGB) == DXGI_FORMAT_BC3_UNORM);
    CHECK(ImageShrink::RawViewFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(ImageShrink::RawViewFormat(DXGI_FORMAT_BC3_UNORM) == DXGI_FORMAT_BC3_UNORM);
    CHECK(ImageShrink::RawViewFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_TYPELESS);

    const Image image = MakeImage(c, srgb, 64, 64, false, Pattern::Grey);
    const Picked picked = CopyPickedLevel(c, image, kStoredSize);
    const Size target = ImageShrink::StoredSize(picked.size.width, picked.size.height, kStoredSize);

    const DXGI_FORMAT views[2] = { ImageShrink::RawViewFormat(srgb), srgb };
    int grey[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        ImageShrink::Failure failure;
        c.Begin();
        ID3D12Resource* made = gpu.Record(c.list.Get(), 0, picked.texture.Get(), views[i], target, failure);
        if (!made)
            throw std::runtime_error(std::string("Record failed at ") + failure.step);
        c.Submit();
        ComPtr<ID3D12Resource> stored;
        stored.Attach(made);
        const std::vector<Pixels> read = Readback(c, stored.Get());
        CHECK(!read.empty());
        if (!read.empty())
            grey[i] = read[0].At(read[0].width / 2, read[0].height / 2)[0];
    }
    CHECK(abs(grey[0] - 128) <= 2);
    // The sRGB view: darker, which is what the raw view is there to avoid.
    CHECK(grey[1] < 100);
    printf("sRGB source, mid grey 128: stored %d with the raw view (sRGB view: %d)\n", grey[0], grey[1]);
}

// The upload of a saved file: pixels as a file holds them (every level tightly packed) become a
// texture on the upload's own copy queue, and reading it back gives the same bytes on every level.
// Shapes the shrink makes, and one that is not square, with one level and with a full chain.
static void RunUploadChecks(Context& c)
{
    ImageUpload::Gpu upload;
    ImageShrink::Failure failure;
    CHECK(upload.Create(c.device.Get(), failure));
    if (!upload.Ready())
        return;

    struct Shape { uint32_t width, height, levels; };
    const Shape shapes[] = {
        { 256, 256, ImageShrink::LevelCount(256, 256) },
        { 256, 128, ImageShrink::LevelCount(256, 128) },
        { 200, 120, ImageShrink::LevelCount(200, 120) },
        { 64,  64,  1 },
        { 1,   1,   1 },
    };

    uint8_t seed = 11;
    for (const Shape& shape : shapes)
    {
        const Size size = { shape.width, shape.height };
        std::vector<uint8_t> file(static_cast<size_t>(ImageShrink::Bytes(size, shape.levels)));
        for (size_t i = 0; i < file.size(); ++i)
            file[i] = static_cast<uint8_t>(seed + i * 13 + (i >> 8));
        ++seed;

        ComPtr<ID3D12Resource> texture;
        texture.Attach(upload.Upload(shape.width, shape.height, shape.levels, file.data(), file.size(), [] { return false; }, failure));
        CHECK(texture != nullptr);
        if (!texture)
            continue;

        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        CHECK(desc.Width == shape.width && desc.Height == shape.height && desc.MipLevels == shape.levels);
        CHECK(desc.Format == ImageShrink::kStoredFormat);
        CHECK((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0);

        // The upload left it in the common state; the harness queue reads it from there.
        const std::vector<Pixels> levels = Readback(c, texture.Get(), D3D12_RESOURCE_STATE_COMMON);
        CHECK(levels.size() == shape.levels);
        size_t offset = 0;
        bool same = levels.size() == shape.levels;
        for (uint32_t i = 0; same && i < shape.levels; ++i)
        {
            const Size at = ImageShrink::LevelSize(size, i);
            const size_t bytes = static_cast<size_t>(at.width) * at.height * ImageShrink::kBytesPerPixel;
            same = levels[i].width == at.width && levels[i].height == at.height && levels[i].rgba.size() == bytes
                && memcmp(levels[i].rgba.data(), file.data() + offset, bytes) == 0;
            if (!same)
                printf("    level %u of %ux%u differs\n", i, shape.width, shape.height);
            offset += bytes;
        }
        CHECK(same);
        CHECK(offset == file.size());
    }

    // A stop that is already asked for ends the wait and abandons the context.
    std::vector<uint8_t> tiny(static_cast<size_t>(ImageShrink::Bytes({ 8, 8 }, 1)), 7);
    ID3D12Resource* stopped = upload.Upload(8, 8, 1, tiny.data(), tiny.size(), [] { return true; }, failure);
    if (stopped)
        stopped->Release();   // the copy can win the race with the first poll
    ID3D12Resource* after = upload.Upload(8, 8, 1, tiny.data(), tiny.size(), [] { return false; }, failure);
    CHECK((stopped == nullptr) == (after == nullptr));   // abandoned only when the stop was seen
    if (after)
        after->Release();

    // A size that does not match the bytes is refused.
    ImageUpload::Gpu other;
    CHECK(other.Create(c.device.Get(), failure));
    CHECK(other.Upload(8, 8, 1, tiny.data(), tiny.size() - 1, [] { return false; }, failure) == nullptr);
    other.Release();
    upload.Release();
}

// The set-up as the loader does it: the shrink pass and the readback buffers are made on another
// thread while the asker sees Busy, and the objects work when the thread has ended.
static void RunSetupThreadChecks(Context& c, ID3D12QueryHeap* queries, ID3D12Resource* queryReadback, const Case& test)
{
    ImageShrink::Setup setup;
    uint32_t generation = 0;
    CHECK(setup.Get() == ImageShrink::Setup::Answer::Busy);
    CHECK(setup.Begin(generation));

    ImageShrink::Gpu made;
    ImageReadback::Ring madeRing;
    bool ok = false;
    std::thread worker([&]
    {
        ImageShrink::Failure failure;
        ok = made.Create(c.device.Get(), 1, failure) && madeRing.Create(c.device.Get(), kStoredSize);
        setup.End(generation, ok);
    });
    while (setup.Get() == ImageShrink::Setup::Answer::Busy)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    worker.join();

    CHECK(ok);
    CHECK(setup.Get() == ImageShrink::Setup::Answer::Ready);
    CHECK(made.Ready());
    CHECK(madeRing.Ready());
    ImageShrink::Gpu gpu = std::move(made);
    ImageReadback::Ring ring = std::move(madeRing);
    CHECK(!made.Ready());
    RunCase(c, gpu, ring, queries, queryReadback, test);
    ring.Release();
    gpu.Release();
}

static int RunChecks(Context& c)
{
    ImageShrink::Gpu gpu;
    ImageShrink::Failure failure;
    if (!gpu.Create(c.device.Get(), 1, failure))
        throw std::runtime_error(std::string("Gpu::Create failed at ") + failure.step);

    D3D12_QUERY_HEAP_DESC queryDesc = {};
    queryDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = 2;
    ComPtr<ID3D12QueryHeap> queries;
    OK(c.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)));
    ComPtr<ID3D12Resource> queryReadback = MakeBuffer(c, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    const DXGI_FORMAT bc3 = DXGI_FORMAT_BC3_UNORM, bgra = DXGI_FORMAT_B8G8R8A8_UNORM, half = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const Case cases[] = {
        { "BC3 512, no smaller levels",        bc3,  512,  512,  false, Pattern::Split  },
        { "BC3 256, no smaller levels",        bc3,  256,  256,  false, Pattern::Split  },
        { "BC3 1024x512, with smaller levels", bc3,  1024, 512,  true,  Pattern::Split  },
        { "BC3 128, no smaller levels",        bc3,  128,  128,  false, Pattern::Split  },
        { "BC3 64, with smaller levels",       bc3,  64,   64,   true,  Pattern::Split  },
        { "BGRA 841x1188, no smaller levels",  bgra, 841,  1188, false, Pattern::Split  },
        { "BGRA 512, with smaller levels",     bgra, 512,  512,  true,  Pattern::Split  },
        { "BGRA 1024, no smaller levels",      bgra, 1024, 1024, false, Pattern::Split  },
        { "BGRA 300x200, no smaller levels",   bgra, 300,  200,  false, Pattern::Split  },
        { "BGRA 32, no smaller levels",        bgra, 32,   32,   false, Pattern::Split  },
        { "RGBA16F 512, no smaller levels",    half, 512,  512,  false, Pattern::Split  },
        { "BGRA 512, transparent white half",  bgra, 512,  512,  false, Pattern::Premul },
    };

    ImageReadback::Ring ring;
    if (!ring.Create(c.device.Get(), kStoredSize))
        throw std::runtime_error("Ring::Create failed");

    printf("adapter: %s\n", c.adapter.c_str());
    printf("%-34s %-10s %-9s    %-11s %-9s %-5s  GPU time per shrink (%d timed, 1 warm-up)\n",
        "case", "source", "picked", "stored", "levels", "", kTimedRuns);
    for (const Case& test : cases)
        RunCase(c, gpu, ring, queries.Get(), queryReadback.Get(), test);

    RunRingChecks(c, gpu, ring);
    RunSrgbChecks(c, gpu);
    RunUploadChecks(c);
    RunSetupThreadChecks(c, queries.Get(), queryReadback.Get(), cases[0]);

    ring.Release();
    gpu.Release();
    return 0;
}

// ---------------------------------------------------------------------------------------
// The look-check sheet

static const char* const kDisplayShader = R"(
Texture2D<float4> image : register(t0);
SamplerState linearClamp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
	VSOut o;
	o.uv  = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float4 ps(VSOut i) : SV_Target { return image.Sample(linearClamp, i.uv); }
)";

// Draws a texture with every level into a rectangle, blended over what is there, with the
// same sampler the UI uses (linear, with levels).
class Display
{
public:
    void Create(Context& c, uint32_t textures)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors                    = 1;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges   = &range;
        param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters     = 1;
        rootDesc.pParameters       = &param;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers   = &sampler;

        ComPtr<ID3DBlob> rootBlob, errors, vs, ps;
        OK(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors));
        OK(c.device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&m_root)));
        OK(D3DCompile(kDisplayShader, strlen(kDisplayShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors));
        OK(D3DCompile(kDisplayShader, strlen(kDisplayShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_root.Get();
        pso.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS             = { ps->GetBufferPointer(), ps->GetBufferSize() };
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
        blend.BlendEnable           = TRUE;
        blend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp               = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha         = D3D12_BLEND_ONE;
        blend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask                      = UINT_MAX;
        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets                = 1;
        pso.RTVFormats[0]                   = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.SampleDesc.Count                = 1;
        OK(c.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline)));

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = textures;
        heap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        OK(c.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)));
        m_increment = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_device    = c.device;
    }

    // Makes the view of a texture that Draw reads; `index` is the descriptor it takes.
    void View(uint32_t index, ID3D12Resource* texture)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format                  = texture->GetDesc().Format;
        srv.Texture2D.MipLevels     = texture->GetDesc().MipLevels;
        D3D12_CPU_DESCRIPTOR_HANDLE at = m_heap->GetCPUDescriptorHandleForHeapStart();
        at.ptr += static_cast<SIZE_T>(index) * m_increment;
        m_device->CreateShaderResourceView(texture, &srv, at);
    }

    void Begin(ID3D12GraphicsCommandList* list)
    {
        ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(m_root.Get());
        list->SetPipelineState(m_pipeline.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void Draw(ID3D12GraphicsCommandList* list, uint32_t index, int x, int y, int width, int height)
    {
        const D3D12_VIEWPORT viewport = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        const D3D12_RECT scissor = { x, y, x + width, y + height };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        D3D12_GPU_DESCRIPTOR_HANDLE at = m_heap->GetGPUDescriptorHandleForHeapStart();
        at.ptr += static_cast<UINT64>(index) * m_increment;
        list->SetGraphicsRootDescriptorTable(0, at);
        list->DrawInstanced(3, 1, 0, 0);
    }

private:
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12RootSignature>       m_root;
    ComPtr<ID3D12PipelineState>       m_pipeline;
    ComPtr<ID3D12DescriptorHeap>      m_heap;
    UINT                              m_increment = 0;
};

struct Picture
{
    std::string          name;
    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba;
};

static bool LoadPng(IWICImagingFactory* factory, const char* path, Picture& out)
{
    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wide, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    OK(decoder->GetFrame(0, &frame));
    OK(factory->CreateFormatConverter(&converter));
    OK(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    converter->GetSize(&out.width, &out.height);
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    OK(converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));

    std::string stem = path;
    stem = stem.substr(stem.find_last_of("\\/") + 1);
    out.name = stem.substr(0, stem.rfind('.'));
    return true;
}

static void SavePng(IWICImagingFactory* factory, const char* path, const uint8_t* bgra, uint32_t width, uint32_t height)
{
    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    OK(factory->CreateStream(&stream));
    OK(stream->InitializeFromFilename(wide, GENERIC_WRITE));
    OK(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    OK(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    OK(encoder->CreateNewFrame(&frame, nullptr));
    OK(frame->Initialize(nullptr));
    OK(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    OK(frame->SetPixelFormat(&format));
    OK(frame->WritePixels(height, width * 4, width * height * 4, const_cast<BYTE*>(bgra)));
    OK(frame->Commit());
    OK(encoder->Commit());
}

static void Fill(std::vector<uint8_t>& bgra, uint32_t width, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t y = y0; y < y0 + h; ++y)
        for (uint32_t x = x0; x < x0 + w; ++x)
        {
            uint8_t* p = &bgra[(static_cast<size_t>(y) * width + x) * 4];
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        }
}

static int RunSheet(Context& c, const char* outPath, const std::vector<std::string>& paths)
{
    ComPtr<IWICImagingFactory> wic;
    OK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    std::vector<Picture> pictures;
    for (const std::string& path : paths)
    {
        Picture picture;
        if (!LoadPng(wic.Get(), path.c_str(), picture))
        {
            printf("cannot read %s\n", path.c_str());
            return 1;
        }
        pictures.push_back(std::move(picture));
    }

    ImageShrink::Gpu gpu;
    ImageShrink::Failure failure;
    if (!gpu.Create(c.device.Get(), 1, failure))
        throw std::runtime_error(std::string("Gpu::Create failed at ") + failure.step);

    // Three stored copies of each picture: its own size, 256 and 128. All are drawn by the
    // shrink pass from the picture as one level, like the temporary copy the loader makes.
    constexpr uint32_t kCopies = 3;
    const uint32_t copySizes[kCopies] = { 0, 256, 128 };
    const uint32_t count = static_cast<uint32_t>(pictures.size());
    std::vector<ComPtr<ID3D12Resource>> stored;
    for (const Picture& picture : pictures)
    {
        ComPtr<ID3D12Resource> source = MakeTexture(c, DXGI_FORMAT_R8G8B8A8_UNORM, picture.width, picture.height, 1,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        Upload(c, source.Get(), { picture.rgba });
        for (uint32_t copy = 0; copy < kCopies; ++copy)
        {
            const Size target = copySizes[copy] == 0 ? Size{ picture.width, picture.height }
                                                     : ImageShrink::StoredSize(picture.width, picture.height, copySizes[copy]);
            c.Begin();
            ID3D12Resource* made = gpu.Record(c.list.Get(), 0, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, target, failure);
            if (!made)
                throw std::runtime_error(std::string("Record failed at ") + failure.step);
            c.Submit();
            stored.emplace_back().Attach(made);
        }
    }

    // One render target holds every cell at 1:1 pixels; the sheet is it enlarged.
    constexpr int kSizes[2] = { 36, 68 };
    constexpr int kCell = 68, kGap = 8, kPitch = kCell + kGap;
    const int cols = static_cast<int>(kCopies) * 2;
    const int targetWidth = cols * kPitch, targetHeight = static_cast<int>(count) * kPitch;
    const float backdrop[4] = { 31 / 255.0f, 31 / 255.0f, 36 / 255.0f, 1.0f };
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    memcpy(clear.Color, backdrop, sizeof(backdrop));
    ComPtr<ID3D12Resource> canvas = MakeTexture(c, DXGI_FORMAT_R8G8B8A8_UNORM, targetWidth, targetHeight, 1,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    OK(c.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    c.device->CreateRenderTargetView(canvas.Get(), nullptr, rtv);

    Display display;
    display.Create(c, count * kCopies);
    for (uint32_t i = 0; i < count * kCopies; ++i)
        display.View(i, stored[i].Get());

    c.Begin();
    c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    c.list->ClearRenderTargetView(rtv, backdrop, 0, nullptr);
    display.Begin(c.list.Get());
    for (uint32_t row = 0; row < count; ++row)
        for (uint32_t copy = 0; copy < kCopies; ++copy)
            for (int s = 0; s < 2; ++s)
            {
                // The picture fits a square of the UI size, keeping its proportions.
                const Picture& picture = pictures[row];
                const int box = kSizes[s];
                const int longer = static_cast<int>(std::max(picture.width, picture.height));
                const int w = std::max(1, static_cast<int>(std::lround(static_cast<double>(box) * picture.width / longer)));
                const int h = std::max(1, static_cast<int>(std::lround(static_cast<double>(box) * picture.height / longer)));
                const int cellX = (static_cast<int>(copy) * 2 + s) * kPitch + kGap / 2;
                const int cellY = static_cast<int>(row) * kPitch + kGap / 2;
                display.Draw(c.list.Get(), row * kCopies + copy, cellX + (kCell - w) / 2, cellY + (kCell - h) / 2, w, h);
            }
    Transition(c.list.Get(), canvas.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    c.Submit();
    const std::vector<Pixels> canvasPixels = Readback(c, canvas.Get());

    // Compose: the canvas enlarged 3x without smoothing, with the labels around it.
    constexpr int kZoom = 3, kLeft = 330, kTop = 76;
    const uint32_t sheetWidth  = kLeft + targetWidth * kZoom + 16;
    const uint32_t sheetHeight = kTop + targetHeight * kZoom + 40;
    std::vector<uint8_t> sheet(static_cast<size_t>(sheetWidth) * sheetHeight * 4);
    Fill(sheet, sheetWidth, 0, 0, sheetWidth, sheetHeight, 31, 31, 36);
    const Pixels& canvasTop = canvasPixels[0];
    for (int y = 0; y < targetHeight * kZoom; ++y)
        for (int x = 0; x < targetWidth * kZoom; ++x)
        {
            const uint8_t* from = canvasTop.At(x / kZoom, y / kZoom);
            uint8_t* to = &sheet[(static_cast<size_t>(kTop + y) * sheetWidth + kLeft + x) * 4];
            to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = 255;
        }

    BITMAPINFO info = {};
    info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth       = static_cast<LONG>(sheetWidth);
    info.bmiHeader.biHeight      = -static_cast<LONG>(sheetHeight);
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, dib);
    memcpy(bits, sheet.data(), sheet.size());
    HFONT font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 220, 225));
    const auto text = [&](int x, int y, const std::string& value) { TextOutA(dc, x, y, value.c_str(), static_cast<int>(value.size())); };

    const char* const groupNames[kCopies] = { "full size", "256 px stored", "128 px stored" };
    for (uint32_t copy = 0; copy < kCopies; ++copy)
    {
        const int x = kLeft + static_cast<int>(copy) * 2 * kPitch * kZoom;
        text(x, 8, groupNames[copy]);
        text(x, 30, "at 36");
        text(x + kPitch * kZoom, 30, "at 68");
    }
    for (uint32_t row = 0; row < count; ++row)
    {
        const int y = kTop + static_cast<int>(row) * kPitch * kZoom + (kPitch * kZoom) / 2 - 18;
        text(8, y, pictures[row].name);
        text(8, y + 20, std::to_string(pictures[row].width) + " x " + std::to_string(pictures[row].height));
    }
    text(8, static_cast<int>(sheetHeight) - 28, "Drawn 1 pixel per point (UI scale 1.0) with the UI's linear sampler and levels, then enlarged 3x without smoothing.");
    GdiFlush();
    memcpy(sheet.data(), bits, sheet.size());
    for (size_t i = 3; i < sheet.size(); i += 4)
        sheet[i] = 255;
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(dib);
    DeleteDC(dc);

    SavePng(wic.Get(), outPath, sheet.data(), sheetWidth, sheetHeight);
    printf("sheet written: %u x %u, %u images\n", sheetWidth, sheetHeight, count);
    gpu.Release();
    return 0;
}

int main(int argc, char** argv)
{
    int result = 0;
    int debugErrors = 0;
    try
    {
        OK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        Context c;
        Create(c);

        if (argc >= 3 && strcmp(argv[1], "/sheet") == 0)
            result = RunSheet(c, argv[2], std::vector<std::string>(argv + 3, argv + argc));
        else
            result = RunChecks(c);

        debugErrors = CountDebugErrors(c);
        if (debugErrors < 0)
            printf("\n%d checks, %d failed, debug-layer errors not measured\n", g_checks, g_failed);
        else
            printf("\n%d checks, %d failed, %d debug-layer errors\n", g_checks, g_failed, debugErrors);
    }
    catch (const std::exception& e)
    {
        printf("harness stopped: %s\n", e.what());
        return 2;
    }
    return result == 0 && g_failed == 0 && debugErrors == 0 ? 0 : 1;   // not measured counts as not passed
}
