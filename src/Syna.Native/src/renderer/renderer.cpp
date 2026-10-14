#include "renderer/renderer.h"

#include "common/log.h"

#include "video_ps.h"
#include "video_vs.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <d3d10.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace syna {
namespace {

// WinUI 3 (Microsoft.UI.Xaml) flavor of ISwapChainPanelNative, from microsoft.ui.xaml.media.dxinterop.h.
struct __declspec(uuid("63aad0b8-7c24-40ff-85a8-640d944cc325")) __declspec(novtable) ISwapChainPanelNative : IUnknown {
    virtual HRESULT __stdcall SetSwapChain(IDXGISwapChain* swap_chain) = 0;
};

}  // namespace

// Matches cbuffer Params in video.hlsl (144 bytes).
struct VideoConstantsCpu {
    float rowR[4];
    float rowG[4];
    float rowB[4];
    float lumaTexSize[2];
    float uvScale[2];
    float chromaOffset[2];
    uint32_t layout;
    uint32_t transfer;
    uint32_t bicubic;
    float ditherAmp;
    float peakNits;
    float pad0;
    float gamut0[3];
    float pad1;
    float gamut1[3];
    float pad2;
    float gamut2[3];
    float pad3;
};
static_assert(sizeof(VideoConstantsCpu) == 144);

namespace {

// Builds the affine YUV -> R'G'B' transform. sample_scale converts the sampled texture value
// into a code value normalized to (2^depth - 1).
void compute_yuv_matrix(AVColorSpace cs, AVColorRange range, int depth, double sample_scale, int width, int height,
                        VideoConstantsCpu& c) {
    double kr = 0.2126, kb = 0.0722;
    switch (cs) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: kr = 0.299; kb = 0.114; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: kr = 0.2627; kb = 0.0593; break;
        case AVCOL_SPC_SMPTE240M: kr = 0.212; kb = 0.087; break;
        case AVCOL_SPC_FCC: kr = 0.30; kb = 0.11; break;
        case AVCOL_SPC_BT709: break;
        default:
            if (width < 1280 && height < 720) { kr = 0.299; kb = 0.114; }
            break;
    }
    const double kg = 1.0 - kr - kb;
    depth = std::clamp(depth, 8, 16);
    const double maxv = static_cast<double>((1 << depth) - 1);
    const double unit = static_cast<double>(1 << (depth - 8));
    double yoff, yscale, coff, cscale;
    if (range == AVCOL_RANGE_JPEG) {
        yoff = 0;
        yscale = 1;
        coff = 128.0 * unit / maxv;
        cscale = 1;
    } else {
        yoff = 16.0 * unit / maxv;
        yscale = maxv / (219.0 * unit);
        coff = 128.0 * unit / maxv;
        cscale = maxv / (224.0 * unit);
    }
    const double s = sample_scale;
    const double rv = 2.0 * (1.0 - kr) * cscale;
    const double bu = 2.0 * (1.0 - kb) * cscale;
    const double gu = 2.0 * kb * (1.0 - kb) / kg * cscale;
    const double gv = 2.0 * kr * (1.0 - kr) / kg * cscale;
    const double y0 = -yoff * yscale;
    auto set = [](float* row, double a, double b, double cc, double d) {
        row[0] = static_cast<float>(a);
        row[1] = static_cast<float>(b);
        row[2] = static_cast<float>(cc);
        row[3] = static_cast<float>(d);
    };
    set(c.rowR, yscale * s, 0.0, rv * s, y0 - rv * coff);
    set(c.rowG, yscale * s, -gu * s, -gv * s, y0 + (gu + gv) * coff);
    set(c.rowB, yscale * s, bu * s, 0.0, y0 - bu * coff);
}

void identity_matrix(VideoConstantsCpu& c) {
    float r[4] = {1, 0, 0, 0}, g[4] = {0, 1, 0, 0}, b[4] = {0, 0, 1, 0};
    std::memcpy(c.rowR, r, sizeof(r));
    std::memcpy(c.rowG, g, sizeof(g));
    std::memcpy(c.rowB, b, sizeof(b));
}

