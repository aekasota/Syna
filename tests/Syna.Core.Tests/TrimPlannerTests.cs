using Syna.Core.Engine;
using Syna.Core.State;
using Xunit;

namespace Syna.Core.Tests;

public class TrimPlannerTests
{
    // A ShadowPlay-like recording: H.264 1080p60, keyframes every 2 s, desktop + microphone in AAC.
    private static MediaInfo Clip(int audioTracks = 2, string codec = "h264", int depth = 8, bool vfr = false) => new()
    {
        DurationUs = 60_000_000,
        Video = new VideoStreamInfo { Codec = codec, Width = 1920, Height = 1080, FpsAvg = 60, BitDepth = depth, Vfr = vfr },
        Audio = Enumerable.Range(0, audioTracks).Select(i => new AudioTrackInfo { StreamIndex = i + 1, Codec = "aac", SampleRate = 48000, Channels = 2 }).ToArray(),
    };

    // The encoders that open on the development PC (RTX 4060 Ti), in the engine's order.
    private static readonly VideoEncoderInfo[] Encoders =
    [
        new() { Codec = "h264", Name = "h264_nvenc", Label = "NVIDIA NVENC (H.264)", Hardware = true, Lossless = true },
        new() { Codec = "h264", Name = "libopenh264", Label = "OpenH264 (CPU)" },
        new() { Codec = "hevc", Name = "hevc_nvenc", Label = "NVIDIA NVENC (HEVC)", Hardware = true, Lossless = true },
        new() { Codec = "hevc", Name = "libkvazaar", Label = "Kvazaar (CPU)" },
        new() { Codec = "av1", Name = "av1_nvenc", Label = "NVIDIA NVENC (AV1)", Hardware = true },
        new() { Codec = "av1", Name = "libsvtav1", Label = "SVT-AV1 (CPU)" },
        new() { Codec = "vp9", Name = "libvpx-vp9", Label = "libvpx VP9 (CPU)", Lossless = true },
    ];

    private static TrimPlan Plan(TrimMode mode, TrimProfile profile, TrimAudio audio = TrimAudio.Mix, MediaInfo? clip = null, string ext = ".mp4",
                                 TrimCustomSettings? custom = null, bool hardware = true) =>
        TrimPlanner.Plan(new TrimPlanInput
        {
            Mode = mode, Profile = profile, Arrangement = audio, Custom = custom ?? new TrimCustomSettings(), Source = clip ?? Clip(),
            InUs = 12_500_000, OutUs = 30_000_000, Keyframes = Enumerable.Range(0, 30).Select(k => k * 2_000_000L).ToArray(),
            OutputExtension = ext, Encoders = Encoders, AllowHardwareEncode = hardware,
        });

    [Fact]
    public void Original_copies_the_video_in_quick_and_smart_cut_and_says_what_is_reencoded()
    {
        var quick = Plan(TrimMode.Quick, TrimProfile.Original);
        Assert.False(quick.ReencodesVideo);
        Assert.Equal(TrimVideo.Original, quick.Output.Video);
        Assert.StartsWith("Stream copy · H.264 · 1920×1080 · 60 fps", quick.Video);
        Assert.Contains("keyframe at 0:12.000", quick.Summary);

        var smart = Plan(TrimMode.Smart, TrimProfile.Original);
        Assert.False(smart.ReencodesVideo);
        Assert.StartsWith("Copy, first 1.500 s re-encoded", smart.Video);
        Assert.Contains("NVIDIA NVENC (H.264)", smart.Summary);

        var exact = Plan(TrimMode.Exact, TrimProfile.Original);
        Assert.True(exact.ReencodesVideo);
        Assert.StartsWith("Re-encode close to the source · H.264 · 1920×1080 · 60 fps", exact.Video);
        Assert.Contains("not a copy", exact.Summary);
        Assert.Equal(TrimPlanner.OriginalQuality, exact.Output.VideoQuality);
    }

