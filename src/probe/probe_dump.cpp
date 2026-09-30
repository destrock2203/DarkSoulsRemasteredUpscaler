// DSRProbe: PNG dumps of render targets during a frame trace. A draw's render target is copied into
// a staging texture on the context that drew it (deferred contexts included), and read back on the
// immediate context after the traced frame; PNGs are encoded on a worker thread.

#include "core/log.h"
#include "probe/probe.h"

#include <d3d11.h>
#include <wrl/client.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_OVERRIDE
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dsru::probe::dump {

namespace {

struct Capture_ {
    ComPtr<ID3D11Texture2D> staging;
    std::string label;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
    int sequence = 0;
};

struct Image {
    std::string name;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

std::mutex g_mutex;
std::vector<Capture_> g_pending;
std::atomic<int> g_sequence{0};

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? 0.0f : 65504.0f;
    else
        v = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -v : v;
}

// Unsigned small floats of R11G11B10_FLOAT: 5-bit exponent, `bits`-bit mantissa.
float SmallFloat(uint32_t value, int bits) {
    const uint32_t exponent = value >> bits, mantissa = value & ((1u << bits) - 1);
    if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - bits);
    if (exponent == 31) return 65504.0f;
    return std::ldexp(static_cast<float>(mantissa | (1u << bits)), static_cast<int>(exponent) - 15 - bits);
}

uint8_t ToByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// HDR scene color: Reinhard, then gamma.
uint8_t Tonemap(float v) { return ToByte(std::pow(std::max(v, 0.0f) / (1.0f + std::max(v, 0.0f)), 1.0f / 2.2f)); }

bool IsDepthLike(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
            return true;
        default: return false;
    }
}

// Converts one mapped texture to RGBA8. False for formats not handled.
bool Convert(const uint8_t* data, UINT pitch, UINT w, UINT h, DXGI_FORMAT f, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 255);
    auto px = [&](UINT x, UINT y) { return &out[(static_cast<size_t>(y) * w + x) * 4]; };

    if (IsDepthLike(f)) {
        // Single channel: normalized over the frame's range (reversed or not), ignoring the clear value.
        std::vector<float> values(static_cast<size_t>(w) * h);
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* row = data + static_cast<size_t>(y) * pitch;
            for (UINT x = 0; x < w; ++x) {
                float v = 0;
                switch (f) {
                    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
                        std::memcpy(&v, row + x * 8, 4);
                        break;
                    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
                    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: {
                        uint32_t u;
                        std::memcpy(&u, row + x * 4, 4);
                        v = static_cast<float>(u & 0xffffff) / 16777215.0f;
                        break;
                    }
                    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: {
                        uint16_t u;
                        std::memcpy(&u, row + x * 2, 2);
                        v = u / 65535.0f;
                        break;
                    }
                    case DXGI_FORMAT_R16_FLOAT: {
                        uint16_t u;
                        std::memcpy(&u, row + x * 2, 2);
                        v = HalfToFloat(u);
                        break;
                    }
                    default: std::memcpy(&v, row + x * 4, 4); break;
                }
                values[static_cast<size_t>(y) * w + x] = v;
            }
        }
        float lo = 1e30f, hi = -1e30f;
        for (float v : values)
            if (std::isfinite(v) && v > 0.0f && v < 1.0f) lo = std::min(lo, v), hi = std::max(hi, v);
        if (!(hi > lo)) lo = 0.0f, hi = 1.0f;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const uint8_t g = ToByte((values[static_cast<size_t>(y) * w + x] - lo) / (hi - lo));
                uint8_t* p = px(x, y);
                p[0] = p[1] = p[2] = g;
            }
        return true;
    }

    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * pitch;
        for (UINT x = 0; x < w; ++x) {
            uint8_t* p = px(x, y);
            switch (f) {
                case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    std::memcpy(p, row + x * 4, 3);
                    break;
                case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8X8_UNORM:
                    p[0] = row[x * 4 + 2], p[1] = row[x * 4 + 1], p[2] = row[x * 4 + 0];
                    break;
                case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                    uint32_t v;
                    std::memcpy(&v, row + x * 4, 4);
                    p[0] = static_cast<uint8_t>((v & 0x3ff) >> 2);
                    p[1] = static_cast<uint8_t>(((v >> 10) & 0x3ff) >> 2);
                    p[2] = static_cast<uint8_t>(((v >> 20) & 0x3ff) >> 2);
                    break;
                }
                case DXGI_FORMAT_R11G11B10_FLOAT: {
                    uint32_t v;
                    std::memcpy(&v, row + x * 4, 4);
                    p[0] = Tonemap(SmallFloat(v & 0x7ff, 6));
                    p[1] = Tonemap(SmallFloat((v >> 11) & 0x7ff, 6));
                    p[2] = Tonemap(SmallFloat((v >> 22) & 0x3ff, 5));
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
                    uint16_t v[4];
                    std::memcpy(v, row + x * 8, 8);
                    for (int c = 0; c < 3; ++c) p[c] = Tonemap(HalfToFloat(v[c]));
                    break;
                }
                case DXGI_FORMAT_R32G32B32A32_FLOAT: {
                    float v[4];
                    std::memcpy(v, row + x * 16, 16);
                    for (int c = 0; c < 3; ++c) p[c] = Tonemap(v[c]);
                    break;
                }
                case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS: {
                    // Motion: 0.5 = none; the game's "no object motion" marker (>= 1) shows as blue.
                    uint16_t v[2];
                    std::memcpy(v, row + x * 4, 4);
                    const float a = HalfToFloat(v[0]), b = HalfToFloat(v[1]);
                    if (a >= 1.0f) {
                        p[0] = 0, p[1] = 0, p[2] = 160;
                    } else {
                        p[0] = ToByte(0.5f + a * 20.0f), p[1] = ToByte(0.5f + b * 20.0f), p[2] = 0;
                    }
                    break;
                }
                case DXGI_FORMAT_R16G16_UNORM: {
                    uint16_t v[2];
                    std::memcpy(v, row + x * 4, 4);
                    p[0] = static_cast<uint8_t>(v[0] >> 8), p[1] = static_cast<uint8_t>(v[1] >> 8), p[2] = 0;
                    break;
                }
                case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_TYPELESS:
                    p[0] = row[x * 2], p[1] = row[x * 2 + 1], p[2] = 0;
                    break;
                case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: case DXGI_FORMAT_R8_TYPELESS:
                    p[0] = p[1] = p[2] = row[x];
                    break;
                default: return false;
            }
        }
    }
    return true;
}

