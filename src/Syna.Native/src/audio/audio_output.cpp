#include "audio/audio_output.h"

#include "common/log.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace syna {

// Receives endpoint notifications on a system thread; only flags work for the audio thread.
class AudioOutput::Notifier : public IMMNotificationClient {
public:
    explicit Notifier(AudioOutput* owner) : owner_(owner) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        if (!owner_->device_ok_.load()) signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        if (!owner_->device_ok_.load()) signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void signal() {
        owner_->device_change_pending_.store(true);
        SetEvent(owner_->device_event_.get());
    }
    LONG ref_ = 1;
    AudioOutput* owner_;
};

namespace {

DWORD default_channel_mask(int channels) {
    switch (channels) {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 4: return KSAUDIO_SPEAKER_QUAD;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return 0;
    }
}

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace

AudioOutput::AudioOutput() {
    quit_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    command_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    command_done_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    device_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    buffer_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

AudioOutput::~AudioOutput() {
    SetEvent(quit_event_.get());
    if (thread_.joinable()) thread_.join();
}

void AudioOutput::start() {
    UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    HANDLE ready_handle = ready.get();
    thread_ = std::thread([this, ready_handle] {
        set_thread_name(L"Syna audio output");
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)))) {
            notifier_ = new Notifier(this);
            enumerator_->RegisterEndpointNotificationCallback(notifier_);
            open_device();
        } else {
            log::error("audio", "MMDeviceEnumerator unavailable; using null sink");
        }
        SetEvent(ready_handle);
        thread_main();
        close_device();
        if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_);
        if (notifier_) notifier_->Release();
        notifier_ = nullptr;
        safe_release(enumerator_);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(co)) CoUninitialize();
    });
    WaitForSingleObject(ready_handle, 5000);
}

void AudioOutput::close_device() {
    if (client_) client_->Stop();
    safe_release(audio_clock_);
    safe_release(render_client_);
    safe_release(client_);
    running_.store(false);
}

bool AudioOutput::open_device() {
    const bool was_open = client_ != nullptr;
    int64_t media_now = media_time_us();
    close_device();
    device_ok_.store(false);
    if (!enumerator_) return false;

    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        log::warn("audio", "no default render endpoint (0x{:08X}); null sink active", static_cast<uint32_t>(hr));
        return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (FAILED(hr)) {
        log::error("audio", "IAudioClient activation failed 0x{:08X}", static_cast<uint32_t>(hr));
        return false;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix)) || !mix) {
        close_device();
        return false;
    }
    if (!format_fixed_) {
        stream_channels_ = std::clamp<int>(mix->nChannels, 1, 8);
        stream_rate_ = static_cast<int>(mix->nSamplesPerSec);
        format_fixed_ = true;
    }
    DWORD mask = default_channel_mask(stream_channels_);
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->nChannels == stream_channels_)
        mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    const int device_rate = static_cast<int>(mix->nSamplesPerSec);
    const int device_channels = mix->nChannels;
    CoTaskMemFree(mix);

    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = static_cast<WORD>(stream_channels_);
    wfx.Format.nSamplesPerSec = static_cast<DWORD>(stream_rate_);
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = static_cast<WORD>(4 * stream_channels_);
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = mask;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const REFERENCE_TIME duration = 400000;  // 40 ms
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (FAILED(hr)) {
        log::error("audio", "Initialize failed 0x{:08X} ({} Hz x{})", static_cast<uint32_t>(hr), stream_rate_, stream_channels_);
        close_device();
        return false;
    }
    client_->SetEventHandle(buffer_event_.get());
    client_->GetBufferSize(&buffer_frames_);
    client_->GetService(IID_PPV_ARGS(&render_client_));
    client_->GetService(IID_PPV_ARGS(&audio_clock_));
    if (!render_client_ || !audio_clock_ || FAILED(audio_clock_->GetFrequency(&clock_frequency_)) || clock_frequency_ == 0) {
        close_device();
        return false;
    }
    buffer_ms_.store(buffer_frames_ * 1000.0 / stream_rate_);
    {
        std::lock_guard lock(clock_mutex_);
        segments_.clear();
        written_frames_ = 0;
        played_frames_ = 0;
        played_qpc_ = qpc_now();
        expected_media_us_ = INT64_MIN;
        if (was_open || playing_) idle_media_us_ = media_now;
    }
    device_ok_.store(true);
    device_generation_.fetch_add(1);
    log::info("audio", "device open stream={}Hz x{} device={}Hz x{} buffer={:.1f}ms", stream_rate_, stream_channels_, device_rate,
              device_channels, buffer_ms_.load());
    if (playing_ && !null_sink_requested_) {
        fill_device();
        client_->Start();
        running_.store(true);
    }
    return true;
}