    [Fact]
    public void Original_mixes_into_aac_in_mp4_and_lossless_flac_in_matroska()
    {
        var mp4 = Plan(TrimMode.Quick, TrimProfile.Original);
        Assert.Equal((AudioCodec.Aac, 320), (mp4.Output.AudioCodec, mp4.Output.AudioBitrateKbps));
        Assert.Equal("2 tracks mixed into one · AAC 320 kb/s (encoded)", mp4.Audio);
        Assert.Contains(mp4.Notes, n => n.Contains("cannot play lossless FLAC inside MP4"));

        var mkv = Plan(TrimMode.Quick, TrimProfile.Original, ext: ".mkv");
        Assert.Equal(AudioCodec.Flac, mkv.Output.AudioCodec);
        Assert.Equal("2 tracks mixed into one · FLAC, lossless (encoded)", mkv.Audio);

        var webm = Plan(TrimMode.Quick, TrimProfile.Original, ext: ".webm", clip: Clip(codec: "vp9"));
        Assert.Equal((AudioCodec.Opus, 256), (webm.Output.AudioCodec, webm.Output.AudioBitrateKbps));
    }

    [Fact]
    public void Separate_tracks_are_copied_unless_a_format_is_asked_for()
    {
        var copy = Plan(TrimMode.Smart, TrimProfile.Balanced, TrimAudio.Separate);
        Assert.Equal(AudioCodec.Auto, copy.Output.AudioCodec);
        Assert.Equal("2 tracks kept separate · copied without re-encoding", copy.Audio);

        var custom = new TrimCustomSettings { AudioCodec = AudioCodec.Aac, AudioBitrateKbps = 192 };
        var encoded = Plan(TrimMode.Smart, TrimProfile.Custom, TrimAudio.Separate, custom: custom);
        Assert.Equal("2 tracks kept separate · re-encoded as AAC 192 kb/s", encoded.Audio);

        // WebM cannot hold AAC: the tracks have to be re-encoded, and the sheet says so.
        var webm = Plan(TrimMode.Smart, TrimProfile.Original, TrimAudio.Separate, clip: Clip(codec: "vp9"), ext: ".webm");
        Assert.StartsWith("2 tracks kept separate · re-encoded as Opus", webm.Audio);
    }

    [Fact]
    public void One_track_is_kept_as_it_is_and_more_than_two_are_never_dropped()
    {
        var one = Plan(TrimMode.Quick, TrimProfile.Original, TrimAudio.Mix, Clip(audioTracks: 1));
        Assert.Equal(AudioCodec.Auto, one.Output.AudioCodec);
        Assert.Equal("1 track · copied without re-encoding", one.Audio);

        Assert.StartsWith("4 tracks mixed into one", Plan(TrimMode.Quick, TrimProfile.Original, TrimAudio.Mix, Clip(audioTracks: 4)).Audio);
        Assert.StartsWith("4 tracks kept separate", Plan(TrimMode.Quick, TrimProfile.Original, TrimAudio.Separate, Clip(audioTracks: 4)).Audio);
        Assert.Equal("No audio", Plan(TrimMode.Quick, TrimProfile.Original, clip: Clip(audioTracks: 0)).Audio);
    }

    [Fact]
    public void Reencoding_profiles_apply_in_every_mode_and_say_the_copy_is_lost()
    {
        foreach (var mode in new[] { TrimMode.Quick, TrimMode.Smart })
        {
            var p = Plan(mode, TrimProfile.Balanced);
            Assert.True(p.ReencodesVideo);
            Assert.Equal(TrimVideo.Encode, p.Output.Video);
            Assert.Contains(p.Notes, n => n.StartsWith($"Balanced re-encodes the video, so {TrimPlanner.ModeName(mode)} cannot copy it"));
            Assert.Equal("Re-encode · H.264 · CQ 23 · 1920×1080 · 60 fps", p.Video);
            Assert.Contains("NVIDIA NVENC (H.264)", p.Summary);
        }
        Assert.DoesNotContain(Plan(TrimMode.Exact, TrimProfile.Balanced).Notes, n => n.Contains("cannot copy"));
    }

