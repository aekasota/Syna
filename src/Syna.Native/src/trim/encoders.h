#pragma once

#include "common/ffmpeg.h"

#include <string>
#include <vector>

namespace syna {

struct EncoderInfo {
    std::string name;      // FFmpeg encoder name, e.g. "hevc_nvenc"
    std::string options;   // private options applied before opening, "key=value:key=value"
    std::string label;     // e.g. "NVIDIA NVENC (HEVC)"
    bool hardware = false; // GPU encoder (NVENC, AMF, QSV, Media Foundation hardware MFT)
};

// Encoder candidates for codec_id in preference order: NVENC, AMF, QSV, GPU via Media Foundation,
// CPU. Not probed: callers check encoder_available() and stop at the first one that works, so a
// machine with NVENC never pays for probing the CPU encoders.
std::vector<EncoderInfo> encoder_candidates(AVCodecID codec_id, bool allow_hardware);

// True when the encoder opens on this machine (probed once per process, cached).
bool encoder_available(const EncoderInfo& e);

void apply_encoder_options(AVCodecContext* ctx, const EncoderInfo& e);

// Puts the encoder in its lossless mode before opening. False when it has none: NVENC (H.264 in the
// High 4:4:4 Predictive profile, HEVC in Main/Main10) and libvpx VP9 do; AV1 NVENC is refused by the
// driver on current GPUs, which the probe below reports.
bool configure_lossless(AVCodecContext* ctx, const EncoderInfo& e);

// True when the encoder opens in lossless mode on this machine (probed once per process, cached).
bool encoder_supports_lossless(const EncoderInfo& e);

std::vector<std::string> available_hardware_encoders();

struct AvailableEncoder {
    AVCodecID codec = AV_CODEC_ID_NONE;
    EncoderInfo info;
    bool lossless = false;
};

// Every video encoder that opens on this machine, per codec in preference order (probes them all).
std::vector<AvailableEncoder> available_video_encoders();

}  // namespace syna