double frame_peak_nits(const AVFrame* f) {
    if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        auto* cll = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
        if (cll->MaxCLL > 0) return cll->MaxCLL;
    }
    if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
        if (md->has_luminance && md->max_luminance.num > 0) return av_q2d(md->max_luminance);
    }
    return 1000.0;
}

struct SwPlaneLayout {
    int planes = 0;        // 2 = semi-planar, 3 = planar
    int bytes = 1;
    int depth = 8;
    bool msb_aligned = false;
};

bool classify_sw_format(AVPixelFormat fmt, SwPlaneLayout& out) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    if (!d) return false;
    const uint64_t reject = AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BE |
                            AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BITSTREAM;
    if ((d->flags & reject) || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) || d->nb_components < 3) return false;
    out.depth = d->comp[0].depth;
    out.bytes = out.depth > 8 ? 2 : 1;
    if (d->comp[0].step != out.bytes) return false;
    if (d->comp[0].plane == 0 && d->comp[1].plane == 1 && d->comp[2].plane == 2 && d->comp[0].shift == 0) {
        out.planes = 3;
        out.msb_aligned = false;
        return d->comp[1].step == out.bytes && d->comp[2].step == out.bytes;
    }
    if (d->comp[0].plane == 0 && d->comp[1].plane == 1 && d->comp[2].plane == 1 && d->comp[1].offset < d->comp[2].offset) {
        out.planes = 2;
        out.msb_aligned = d->comp[0].shift > 0;
        return true;
    }
    return false;
}

double sample_scale(int bytes, int depth, bool msb_aligned) {
    if (bytes == 1 || depth >= 16) return 1.0;
    const double maxv = static_cast<double>((1 << depth) - 1);
    if (msb_aligned) return 65535.0 / (maxv * static_cast<double>(1 << (16 - depth)));
    return 65535.0 / maxv;
}

void chroma_offset(const AVFrame* f, int log2_w, int log2_h, int tex_w, int tex_h, VideoConstantsCpu& c) {
    c.chromaOffset[0] = 0;
    c.chromaOffset[1] = 0;
    AVChromaLocation loc = f->chroma_location;
    if (loc == AVCHROMA_LOC_UNSPECIFIED) loc = (log2_w || log2_h) ? AVCHROMA_LOC_LEFT : AVCHROMA_LOC_CENTER;
    if (log2_w && (loc == AVCHROMA_LOC_LEFT || loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_BOTTOMLEFT))
        c.chromaOffset[0] = 0.5f / static_cast<float>(tex_w);
    if (log2_h && loc == AVCHROMA_LOC_TOPLEFT) c.chromaOffset[1] = 0.5f / static_cast<float>(tex_h);
    if (log2_h && loc == AVCHROMA_LOC_BOTTOMLEFT) c.chromaOffset[1] = -0.5f / static_cast<float>(tex_h);
}

ComPtr<ID3D11ShaderResourceView> make_array_srv(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT fmt, UINT slice) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MostDetailedMip = 0;
    d.Texture2DArray.MipLevels = 1;
    d.Texture2DArray.FirstArraySlice = slice;
    d.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dev->CreateShaderResourceView(tex, &d, &srv))) return nullptr;
    return srv;
}

}  // namespace

const char* vendor_name(uint32_t vendor_id) {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002:
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        case 0x5143: return "Qualcomm";
        default: return "Unknown";
    }
}

Renderer::~Renderer() {
    hw_views_.clear();
    if (sws_) sws_freeContext(sws_);
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
}