    [Fact]
    public void Profiles_trade_quality_for_size_without_touching_size_or_frame_rate()
    {
        var high = Plan(TrimMode.Exact, TrimProfile.HighQuality).Output;
        var balanced = Plan(TrimMode.Exact, TrimProfile.Balanced).Output;
        var smaller = Plan(TrimMode.Exact, TrimProfile.SmallerFile).Output;
        Assert.True(high.VideoQuality < balanced.VideoQuality && balanced.VideoQuality < smaller.VideoQuality);
        Assert.True(high.AudioBitrateKbps > balanced.AudioBitrateKbps && balanced.AudioBitrateKbps > smaller.AudioBitrateKbps);
        foreach (var o in new[] { high, balanced, smaller })
        {
            Assert.Equal(0, o.VideoHeight);  // the source's size
            Assert.Equal(0, o.VideoFps);     // the source's frame timing: never a forced 30 fps
            Assert.Equal(VideoCodec.Source, o.VideoCodec);
        }
    }

    [Fact]
    public void Custom_settings_reach_the_request_and_the_preview()
    {
        var custom = new TrimCustomSettings { Video = TrimVideo.Encode, Codec = VideoCodec.Hevc, Height = 720, Fps = 30, UseBitrate = true, BitrateKbps = 8000, MaxBitrateKbps = 12000, Speed = EncoderSpeed.Smaller };
        var p = Plan(TrimMode.Exact, TrimProfile.Custom, custom: custom);
        Assert.Equal((TrimVideo.Encode, VideoCodec.Hevc, 720, 30, 8000, 12000, 0), (p.Output.Video, p.Output.VideoCodec, p.Output.VideoHeight, p.Output.VideoFps, p.Output.VideoBitrateKbps, p.Output.VideoMaxBitrateKbps, p.Output.VideoQuality));
        Assert.Equal(EncoderSpeed.Smaller, p.Output.VideoSpeed);
        Assert.Equal("Re-encode · HEVC · 8 Mb/s, max 12 Mb/s · 1280×720 · 30 fps", p.Video);
        Assert.Contains("NVIDIA NVENC (HEVC)", p.Summary);

        // A larger size or a higher rate than the source is not offered as a change.
        var bigger = Plan(TrimMode.Exact, TrimProfile.Custom, custom: new TrimCustomSettings { Video = TrimVideo.Encode, Height = 2160, Fps = 120 });
        Assert.EndsWith("1920×1080 · 60 fps", bigger.Video);
    }

    [Fact]
    public void Lossless_is_offered_only_where_an_encoder_really_does_it()
    {
        var hevc = Plan(TrimMode.Exact, TrimProfile.Custom, custom: new TrimCustomSettings { Video = TrimVideo.Lossless, Codec = VideoCodec.Hevc });
        Assert.Null(hevc.Blocker);
        Assert.Equal(TrimVideo.Lossless, hevc.Output.Video);
        Assert.Equal("Lossless re-encode · HEVC · 1920×1080 · 60 fps", hevc.Video);
        Assert.Contains(hevc.Notes, n => n.Contains("very large"));

        var h264 = Plan(TrimMode.Exact, TrimProfile.Custom, custom: new TrimCustomSettings { Video = TrimVideo.Lossless, Codec = VideoCodec.H264 });
        Assert.Contains(h264.Notes, n => n.Contains("High 4:4:4 Predictive"));

        var av1 = Plan(TrimMode.Exact, TrimProfile.Custom, custom: new TrimCustomSettings { Video = TrimVideo.Lossless, Codec = VideoCodec.Av1 });
        Assert.Equal("No encoder on this PC can encode AV1 losslessly.", av1.Blocker);
    }

