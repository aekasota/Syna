#pragma once

#include "common/ffmpeg.h"
#include "common/util.h"

#include <d3d11_1.h>
#include <dxgi1_3.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

struct SwsContext;

namespace syna {

struct GpuAdapterInfo {
    std::string name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    int64_t dedicated_video_memory = 0;
    LUID luid{};
};

const char* vendor_name(uint32_t vendor_id);

struct VideoConstantsCpu;

// Owns the D3D11 device and the composition swap chain. Rendering methods must be called with
// lock() held (the render thread holds it while drawing; snapshots take it from other threads).
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool init(std::string* error);

    ID3D11Device* device() const { return device_.Get(); }
    const GpuAdapterInfo& adapter() const { return adapter_; }
    std::mutex& mutex() { return mutex_; }

    // UI thread only.
    HRESULT attach_panel(IUnknown* panel);

    // Applies a pending size; returns true if the back buffer changed.
    bool apply_size(int width, int height, float scale_x, float scale_y);
    void set_background(float r, float g, float b);

    // Draws frame (or only the background when null) and presents. Returns false on device loss.
    bool present(const AVFrame* frame, double* cpu_ms);
    HANDLE frame_latency_waitable() const { return waitable_.get(); }
    bool device_lost() const { return device_lost_; }

    // Renders frame at its display resolution into tightly packed BGRA.
    bool render_to_bgra(const AVFrame* frame, std::vector<uint8_t>& pixels, int& width, int& height);

    // Release cached views into decoder surfaces (call when a decoder is destroyed).
    void release_frame_resources();

private:
    struct PlaneViews {
        ComPtr<ID3D11ShaderResourceView> srv[3];
        int count = 0;
    };
    struct SoftwareTextures {
        int format = -1;
        int width = 0;
        int height = 0;
        ComPtr<ID3D11Texture2D> tex[3];
        ComPtr<ID3D11ShaderResourceView> srv[3];
        int planes = 0;
    };

    bool create_size_dependent();
    bool prepare_frame(const AVFrame* frame, PlaneViews& views, VideoConstantsCpu& c, int& tex_w, int& tex_h);
    bool prepare_hw_frame(const AVFrame* frame, PlaneViews& views, DXGI_FORMAT& fmt, int& tex_w, int& tex_h);
    bool prepare_sw_frame(const AVFrame* frame, PlaneViews& views, int& layout, int& sw_format, int& tex_w, int& tex_h);
    void draw(const AVFrame* frame, ID3D11RenderTargetView* rtv, int target_w, int target_h, bool letterbox);
    bool check_device(HRESULT hr);

    std::mutex mutex_;
    GpuAdapterInfo adapter_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    UniqueHandle waitable_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;

    std::map<std::pair<ID3D11Texture2D*, UINT>, PlaneViews> hw_views_;
    ComPtr<ID3D11Texture2D> copy_tex_;   // used when decoder surfaces cannot be sampled directly
    PlaneViews copy_views_;
    SoftwareTextures sw_;
    ::SwsContext* sws_ = nullptr;
    FramePtr sws_frame_;

    int width_ = 1;
    int height_ = 1;
    float scale_x_ = 1.0f;
    float scale_y_ = 1.0f;
    float background_[4] = {0.043f, 0.043f, 0.047f, 1.0f};
    bool device_lost_ = false;
};

}  // namespace syna
