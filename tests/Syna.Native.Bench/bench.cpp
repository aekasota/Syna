// Engine benchmark. Drives the public C ABI the way the app does (open, seek, step, play, trim)
// against one media file and writes a JSON report. scripts/benchmark.ps1 runs it over the fixtures
// and real clips and aggregates the results; docs/PERFORMANCE.md quotes them.
//
// Usage: syna_bench <file> [--out report.json] [--play-seconds N] [--reopens N] [--no-trim]

#include "syna_api.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<int>(c));
            continue;
        }
        out += c;
    }
    return out + "\"";
}

struct Summary {
    double median = 0, p95 = 0, max = 0;
};

Summary summarize(std::vector<double> v) {
    Summary s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.median = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(0.95 * static_cast<double>(v.size()))) - 1)];
    s.max = v.back();
    return s;
}

std::string summary_json(const Summary& s) { return std::format(R"({{"medianMs":{:.2f},"p95Ms":{:.2f},"maxMs":{:.2f}}})", s.median, s.p95, s.max); }

class Player {
public:
    Player() {
        if (syna_player_create(&p_) != SYNA_OK) throw std::runtime_error(std::string("player create: ") + syna_last_error());
        syna_player_resize(p_, 1920, 1080, 1.0f, 1.0f);
        syna_player_set_master_mute(p_, 1);  // silent run; the audio clock and mixer still work
    }
    ~Player() { syna_player_destroy(p_); }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    syna_player* get() const { return p_; }

    syna_event wait(int type, std::chrono::milliseconds timeout = 15s) {
        const auto deadline = Clock::now() + timeout;
        for (;;) {
            syna_event ev[32];
            int n = syna_player_poll_events(p_, ev, 32);
            for (int i = 0; i < n; ++i) {
                if (ev[i].type == type) return ev[i];
                if (ev[i].type == SYNA_EVENT_OPEN_FAILED || ev[i].type == SYNA_EVENT_ERROR)
                    throw std::runtime_error(std::format("engine event {} code {}", ev[i].type, ev[i].code));
            }
            if (Clock::now() > deadline) throw std::runtime_error(std::format("timeout waiting for event {}", type));
            std::this_thread::sleep_for(1ms);
        }
    }

    void drain() {
        syna_event ev[32];
        while (syna_player_poll_events(p_, ev, 32) > 0) {
        }
    }

    double open(const std::wstring& path) {
        drain();
        syna_open_options o{};
        o.hw_decode = 1;
        const auto t0 = Clock::now();
        syna_player_open(p_, path.c_str(), &o);
        wait(SYNA_EVENT_OPENED);
        wait(SYNA_EVENT_SEEK_DONE);
        return ms_since(t0);
    }

    double seek(int64_t us, int mode) {
        drain();
        const auto t0 = Clock::now();
        syna_player_seek(p_, us, mode);
        wait(SYNA_EVENT_SEEK_DONE);
        return ms_since(t0);
    }

    double step(int frames) {
        drain();
        const auto t0 = Clock::now();
        syna_player_step(p_, frames);
        wait(SYNA_EVENT_SEEK_DONE);
        return ms_since(t0);
    }

    syna_player_state state() const {
        syna_player_state s{};
        syna_player_get_state(p_, &s);
        return s;
    }

private:
    syna_player* p_ = nullptr;
};

syna_process_stats sample() {
    syna_process_stats s{};
    syna_sample_process_stats(&s);
    return s;
}

// Runs a background job to completion; returns elapsed ms and the job's JSON result (may be empty).
std::pair<double, std::string> run_job(syna_job* job, Clock::time_point t0) {
    syna_job_status st{};
    do {
        std::this_thread::sleep_for(5ms);
        syna_job_status_get(job, &st);
    } while (st.state == SYNA_JOB_RUNNING);
    const double ms = ms_since(t0);
    std::string result;
    char* json = nullptr;
    if (syna_job_result_json(job, &json) == SYNA_OK && json) {
        result = json;
        syna_free_string(json);
    }
    syna_job_destroy(job);
    if (st.state != SYNA_JOB_SUCCEEDED) throw std::runtime_error(std::format("job ended with state {} error {}", st.state, st.error));
    return {ms, result};
}