void AudioOutput::thread_main() {
    std::vector<float> scratch;
    for (;;) {
        const bool null_mode = !client_ || null_sink_requested_;
        DWORD timeout = INFINITE;
        if (playing_) timeout = null_mode ? 5 : 200;
        if (!device_ok_ && enumerator_) timeout = std::min<DWORD>(timeout, 2000);
        HANDLE handles[4] = {quit_event_.get(), command_event_.get(), device_event_.get(), buffer_event_.get()};
        DWORD r = WaitForMultipleObjects(4, handles, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;

        if (device_change_pending_.exchange(false)) {
            log::info("audio", "default device changed; reopening output");
            open_device();
        } else if (!device_ok_ && enumerator_ && qpc_now() >= next_retry_qpc_) {
            next_retry_qpc_ = qpc_now() + qpc_frequency() * 2;
            if (open_device()) log::info("audio", "output device available again");
        }
        apply_command();
        if (!playing_) continue;
        if (!client_ || null_sink_requested_) fill_null_sink();
        else fill_device();
    }
}

void AudioOutput::apply_command() {
    Cmd cmd;
    int64_t flush_us;
    {
        std::lock_guard lock(clock_mutex_);
        cmd = pending_cmd_;
        flush_us = pending_flush_us_;
        pending_cmd_ = Cmd::None;
    }
    if (cmd == Cmd::None) return;
    const bool null_mode = !client_ || null_sink_requested_;
    switch (cmd) {
        case Cmd::Play:
            if (!playing_) {
                {
                    std::lock_guard lock(clock_mutex_);
                    played_qpc_ = qpc_now();
                    null_start_qpc_ = played_qpc_;
                    null_start_frames_ = played_frames_;
                }
                playing_.store(true);
                if (!null_mode) {
                    fill_device();
                    HRESULT hr = client_->Start();
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        device_change_pending_.store(true);
                        SetEvent(device_event_.get());
                    }
                }
                running_.store(true);
            }
            break;
        case Cmd::Pause:
            if (playing_) {
                if (!null_mode) client_->Stop();
                if (null_mode) {
                    std::lock_guard lock(clock_mutex_);
                    played_frames_ = std::min(written_frames_, null_start_frames_ + (qpc_now() - null_start_qpc_) * stream_rate_ / qpc_frequency());
                    played_qpc_ = qpc_now();
                } else {
                    UINT64 pos = 0, qpc = 0;
                    if (audio_clock_ && SUCCEEDED(audio_clock_->GetPosition(&pos, &qpc))) {
                        std::lock_guard lock(clock_mutex_);
                        played_frames_ = static_cast<int64_t>(pos * static_cast<uint64_t>(stream_rate_) / clock_frequency_);
                    }
                }
                playing_.store(false);
                running_.store(false);
            }
            break;
        case Cmd::Flush:
            if (client_) {
                client_->Stop();
                client_->Reset();
            }
            playing_.store(false);
            running_.store(false);
            {
                std::lock_guard lock(clock_mutex_);
                reset_clock_locked(flush_us);
            }
            break;
        case Cmd::None: break;
    }
    SetEvent(command_done_event_.get());
}

void AudioOutput::reset_clock_locked(int64_t media_us) {
    segments_.clear();
    written_frames_ = 0;
    played_frames_ = 0;
    played_qpc_ = qpc_now();
    idle_media_us_ = media_us;
    expected_media_us_ = INT64_MIN;
}

void AudioOutput::render_from_source(float* out, int frames, AudioChunkInfo& info) {
    std::lock_guard lock(source_mutex_);
    if (source_) {
        source_->render(out, frames, info);
        return;
    }
    std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames) * stream_channels_);
    std::lock_guard clock(clock_mutex_);
    info.media_us = expected_media_us_ != INT64_MIN ? expected_media_us_ : idle_media_us_;
    info.rate = last_rate_;
}

void AudioOutput::record_chunk(int64_t frames, const AudioChunkInfo& info) {
    std::lock_guard lock(clock_mutex_);
    if (expected_media_us_ == INT64_MIN || std::llabs(info.media_us - expected_media_us_) > 2000 || info.rate != last_rate_) {
        segments_.push_back({written_frames_, info.media_us, info.rate});
    }
    expected_media_us_ = info.media_us + static_cast<int64_t>(std::llround(frames * 1e6 * info.rate / stream_rate_));
    last_rate_ = info.rate;
    written_frames_ += frames;
    while (segments_.size() > 1 && segments_[1].frame <= played_frames_) segments_.pop_front();
}

