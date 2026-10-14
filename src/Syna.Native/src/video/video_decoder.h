#pragma once

#include "common/ffmpeg.h"

#include <d3d11.h>

#include <string>

namespace syna {

class VideoDecoder {
public:
    VideoDecoder() = default;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // device may be null (software only). extra_surfaces: frames the consumer may hold at once.
    bool open(AVStream* stream, ID3D11Device* device, bool prefer_hw, int extra_surfaces, int sw_threads, std::string* error);

    int send(const AVPacket* packet) { return avcodec_send_packet(ctx_.get(), packet); }
    int receive(AVFrame* frame) { return avcodec_receive_frame(ctx_.get(), frame); }
    void flush() { avcodec_flush_buffers(ctx_.get()); }

    AVCodecContext* context() const { return ctx_.get(); }
    bool hw_attempted() const { return hw_device_ != nullptr; }
    const std::string& hw_failure() const { return hw_failure_; }
    bool shader_bound() const { return shader_bound_; }
    int pool_size() const { return pool_size_; }
    std::string codec_name() const;

private:
    static AVPixelFormat get_format(AVCodecContext* ctx, const AVPixelFormat* formats);
    bool setup_hw_frames(AVCodecContext* ctx);
    bool create_hw_device(ID3D11Device* device);

    CodecPtr ctx_;
    BufferPtr hw_device_;
    ID3D11Device* device_ = nullptr;
    int pool_size_ = 0;
    bool shader_bound_ = false;
    std::string hw_failure_;
};

}  // namespace syna