std::string decoder_name(int d) {
    switch (d) {
    case SYNA_DECODER_D3D11VA: return "d3d11va";
    case SYNA_DECODER_SOFTWARE: return "software";
    default: return "none";
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::fprintf(stderr, "usage: syna_bench <file> [--out report.json] [--play-seconds N] [--reopens N] [--no-trim]\n");
        return 2;
    }
    const std::wstring file = std::filesystem::absolute(argv[1]).wstring();
    std::wstring out_path;
    double play_seconds = 8;
    int reopens = 10;
    bool trims = true;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == L"--play-seconds" && i + 1 < argc) play_seconds = std::stod(argv[++i]);
        else if (a == L"--reopens" && i + 1 < argc) reopens = std::max(1, std::stoi(argv[++i]));
        else if (a == L"--no-trim") trims = false;
    }

    const std::filesystem::path work = std::filesystem::temp_directory_path() / "syna-bench";
    std::filesystem::create_directories(work);
    syna_initialize((work / "logs").wstring().c_str(), 0);

    std::vector<std::string> fields;
    auto add = [&](const std::string& key, const std::string& raw) { fields.push_back(json_string(key) + ":" + raw); };
    add("file", json_string(utf8(file)));
    add("fileBytes", std::to_string(std::filesystem::file_size(file)));
    int exit_code = 0;

    try {
        // Probe (what the explorer and session open do on a worker thread).
        {
            char* json = nullptr;
            auto t0 = Clock::now();
            if (syna_probe_json(file.c_str(), SYNA_PROBE_FAST, &json) != SYNA_OK) throw std::runtime_error(std::string("probe: ") + syna_last_error());
            add("probeFastMs", std::format("{:.2f}", ms_since(t0)));
            syna_free_string(json);
            t0 = Clock::now();
            syna_probe_json(file.c_str(), SYNA_PROBE_FULL, &json);
            add("probeFullMs", std::format("{:.2f}", ms_since(t0)));
            syna_free_string(json);
        }

        const auto mem_start = sample();
        auto t_create = Clock::now();
        Player player;
        add("playerCreateMs", std::format("{:.2f}", ms_since(t_create)));
        add("openToFirstFrameMs", std::format("{:.2f}", player.open(file)));
        syna_player_state s0 = player.state();
        add("decoder", json_string(decoder_name(s0.decoder)));
        add("durationUs", std::to_string(s0.duration_us));
        add("frameDurationUs", std::to_string(s0.frame_duration_us));
        add("audioTracks", std::to_string(s0.audio_tracks));
        const int64_t duration = s0.duration_us;
        const int64_t frame = std::max<int64_t>(s0.frame_duration_us, 1000);

        // Seeks to fixed pseudo-random positions (same sequence every run).
        std::mt19937 rng(42);
        std::uniform_int_distribution<int64_t> pos(0, std::max<int64_t>(0, duration - 2 * frame));
        std::vector<double> exact, fast;
        for (int i = 0; i < 25; ++i) exact.push_back(player.seek(pos(rng), SYNA_SEEK_EXACT));
        for (int i = 0; i < 25; ++i) fast.push_back(player.seek(pos(rng), SYNA_SEEK_FAST));
        add("seekExact", summary_json(summarize(exact)));
        add("seekFast", summary_json(summarize(fast)));

        // Frame stepping around the middle; backward steps cross at least one keyframe boundary.
        player.seek(duration / 2, SYNA_SEEK_EXACT);
        std::vector<double> fwd, back;
        for (int i = 0; i < 15; ++i) fwd.push_back(player.step(1));
        for (int i = 0; i < 45; ++i) back.push_back(player.step(-1));
        add("stepForward", summary_json(summarize(fwd)));
        add("stepBackward", summary_json(summarize(back)));

        // Real-time playback from the start.
        player.seek(0, SYNA_SEEK_EXACT);
        const double seconds = std::min(play_seconds, std::max(1.0, duration / 1e6 - 0.5));
        syna_player_state before = player.state();
        sample();  // resets the CPU/GPU interval
        syna_player_play(player.get());
        const auto t_play = Clock::now();
        double drift_max = 0, drift_sum = 0;
        int drift_n = 0;
        while (ms_since(t_play) < seconds * 1000) {
            std::this_thread::sleep_for(50ms);
            syna_player_state s = player.state();
            if (s.state != SYNA_STATE_PLAYING || ms_since(t_play) < 500) continue;  // skip start-up
            drift_max = std::max(drift_max, std::fabs(s.av_drift_ms));
            drift_sum += std::fabs(s.av_drift_ms);
            ++drift_n;
        }
        const syna_process_stats during = sample();
        syna_player_state after = player.state();
        syna_player_pause(player.get());
        const double wall_s = ms_since(t_play) / 1000.0;
        add("playback", std::format(
            R"({{"seconds":{:.2f},"framesPresented":{},"framesDropped":{},"presentedFps":{:.2f},"avDriftMeanMs":{:.2f},"avDriftMaxMs":{:.2f},"audioUnderruns":{},"cpuPercentAllCores":{:.2f},"gpuDecodePercent":{:.2f},"gpu3dPercent":{:.2f},"workingSetMB":{:.1f},"privateMB":{:.1f},"gpuDedicatedMB":{:.1f}}})",
            wall_s, after.frames_presented - before.frames_presented, after.frames_dropped - before.frames_dropped, after.presented_fps,
            drift_n ? drift_sum / drift_n : 0.0, drift_max, after.audio_underruns - before.audio_underruns, during.cpu_percent,
            during.gpu_decode_percent, during.gpu_3d_percent, during.working_set_bytes / 1048576.0, during.private_bytes / 1048576.0,
            during.gpu_dedicated_bytes / 1048576.0));

        // Repeated open/close: private memory must plateau, not grow per open (leak check).
        const auto mem_before = sample();
        std::vector<double> reopen;
        std::string curve = "[";
        for (int i = 0; i < reopens; ++i) {
            syna_player_close(player.get());
            reopen.push_back(player.open(file));
            curve += std::format("{}{:.1f}", i ? "," : "", sample().private_bytes / 1048576.0);
        }
        std::this_thread::sleep_for(300ms);
        const auto mem_after = sample();
        add("reopen", summary_json(summarize(reopen)));
        add("memory", std::format(R"({{"privateStartMB":{:.1f},"privateBeforeReopenMB":{:.1f},"privateAfterReopensMB":{:.1f},"workingSetAfterMB":{:.1f},"reopens":{},"privateCurveMB":{}]}})",
                                  mem_start.private_bytes / 1048576.0, mem_before.private_bytes / 1048576.0, mem_after.private_bytes / 1048576.0,
                                  mem_after.working_set_bytes / 1048576.0, reopens, curve));
    } catch (const std::exception& e) {
        add("error", json_string(e.what()));
        exit_code = 1;
    }

    // Background jobs the app runs when a clip opens.
    try {
        syna_job* job = nullptr;
        const std::wstring wave = (work / "bench.synw").wstring();
        auto t0 = Clock::now();
        if (syna_waveform_start(file.c_str(), wave.c_str(), 200, &job) != SYNA_OK) throw std::runtime_error(syna_last_error());
        add("waveformMs", std::format("{:.2f}", run_job(job, t0).first));
        std::filesystem::remove(wave);

        const std::wstring thumb = (work / "bench.jpg").wstring();
        t0 = Clock::now();
        if (syna_make_thumbnail(file.c_str(), thumb.c_str(), 384, -1) != SYNA_OK) throw std::runtime_error(syna_last_error());
        add("thumbnailMs", std::format("{:.2f}", ms_since(t0)));
        std::filesystem::remove(thumb);
    } catch (const std::exception& e) {
        add("jobError", json_string(e.what()));
        exit_code = 1;
    }

    if (trims) {
        // The app probes encoders in the background at startup; do the same before timing trims.
        char* json = nullptr;
        const auto t_probe = Clock::now();
        if (syna_query_system_json(&json) == SYNA_OK) syna_free_string(json);
        add("encoderProbeMs", std::format("{:.2f}", ms_since(t_probe)));
        json = nullptr;
        int64_t duration = 0;
        if (syna_probe_json(file.c_str(), SYNA_PROBE_FAST, &json) == SYNA_OK) {
            std::string j = json;
            syna_free_string(json);
            auto at = j.find("\"durationUs\":");
            if (at != std::string::npos) duration = std::stoll(j.substr(at + 13));
        }
        std::vector<std::string> results;
        const std::wstring ext = std::filesystem::path(file).extension().wstring();
        const wchar_t* names[] = {L"quick", L"smart", L"exact"};
        for (int mode = SYNA_TRIM_QUICK; mode <= SYNA_TRIM_EXACT; ++mode) {
            const std::wstring out = (work / (std::wstring(L"bench_trim_") + names[mode] + ext)).wstring();
            std::filesystem::remove(out);
            syna_trim_request r{};
            r.source = file.c_str();
            r.destination = out.c_str();
            r.in_us = duration * 3 / 10;
            r.out_us = duration * 7 / 10;
            r.mode = mode;
            r.allow_hw_encode = 1;
            try {
                syna_job* job = nullptr;
                auto t0 = Clock::now();
                if (syna_trim_start(&r, &job) != SYNA_OK) throw std::runtime_error(syna_last_error());
                auto [ms, report] = run_job(job, t0);
                const auto bytes = std::filesystem::exists(out) ? std::filesystem::file_size(out) : 0;
                results.push_back(std::format(R"({{"mode":"{}","ms":{:.2f},"outputBytes":{},"report":{}}})", utf8(names[mode]), ms, bytes,
                                              report.empty() ? "null" : report));
            } catch (const std::exception& e) {
                results.push_back(std::format(R"({{"mode":"{}","error":{}}})", utf8(names[mode]), json_string(e.what())));
                exit_code = 1;
            }
            std::filesystem::remove(out);
        }
        std::string arr = "[";
        for (size_t i = 0; i < results.size(); ++i) arr += (i ? "," : "") + results[i];
        add("trims", arr + "]");
    }

    std::string doc = "{";
    for (size_t i = 0; i < fields.size(); ++i) doc += (i ? ",\n  " : "\n  ") + fields[i];
    doc += "\n}\n";
    if (!out_path.empty()) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, out_path.c_str(), L"wb") == 0 && f) {
            std::fwrite(doc.data(), 1, doc.size(), f);
            std::fclose(f);
        }
    }
    std::fwrite(doc.data(), 1, doc.size(), stdout);
    syna_shutdown();
    return exit_code;
}