void AudioOutput::fill_device() {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            log::warn("audio", "device invalidated; reopening");
            device_change_pending_.store(true);
            SetEvent(device_event_.get());
        }
        return;
    }
    queued_ms_.store(padding * 1000.0 / stream_rate_);
    if (padding == 0 && running_.load()) starvations_.fetch_add(1);
    const UINT32 avail = buffer_frames_ - padding;
    if (avail > 0) {
        BYTE* data = nullptr;
        hr = render_client_->GetBuffer(avail, &data);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                device_change_pending_.store(true);
                SetEvent(device_event_.get());
            }
            return;
        }
        AudioChunkInfo info;
        render_from_source(reinterpret_cast<float*>(data), static_cast<int>(avail), info);
        render_client_->ReleaseBuffer(avail, 0);
        record_chunk(avail, info);
    }
    UINT64 pos = 0, qpc100ns = 0;
    if (SUCCEEDED(audio_clock_->GetPosition(&pos, &qpc100ns))) {
        std::lock_guard lock(clock_mutex_);
        played_frames_ = static_cast<int64_t>(pos * static_cast<uint64_t>(stream_rate_) / clock_frequency_);
        played_qpc_ = static_cast<int64_t>(qpc100ns * static_cast<uint64_t>(qpc_frequency()) / 10000000ULL);
        while (segments_.size() > 1 && segments_[1].frame <= played_frames_) segments_.pop_front();
    }
}

void AudioOutput::fill_null_sink() {
    static thread_local std::vector<float> scratch;
    const int64_t now = qpc_now();
    int64_t written;
    {
        std::lock_guard lock(clock_mutex_);
        played_frames_ = null_start_frames_ + (now - null_start_qpc_) * stream_rate_ / qpc_frequency();
        played_qpc_ = now;
        written = written_frames_;
    }
    const int64_t target = played_frames_ + stream_rate_ * 30 / 1000;
    while (written < target) {
        const int n = static_cast<int>(std::min<int64_t>(target - written, 1024));
        scratch.resize(static_cast<size_t>(n) * stream_channels_);
        AudioChunkInfo info;
        render_from_source(scratch.data(), n, info);
        record_chunk(n, info);
        written += n;
    }
    queued_ms_.store((written - played_frames_) * 1000.0 / stream_rate_);
}

void AudioOutput::set_source(AudioSource* source, bool null_sink) {
    std::lock_guard lock(source_mutex_);
    source_ = source;
    null_sink_requested_ = null_sink;
}

void AudioOutput::play() {
    std::lock_guard serial(command_mutex_);
    {
        std::lock_guard lock(clock_mutex_);
        pending_cmd_ = Cmd::Play;
    }
    SetEvent(command_event_.get());
    WaitForSingleObject(command_done_event_.get(), 1000);
}

void AudioOutput::pause() {
    std::lock_guard serial(command_mutex_);
    {
        std::lock_guard lock(clock_mutex_);
        pending_cmd_ = Cmd::Pause;
    }
    SetEvent(command_event_.get());
    WaitForSingleObject(command_done_event_.get(), 1000);
}

void AudioOutput::flush(int64_t media_us) {
    std::lock_guard serial(command_mutex_);
    {
        std::lock_guard lock(clock_mutex_);
        pending_cmd_ = Cmd::Flush;
        pending_flush_us_ = media_us;
        // Visible immediately to clock readers, before the audio thread applies it.
        reset_clock_locked(media_us);
    }
    running_.store(false);
    SetEvent(command_event_.get());
    WaitForSingleObject(command_done_event_.get(), 1000);
}

int64_t AudioOutput::media_time_us() const {
    std::lock_guard lock(clock_mutex_);
    if (segments_.empty()) return idle_media_us_;
    int64_t p = played_frames_;
    if (running_.load()) p += (qpc_now() - played_qpc_) * stream_rate_ / qpc_frequency();
    p = std::clamp<int64_t>(p, 0, written_frames_);
    const Segment* s = &segments_.front();
    for (const Segment& seg : segments_) {
        if (seg.frame <= p) s = &seg;
    }
    if (p <= s->frame) return s->media_us;
    return s->media_us + static_cast<int64_t>(static_cast<double>(p - s->frame) * 1e6 * s->rate / stream_rate_);
}

bool AudioOutput::device_changed_since(uint64_t& token) const {
    uint64_t g = device_generation_.load();
    if (g == token) return false;
    token = g;
    return true;
}

}  // namespace syna