    [Fact]
    public void Combinations_that_cannot_be_written_are_blocked_with_a_reason()
    {
        var h264InWebm = Plan(TrimMode.Exact, TrimProfile.Custom, clip: Clip(codec: "vp9"), ext: ".webm",
                              custom: new TrimCustomSettings { Video = TrimVideo.Encode, Codec = VideoCodec.H264 });
        Assert.StartsWith("WebM cannot store H.264 video", h264InWebm.Blocker);

        var flacInWebm = Plan(TrimMode.Quick, TrimProfile.Custom, clip: Clip(codec: "vp9"), ext: ".webm", custom: new TrimCustomSettings { AudioCodec = AudioCodec.Flac });
        Assert.StartsWith("WebM cannot store FLAC audio", flacInWebm.Blocker);

        var h264TenBit = Plan(TrimMode.Exact, TrimProfile.Custom, clip: Clip(codec: "hevc", depth: 10), custom: new TrimCustomSettings { Video = TrimVideo.Encode, Codec = VideoCodec.H264 });
        Assert.StartsWith("There is no 10-bit H.264 encoder", h264TenBit.Blocker);

        // Keeping the source's codec on a 10-bit HEVC clip stays HEVC.
        Assert.Contains("HEVC", Plan(TrimMode.Exact, TrimProfile.Balanced, clip: Clip(codec: "hevc", depth: 10)).Video);

        var flacInMp4 = Plan(TrimMode.Quick, TrimProfile.Custom, custom: new TrimCustomSettings { AudioCodec = AudioCodec.Flac });
        Assert.Null(flacInMp4.Blocker);
        Assert.Contains(flacInMp4.Notes, n => n.StartsWith("Windows' own player"));
    }

    [Fact]
    public void Without_hardware_encoding_the_cpu_encoder_is_named()
    {
        var p = Plan(TrimMode.Exact, TrimProfile.Balanced, hardware: false);
        Assert.Contains("OpenH264 (CPU)", p.Summary);
        Assert.Contains(p.Notes, n => n.Contains("the CPU encodes it"));
    }
}

public class TrimSettingsTests
{
    [Fact]
    public void Trim_choices_survive_a_restart_and_old_settings_still_load()
    {
        using var dir = new TempDir();
        string file = Path.Combine(dir.Path, "settings.json");
        var store = new SettingsStore(file);
        var s = new AppSettings
        {
            TrimFolder = @"D:\EditedClips",
            TrimProfile = TrimProfile.Custom,
            TrimAudio = TrimAudio.Separate,
            TrimCustom = new TrimCustomSettings { Video = TrimVideo.Encode, Codec = VideoCodec.Hevc, Height = 720, Fps = 30, Quality = 24, AudioCodec = AudioCodec.Opus },
        };
        store.Save(s);
        AppSettings back = store.Load();
        Assert.Equal(@"D:\EditedClips", back.TrimFolder);
        Assert.Equal((TrimProfile.Custom, TrimAudio.Separate), (back.TrimProfile, back.TrimAudio));
        Assert.Equal((TrimVideo.Encode, VideoCodec.Hevc, 720, 30, 24, AudioCodec.Opus),
                     (back.TrimCustom.Video, back.TrimCustom.Codec, back.TrimCustom.Height, back.TrimCustom.Fps, back.TrimCustom.Quality, back.TrimCustom.AudioCodec));

        // A settings file from before profiles (it had "trimQuality") loads with the defaults.
        File.WriteAllText(file, """{ "trimFolder": "E:\\Clips", "trimMode": "Quick", "trimQuality": "VeryHigh" }""");
        AppSettings old = store.Load();
        Assert.Equal(@"E:\Clips", old.TrimFolder);
        Assert.Equal(TrimMode.Quick, old.TrimMode);
        Assert.Equal(TrimProfile.Original, old.TrimProfile);
    }
}