bool Renderer::init(std::string* error) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (error) *error = "CreateDXGIFactory1 failed";
        return false;
    }
    // The adapter that owns the primary display: presenting from it avoids cross-adapter copies in DWM.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        adapter.Reset();
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        log::warn("renderer", "device with video support failed hr=0x{:08X}; retrying without", static_cast<uint32_t>(hr));
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    }
    if (FAILED(hr)) {
        if (error) *error = std::format("D3D11CreateDevice failed (0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    ComPtr<IDXGIAdapter> used_adapter;
    dxgi_device->GetAdapter(&used_adapter);
    DXGI_ADAPTER_DESC ad;
    used_adapter->GetDesc(&ad);
    adapter_.name = to_utf8(ad.Description);
    adapter_.vendor_id = ad.VendorId;
    adapter_.device_id = ad.DeviceId;
    adapter_.dedicated_video_memory = static_cast<int64_t>(ad.DedicatedVideoMemory);
    adapter_.luid = ad.AdapterLuid;

    ComPtr<IDXGIFactory2> factory2;
    used_adapter->GetParent(IID_PPV_ARGS(&factory2));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 64;
    desc.Height = 64;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr = factory2->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swap_chain_);
    if (FAILED(hr)) {
        if (error) *error = std::format("CreateSwapChainForComposition failed (0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    ComPtr<IDXGISwapChain2> sc2;
    swap_chain_.As(&sc2);
    sc2->SetMaximumFrameLatency(1);
    waitable_.reset(sc2->GetFrameLatencyWaitableObject());
    width_ = 64;
    height_ = 64;

    device_->CreateVertexShader(g_video_vs, sizeof(g_video_vs), nullptr, &vs_);
    device_->CreatePixelShader(g_video_ps, sizeof(g_video_ps), nullptr, &ps_);
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(VideoConstantsCpu);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cb, nullptr, &constants_);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &raster_);
    if (!vs_ || !ps_ || !constants_ || !sampler_ || !raster_) {
        if (error) *error = "failed to create renderer pipeline state";
        return false;
    }
    if (!create_size_dependent()) {
        if (error) *error = "failed to create render target";
        return false;
    }
    log::info("renderer", "device ready adapter=\"{}\" vendor={} vram_mb={} video_support={}", adapter_.name,
              vendor_name(adapter_.vendor_id), adapter_.dedicated_video_memory >> 20,
              (flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) != 0);
    return true;
}

HRESULT Renderer::attach_panel(IUnknown* panel) {
    if (!panel) return E_INVALIDARG;
    ComPtr<ISwapChainPanelNative> native;
    HRESULT hr = panel->QueryInterface(__uuidof(ISwapChainPanelNative), reinterpret_cast<void**>(native.GetAddressOf()));
    if (FAILED(hr)) return hr;
    return native->SetSwapChain(swap_chain_.Get());
}

bool Renderer::create_size_dependent() {
    rtv_.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
}

bool Renderer::apply_size(int width, int height, float scale_x, float scale_y) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (width == width_ && height == height_ && scale_x == scale_x_ && scale_y == scale_y_) return false;
    if (width != width_ || height != height_) {
        rtv_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->Flush();
        HRESULT hr = swap_chain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN,
                                                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (!check_device(hr)) return false;
        if (FAILED(hr)) {
            log::error("renderer", "ResizeBuffers {}x{} failed hr=0x{:08X}", width, height, static_cast<uint32_t>(hr));
            return false;
        }
        width_ = width;
        height_ = height;
        create_size_dependent();
    }
    scale_x_ = scale_x > 0 ? scale_x : 1.0f;
    scale_y_ = scale_y > 0 ? scale_y : 1.0f;
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swap_chain_.As(&sc2))) {
        DXGI_MATRIX_3X2_F m = {};
        m._11 = 1.0f / scale_x_;
        m._22 = 1.0f / scale_y_;
        sc2->SetMatrixTransform(&m);
    }
    return true;
}

void Renderer::set_background(float r, float g, float b) {
    background_[0] = r;
    background_[1] = g;
    background_[2] = b;
}

bool Renderer::check_device(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HRESULT reason = device_->GetDeviceRemovedReason();
        log::error("renderer", "device lost hr=0x{:08X} reason=0x{:08X}", static_cast<uint32_t>(hr), static_cast<uint32_t>(reason));
        device_lost_ = true;
        return false;
    }
    return true;
}

void Renderer::release_frame_resources() {
    hw_views_.clear();
    copy_views_ = {};
    copy_tex_.Reset();
}