std::string SafeName(std::string text) {
    for (char& c : text)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
    return text;
}

void WriteChunk(void* context, void* data, int size) {
    std::fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE*>(context));
}

}  // namespace

void Capture(ID3D11DeviceContext* context, ID3D11Resource* resource, std::string label) {
    if (!context || !resource) return;
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return;
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) return;  // MSAA: not copyable to staging directly

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    Capture_ c;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &c.staging))) return;
    context->CopySubresourceRegion(c.staging.Get(), 0, 0, 0, 0, resource, 0, nullptr);
    c.label = std::move(label);
    c.format = desc.Format;
    c.width = desc.Width;
    c.height = desc.Height;
    c.sequence = ++g_sequence;
    std::lock_guard lock(g_mutex);
    g_pending.push_back(std::move(c));
}

int Pending() {
    std::lock_guard lock(g_mutex);
    return static_cast<int>(g_pending.size());
}

void Flush(ID3D11DeviceContext* immediate, const std::filesystem::path& directory) {
    std::vector<Capture_> captures;
    {
        std::lock_guard lock(g_mutex);
        captures = std::move(g_pending);
        g_pending.clear();
    }
    std::vector<Image> images;
    for (Capture_& c : captures) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(immediate->Map(c.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) continue;
        Image image;
        image.width = static_cast<int>(c.width);
        image.height = static_cast<int>(c.height);
        image.name = std::format("{:03}_{}_{}.png", c.sequence, SafeName(c.label), FormatName(c.format));
        const bool ok = Convert(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, c.width, c.height, c.format,
                                image.rgba);
        immediate->Unmap(c.staging.Get(), 0);
        if (ok)
            images.push_back(std::move(image));
        else
            log::Info("Texture dump: format {} of '{}' not converted", FormatName(c.format), c.label);
    }
    captures.clear();
    if (images.empty()) return;
    std::thread([dir = directory, images = std::move(images)] {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (const Image& image : images) {
            FILE* file = _wfopen((dir / image.name).c_str(), L"wb");
            if (!file) continue;
            stbi_write_png_to_func(WriteChunk, file, image.width, image.height, 4, image.rgba.data(), image.width * 4);
            std::fclose(file);
        }
        log::Info("Texture dumps: {} PNGs in {}", images.size(), dir.string());
    }).detach();
}

}  // namespace dsru::probe::dump
