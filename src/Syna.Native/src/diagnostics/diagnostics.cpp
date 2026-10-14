#include "diagnostics/diagnostics.h"

#include "common/ffmpeg.h"
#include "common/json.h"
#include "common/log.h"
#include "renderer/renderer.h"
#include "trim/encoders.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <mutex>
#include <vector>

namespace syna {
namespace {

struct ProfileName {
    GUID guid;
    const char* name;
};

const ProfileName kProfiles[] = {
    {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, "H.264"},
    {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, "HEVC Main"},
    {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, "HEVC Main 10"},
    {D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, "VP9"},
    {D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2, "VP9 Profile 2 (10-bit)"},
    {D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, "AV1 Main"},
};

class StatsSampler {
public:
    syna_result sample(syna_process_stats& out) {
        std::lock_guard lock(m_);
        if (!init_attempted_) init();
        out = {};
        FILETIME create, exit, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
        const int64_t cpu = to_100ns(kernel) + to_100ns(user);
        const int64_t now = qpc_now();
        if (last_qpc_ != 0) {
            const double wall_100ns = static_cast<double>(now - last_qpc_) * 1e7 / qpc_frequency();
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            if (wall_100ns > 0) out.cpu_percent = (cpu - last_cpu_) / wall_100ns / si.dwNumberOfProcessors * 100.0;
        }
        last_cpu_ = cpu;
        last_qpc_ = now;

        PROCESS_MEMORY_COUNTERS_EX mem = {};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
            out.private_bytes = static_cast<int64_t>(mem.PrivateUsage);
            out.working_set_bytes = static_cast<int64_t>(mem.WorkingSetSize);
        }
        if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
            out.gpu_decode_percent = sum(decode_);
            out.gpu_3d_percent = sum(three_d_);
            out.gpu_encode_percent = sum(encode_);
            out.gpu_dedicated_bytes = static_cast<int64_t>(sum(dedicated_));
        }
        return SYNA_OK;
    }

private:
    static int64_t to_100ns(const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    void init() {
        init_attempted_ = true;
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        const DWORD pid = GetCurrentProcessId();
        auto add = [&](const std::wstring& path, PDH_HCOUNTER& c) {
            if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &c) != ERROR_SUCCESS) c = nullptr;
        };
        const std::wstring prefix = L"\\GPU Engine(pid_" + std::to_wstring(pid) + L"*engtype_";
        add(prefix + L"VideoDecode)\\Utilization Percentage", decode_);
        add(prefix + L"3D)\\Utilization Percentage", three_d_);
        add(prefix + L"VideoEncode)\\Utilization Percentage", encode_);
        add(L"\\GPU Process Memory(pid_" + std::to_wstring(pid) + L"*)\\Dedicated Usage", dedicated_);
        PdhCollectQueryData(query_);
    }

    double sum(PDH_HCOUNTER c) {
        if (!c) return 0;
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA) return 0;
        std::vector<uint8_t> buf(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) return 0;
        double total = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS) total += items[i].FmtValue.doubleValue;
        }
        return total;
    }

    std::mutex m_;
    bool init_attempted_ = false;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER decode_ = nullptr, three_d_ = nullptr, encode_ = nullptr, dedicated_ = nullptr;
    int64_t last_cpu_ = 0;
    int64_t last_qpc_ = 0;
};

StatsSampler& sampler() {
    static StatsSampler s;
    return s;
}

}  // namespace

std::string system_info_json() {
    JsonWriter w;
    w.begin_object();
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &device, &fl, nullptr);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIDevice> dxgi;
        device.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter;
        dxgi->GetAdapter(&adapter);
        DXGI_ADAPTER_DESC d;
        adapter->GetDesc(&d);
        w.field("adapter", to_utf8(d.Description));
        w.field("vendor", vendor_name(d.VendorId));
        w.field("dedicatedVideoMemory", static_cast<int64_t>(d.DedicatedVideoMemory));
        w.key("hardwareDecoders").begin_array();
        ComPtr<ID3D11VideoDevice> video;
        if (SUCCEEDED(device.As(&video))) {
            const UINT n = video->GetVideoDecoderProfileCount();
            for (const ProfileName& p : kProfiles) {
                for (UINT i = 0; i < n; ++i) {
                    GUID g;
                    if (SUCCEEDED(video->GetVideoDecoderProfile(i, &g)) && IsEqualGUID(g, p.guid)) {
                        w.value(p.name);
                        break;
                    }
                }
            }
        }
        w.end_array();
    } else {
        w.field("adapter", "");
        w.field("vendor", "");
        w.field("dedicatedVideoMemory", static_cast<int64_t>(0));
        w.key("hardwareDecoders").begin_array().end_array();
    }
    w.key("hardwareEncoders").begin_array();
    for (const std::string& e : available_hardware_encoders()) w.value(e);
    w.end_array();
    // What the trim sheet can offer: per codec, the encoders that open here, in the order trims use them.
    w.key("videoEncoders").begin_array();
    for (const AvailableEncoder& e : available_video_encoders()) {
        w.begin_object();
        w.field("codec", avcodec_get_name(e.codec));
        w.field("name", e.info.name);
        w.field("label", e.info.label);
        w.field("hardware", e.info.hardware);
        w.field("lossless", e.lossless);
        w.end_object();
    }
    w.end_array();
    w.field("ffmpegVersion", av_version_info());
    w.field("ffmpegLicense", avcodec_license());
    w.end_object();
    return w.str();
}

syna_result sample_process_stats(syna_process_stats& out) { return sampler().sample(out); }

}  // namespace syna