bool Renderer::prepare_hw_frame(const AVFrame* frame, PlaneViews& views, DXGI_FORMAT& fmt, int& tex_w, int& tex_h) {
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    fmt = desc.Format;
    DXGI_FORMAT luma, chroma;
    switch (desc.Format) {
        case DXGI_FORMAT_NV12: luma = DXGI_FORMAT_R8_UNORM; chroma = DXGI_FORMAT_R8G8_UNORM; break;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016: luma = DXGI_FORMAT_R16_UNORM; chroma = DXGI_FORMAT_R16G16_UNORM; break;
        default: return false;
    }
    tex_w = static_cast<int>(desc.Width);
    tex_h = static_cast<int>(desc.Height);

    if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        auto key = std::make_pair(tex, index);
        auto it = hw_views_.find(key);
        if (it == hw_views_.end()) {
            if (hw_views_.size() > 96) hw_views_.clear();
            PlaneViews v;
            v.srv[0] = make_array_srv(device_.Get(), tex, luma, index);
            v.srv[1] = make_array_srv(device_.Get(), tex, chroma, index);
            v.count = 2;
            if (!v.srv[0] || !v.srv[1]) return false;
            it = hw_views_.emplace(key, std::move(v)).first;
        }
        views = it->second;
        return true;
    }

    // Decoder pool is not shader-readable on this driver: one GPU-side copy per frame.
    D3D11_TEXTURE2D_DESC cd = {};
    if (copy_tex_) copy_tex_->GetDesc(&cd);
    if (!copy_tex_ || cd.Width != desc.Width || cd.Height != desc.Height || cd.Format != desc.Format) {
        copy_views_ = {};
        copy_tex_.Reset();
        D3D11_TEXTURE2D_DESC nd = {};
        nd.Width = desc.Width;
        nd.Height = desc.Height;
        nd.MipLevels = 1;
        nd.ArraySize = 1;
        nd.Format = desc.Format;
        nd.SampleDesc.Count = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&nd, nullptr, &copy_tex_))) return false;
        copy_views_.srv[0] = make_array_srv(device_.Get(), copy_tex_.Get(), luma, 0);
        copy_views_.srv[1] = make_array_srv(device_.Get(), copy_tex_.Get(), chroma, 0);
        copy_views_.count = 2;
        log::info("renderer", "decoder surfaces are not shader-readable; using GPU copy path");
    }
    context_->CopySubresourceRegion(copy_tex_.Get(), 0, 0, 0, 0, tex, D3D11CalcSubresource(0, index, desc.MipLevels), nullptr);
    views = copy_views_;
    return true;
}

bool Renderer::prepare_sw_frame(const AVFrame* frame, PlaneViews& views, int& layout, int& sw_format, int& tex_w, int& tex_h) {
    const AVFrame* src = frame;
    SwPlaneLayout pl;
    bool direct = classify_sw_format(static_cast<AVPixelFormat>(frame->format), pl);
    if (!direct) {
        // Rare formats (RGB, gray, palettes, ...) are converted to BGRA on the CPU.
        sws_ = sws_getCachedContext(sws_, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                    frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        if (!sws_frame_) sws_frame_ = make_frame();
        if (sws_frame_->width != frame->width || sws_frame_->height != frame->height) {
            av_frame_unref(sws_frame_.get());
            sws_frame_->format = AV_PIX_FMT_BGRA;
            sws_frame_->width = frame->width;
            sws_frame_->height = frame->height;
            if (av_frame_get_buffer(sws_frame_.get(), 0) < 0) return false;
        }
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, sws_frame_->data, sws_frame_->linesize);
        src = sws_frame_.get();
    }
    const int fmt = src->format;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fmt));
    if (sw_.format != fmt || sw_.width != src->width || sw_.height != src->height) {
        sw_ = {};
        sw_.format = fmt;
        sw_.width = src->width;
        sw_.height = src->height;
        DXGI_FORMAT formats[3];
        int widths[3], heights[3];
        if (!direct) {
            sw_.planes = 1;
            formats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
            widths[0] = src->width;
            heights[0] = src->height;
        } else {
            const int cw = AV_CEIL_RSHIFT(src->width, d->log2_chroma_w);
            const int ch = AV_CEIL_RSHIFT(src->height, d->log2_chroma_h);
            sw_.planes = pl.planes;
            formats[0] = pl.bytes == 1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
            widths[0] = src->width;
            heights[0] = src->height;
            if (pl.planes == 2) {
                formats[1] = pl.bytes == 1 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
                widths[1] = cw;
                heights[1] = ch;
            } else {
                formats[1] = formats[2] = formats[0];
                widths[1] = widths[2] = cw;
                heights[1] = heights[2] = ch;
            }
        }
        for (int i = 0; i < sw_.planes; ++i) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = static_cast<UINT>(widths[i]);
            td.Height = static_cast<UINT>(heights[i]);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = formats[i];
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &sw_.tex[i]))) return false;
            sw_.srv[i] = make_array_srv(device_.Get(), sw_.tex[i].Get(), formats[i], 0);
        }
    }
    for (int i = 0; i < sw_.planes; ++i) {
        D3D11_TEXTURE2D_DESC td;
        sw_.tex[i]->GetDesc(&td);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(context_->Map(sw_.tex[i].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        int bpp = 4;
        switch (td.Format) {
            case DXGI_FORMAT_R8_UNORM: bpp = 1; break;
            case DXGI_FORMAT_R16_UNORM:
            case DXGI_FORMAT_R8G8_UNORM: bpp = 2; break;
            default: bpp = 4; break;
        }
        const size_t row = static_cast<size_t>(td.Width) * bpp;
        for (UINT y = 0; y < td.Height; ++y) {
            std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                        src->data[i] + static_cast<ptrdiff_t>(y) * src->linesize[i], row);
        }
        context_->Unmap(sw_.tex[i].Get(), 0);
    }
    for (int i = 0; i < sw_.planes; ++i) views.srv[i] = sw_.srv[i];
    views.count = sw_.planes;
    layout = !direct ? 2 : (pl.planes == 2 ? 1 : 0);
    sw_format = fmt;
    tex_w = src->width;
    tex_h = src->height;
    return true;
}

bool Renderer::prepare_frame(const AVFrame* frame, PlaneViews& views, VideoConstantsCpu& c, int& tex_w, int& tex_h) {
    std::memset(&c, 0, sizeof(c));
    int depth = 8;
    bool msb = false;
    int bytes = 1;
    int log2_w = 1, log2_h = 1;
    AVColorRange range = frame->color_range;

    if (frame->format == AV_PIX_FMT_D3D11) {
        DXGI_FORMAT fmt;
        if (!prepare_hw_frame(frame, views, fmt, tex_w, tex_h)) return false;
        c.layout = 1;
        const AVHWFramesContext* fc = frame->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data) : nullptr;
        const AVPixFmtDescriptor* sd = fc ? av_pix_fmt_desc_get(fc->sw_format) : nullptr;
        if (fmt == DXGI_FORMAT_NV12) {
            depth = 8;
        } else {
            depth = sd ? sd->comp[0].depth : 10;
            bytes = 2;
            msb = true;
        }
        if (sd) {
            log2_w = sd->log2_chroma_w;
            log2_h = sd->log2_chroma_h;
        }
    } else {
        int layout = 0, sw_format = 0;
        if (!prepare_sw_frame(frame, views, layout, sw_format, tex_w, tex_h)) return false;
        c.layout = static_cast<uint32_t>(layout);
        if (layout != 2) {
            SwPlaneLayout pl;
            classify_sw_format(static_cast<AVPixelFormat>(sw_format), pl);
            depth = pl.depth;
            bytes = pl.bytes;
            msb = pl.msb_aligned;
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(sw_format));
            log2_w = d->log2_chroma_w;
            log2_h = d->log2_chroma_h;
            if (sw_format == AV_PIX_FMT_YUVJ420P || sw_format == AV_PIX_FMT_YUVJ422P || sw_format == AV_PIX_FMT_YUVJ444P)
                range = AVCOL_RANGE_JPEG;
        }
    }

    if (c.layout == 2) {
        identity_matrix(c);
    } else {
        compute_yuv_matrix(frame->colorspace, range, depth, sample_scale(bytes, depth, msb), frame->width, frame->height, c);
        chroma_offset(frame, log2_w, log2_h, tex_w, tex_h, c);
    }
    c.lumaTexSize[0] = static_cast<float>(tex_w);
    c.lumaTexSize[1] = static_cast<float>(tex_h);
    c.uvScale[0] = static_cast<float>(frame->width) / static_cast<float>(tex_w);
    c.uvScale[1] = static_cast<float>(frame->height) / static_cast<float>(tex_h);
    c.bicubic = 1;
    c.transfer = frame->color_trc == AVCOL_TRC_SMPTE2084 ? 1u : frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ? 2u : 0u;
    c.peakNits = static_cast<float>(frame_peak_nits(frame));
    c.ditherAmp = (depth > 8 || c.transfer != 0) ? 1.0f / 255.0f : 0.0f;
    const float g0[3] = {1.6605f, -0.5876f, -0.0728f}, g1[3] = {-0.1246f, 1.1329f, -0.0083f}, g2[3] = {-0.0182f, -0.1006f, 1.1187f};
    std::memcpy(c.gamut0, g0, sizeof(g0));
    std::memcpy(c.gamut1, g1, sizeof(g1));
    std::memcpy(c.gamut2, g2, sizeof(g2));
    return true;
}

void Renderer::draw(const AVFrame* frame, ID3D11RenderTargetView* rtv, int target_w, int target_h, bool letterbox) {
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, background_);
    if (!frame || frame->width <= 0 || frame->height <= 0) return;

    PlaneViews views;
    VideoConstantsCpu c;
    int tex_w = 0, tex_h = 0;
    if (!prepare_frame(frame, views, c, tex_w, tex_h)) return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &c, sizeof(c));
        context_->Unmap(constants_.Get(), 0);
    }

    D3D11_VIEWPORT vp = {};
    vp.MaxDepth = 1.0f;
    if (letterbox) {
        const double sar = frame->sample_aspect_ratio.num > 0 ? av_q2d(frame->sample_aspect_ratio) : 1.0;
        const double dar = frame->width * sar / frame->height;
        double w = target_w, h = target_w / dar;
        if (h > target_h) {
            h = target_h;
            w = target_h * dar;
        }
        w = std::round(w);
        h = std::round(h);
        vp.TopLeftX = static_cast<float>(std::floor((target_w - w) / 2.0));
        vp.TopLeftY = static_cast<float>(std::floor((target_h - h) / 2.0));
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
    } else {
        vp.Width = static_cast<float>(target_w);
        vp.Height = static_cast<float>(target_h);
    }
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {constants_.Get()};
    context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samplers);
    ID3D11ShaderResourceView* srvs[3] = {views.srv[0].Get(), views.srv[1].Get(), views.srv[2].Get()};
    context_->PSSetShaderResources(0, 3, srvs);
    context_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    context_->PSSetShaderResources(0, 3, nulls);
}

bool Renderer::present(const AVFrame* frame, double* cpu_ms) {
    if (device_lost_) return false;
    const int64_t t0 = qpc_now();
    if (!rtv_ && !create_size_dependent()) return false;
    draw(frame, rtv_.Get(), width_, height_, true);
    HRESULT hr = swap_chain_->Present(1, 0);
    if (cpu_ms) *cpu_ms = qpc_to_ms(qpc_now() - t0);
    return check_device(hr);
}

bool Renderer::render_to_bgra(const AVFrame* frame, std::vector<uint8_t>& pixels, int& width, int& height) {
    if (!frame || device_lost_) return false;
    const double sar = frame->sample_aspect_ratio.num > 0 ? av_q2d(frame->sample_aspect_ratio) : 1.0;
    width = static_cast<int>(std::lround(frame->width * sar));
    height = frame->height;
    if (width <= 0 || height <= 0) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &target))) return false;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateRenderTargetView(target.Get(), nullptr, &rtv))) return false;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) return false;

    draw(frame, rtv.Get(), width, height, false);
    context_->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4, static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                    static_cast<size_t>(width) * 4);
    }
    context_->Unmap(staging.Get(), 0);
    if (rtv_) {
        ID3D11RenderTargetView* main = rtv_.Get();
        context_->OMSetRenderTargets(1, &main, nullptr);
    }
    return true;
}

}  // namespace syna
