using Syna.Core.Services;
using Syna.Core.State;

namespace Syna.Core.Engine;

public enum TrimProfile { Original, HighQuality, Balanced, SmallerFile, Custom }

public enum OutputContainer { Mp4, Matroska, WebM }

/// <summary>The output settings of the Custom profile (kept in settings.json).</summary>
public sealed class TrimCustomSettings
{
    public TrimVideo Video { get; set; } = TrimVideo.Original;
    public VideoCodec Codec { get; set; } = VideoCodec.Source;
    /// <summary>Output height; 0 keeps the source's size. Never larger than the source.</summary>
    public int Height { get; set; }
    /// <summary>Constant output frame rate; 0 keeps the source's frame timing. Never higher than the source.</summary>
    public int Fps { get; set; }
    public bool UseBitrate { get; set; }
    /// <summary>Constant quality on the H.264 CQ scale (lower is better).</summary>
    public int Quality { get; set; } = 20;
    public int BitrateKbps { get; set; } = 12000;
    /// <summary>Peak bitrate cap; 0 = none.</summary>
    public int MaxBitrateKbps { get; set; }
    public EncoderSpeed Speed { get; set; } = EncoderSpeed.Balanced;
    public AudioCodec AudioCodec { get; set; } = AudioCodec.Auto;
    public int AudioBitrateKbps { get; set; } = 256;
    /// <summary>0 keeps the source's rate.</summary>
    public int AudioSampleRate { get; set; }
    /// <summary>0 keeps the source's channels; 1 mono; 2 stereo.</summary>
    public int AudioChannels { get; set; }
}

/// <summary>Everything a trim plan depends on.</summary>
public sealed record TrimPlanInput
{
    public TrimMode Mode { get; init; }
    public TrimProfile Profile { get; init; }
    public TrimAudio Arrangement { get; init; }
    public TrimCustomSettings Custom { get; init; } = new();
    public MediaInfo? Source { get; init; }
    public long InUs { get; init; }
    public long OutUs { get; init; }
    public long[] Keyframes { get; init; } = [];
    public string OutputExtension { get; init; } = ".mp4";
    /// <summary>Encoders that open on this PC (SystemInfo.VideoEncoders); empty while unknown.</summary>
    public IReadOnlyList<VideoEncoderInfo> Encoders { get; init; } = [];
    public bool AllowHardwareEncode { get; init; } = true;
    /// <summary>The clip's mixer, by track in Source.Audio order; null uses every track as recorded.</summary>
    public MixerState? Mixer { get; init; }
    /// <summary>The tracks' names as the clip shows them (renamed or suggested), for the sheet's text only.</summary>
    public IReadOnlyList<string> TrackNames { get; init; } = [];
}

/// <summary>What a trim will do, worked out before it runs: the engine request and what the sheet shows.</summary>
public sealed record TrimPlan
{
    public TrimOutput Output { get; init; } = new();
    /// <summary>What happens to this selection, in a sentence or two.</summary>
    public string Summary { get; init; } = "";
    public string Video { get; init; } = "";
    public string Audio { get; init; } = "";
    /// <summary>Costs and compatibility worth knowing before saving.</summary>
    public IReadOnlyList<string> Notes { get; init; } = [];
    public bool ReencodesVideo { get; init; }
    /// <summary>Set when these settings cannot be exported; the sheet shows it instead of saving.</summary>
    public string? Blocker { get; init; }
}

/// <summary>
/// Turns a profile (or the Custom settings) into the engine's request and describes, for the clip at
/// hand, what will really happen: what is copied, what is re-encoded and with which encoder. It
/// follows the engine's rules (trim.cpp), so the sheet never promises a copy that will not happen.
/// </summary>
public static class TrimPlanner
{
    // Constant quality on the H.264 CQ scale. Original's re-encodes stay close to the source.
    public const int OriginalQuality = 16;
    public const int HighQuality = 18;
    public const int BalancedQuality = 23;
    public const int SmallerQuality = 28;

    public static OutputContainer ContainerOf(string extension) => extension.ToLowerInvariant() switch
    {
        ".mkv" => OutputContainer.Matroska,
        ".webm" => OutputContainer.WebM,
        _ => OutputContainer.Mp4,
    };

    public static string ProfileName(TrimProfile p) => p switch
    {
        TrimProfile.HighQuality => "High Quality",
        TrimProfile.Balanced => "Balanced",
        TrimProfile.SmallerFile => "Smaller File",
        TrimProfile.Custom => "Custom",
        _ => "Original",
    };

    public static string ModeName(TrimMode m) => m switch { TrimMode.Quick => "Quick Cut", TrimMode.Smart => "Smart Cut", _ => "Exact Cut" };

    public static string CodecName(string codec) => codec switch
    {
        "h264" => "H.264",
        "hevc" => "HEVC",
        "av1" => "AV1",
        "vp9" => "VP9",
        "aac" => "AAC",
        "opus" => "Opus",
        "flac" => "FLAC",
        "" => "",
        _ => codec.ToUpperInvariant(),
    };

    public static string EngineCodec(VideoCodec c) => c switch
    {
        VideoCodec.H264 => "h264",
        VideoCodec.Hevc => "hevc",
        VideoCodec.Av1 => "av1",
        VideoCodec.Vp9 => "vp9",
        _ => "",
    };

    /// <summary>The engine request for a profile. Mixing makes a new track, so it is always encoded.
    /// Separate tracks stay Automatic (copied where possible); the profile's bitrate applies to those
    /// that must be encoded, because the mixer changes them or the container cannot store their codec.</summary>
    public static TrimOutput OutputFor(TrimProfile profile, TrimAudio arrangement, TrimCustomSettings custom, OutputContainer container, int audioTracks)
    {
        bool mixing = arrangement == TrimAudio.Mix && audioTracks > 1;
        (AudioCodec Codec, int Kbps) Mix(int aac, int opus) =>
            container == OutputContainer.WebM ? (mixing ? AudioCodec.Opus : AudioCodec.Auto, opus) : (mixing ? AudioCodec.Aac : AudioCodec.Auto, aac);
        TrimOutput Encode(int quality, EncoderSpeed speed, (AudioCodec Codec, int Kbps) audio) => new()
        {
            Video = TrimVideo.Encode, VideoQuality = quality, VideoSpeed = speed,
            Audio = arrangement, AudioCodec = audio.Codec, AudioBitrateKbps = audio.Kbps,
        };
        switch (profile)
        {
            case TrimProfile.Original:
            {
                // Lossless for a mix where Windows plays it (FLAC in Matroska); in MP4 Windows' own
                // player cannot decode FLAC, so the mix is AAC at its highest common rate.
                var audio = mixing && container == OutputContainer.Matroska ? (AudioCodec.Flac, 0) : Mix(320, 256);
                return new TrimOutput { Video = TrimVideo.Original, VideoQuality = OriginalQuality, Audio = arrangement, AudioCodec = audio.Item1, AudioBitrateKbps = audio.Item2 };
            }
            case TrimProfile.HighQuality: return Encode(HighQuality, EncoderSpeed.Smaller, Mix(256, 192));
            case TrimProfile.Balanced: return Encode(BalancedQuality, EncoderSpeed.Balanced, Mix(192, 160));
            case TrimProfile.SmallerFile: return Encode(SmallerQuality, EncoderSpeed.Smaller, Mix(128, 96));
        }
        var c = custom;
        bool encode = c.Video == TrimVideo.Encode;
        return new TrimOutput
        {
            Video = c.Video,
            VideoCodec = c.Video == TrimVideo.Original ? VideoCodec.Source : c.Codec,
            VideoHeight = encode ? c.Height : 0,
            VideoFps = encode ? c.Fps : 0,
            VideoQuality = c.Video == TrimVideo.Original ? OriginalQuality : encode && !c.UseBitrate ? c.Quality : 0,
            VideoBitrateKbps = encode && c.UseBitrate ? c.BitrateKbps : 0,
            VideoMaxBitrateKbps = encode ? c.MaxBitrateKbps : 0,
            VideoSpeed = c.Speed,
            Audio = arrangement,
            AudioCodec = c.AudioCodec,
            AudioBitrateKbps = c.AudioCodec is AudioCodec.Aac or AudioCodec.Opus ? c.AudioBitrateKbps : 0,
            AudioSampleRate = c.AudioSampleRate,
            AudioChannels = c.AudioChannels,
        };
    }

    /// <summary>The mixer per source track, keyed by stream index (a track's name plays no part).</summary>
    public static IReadOnlyList<TrimTrackMix> MixFor(MediaInfo? source, MixerState? mixer) =>
        source == null || mixer == null ? [] : source.Audio.Select((a, k) => new TrimTrackMix(a.StreamIndex, mixer.EffectiveGain(k), mixer.Offset(k))).ToArray();

    public static TrimPlan Plan(TrimPlanInput i)
    {
        var container = ContainerOf(i.OutputExtension);
        int tracks = i.Source?.Audio.Length ?? 0;
        TrimOutput o = OutputFor(i.Profile, i.Arrangement, i.Custom, container, tracks) with { Tracks = MixFor(i.Source, i.Mixer) };
        var notes = new List<string>();
        string? blocker = null;
        var (summary, video, reencodes) = PlanVideo(i, o, container, notes, ref blocker);
        string audio = PlanAudio(i, o, container, tracks, notes, ref blocker);
        return new TrimPlan
        {
            Output = o, Summary = summary, Video = video, Audio = audio, Notes = notes, ReencodesVideo = reencodes, Blocker = blocker,
        };
    }

    private static (string Summary, string Video, bool Reencodes) PlanVideo(TrimPlanInput i, TrimOutput o, OutputContainer container, List<string> notes, ref string? blocker)
    {
        VideoStreamInfo? v = i.Source?.Video;
        if (v == null) return ("Audio only: the selected audio is exported.", "No video", false);
        double total = (i.OutUs - i.InUs) / 1e6;
        string size = $"{v.Width}×{v.Height}";
        string rate = v.Vfr ? $"{v.FpsAvg:0.##} fps, variable timing kept" : $"{v.FpsAvg:0.##} fps";
        string codec = CodecName(v.Codec);

        if (o.Video == TrimVideo.Original)
        {
            string encoder = EncoderLabel(i, v.Codec, lossless: false) ?? "the GPU encoder when available";
            long before = i.Keyframes.Where(k => k <= i.InUs).DefaultIfEmpty(0).Max();
            long next = i.Keyframes.Where(k => k >= i.InUs && k < i.OutUs).DefaultIfEmpty(-1).Min();
            switch (i.Mode)
            {
                case TrimMode.Quick:
                    return (before == i.InUs
                            ? "Copies the video without re-encoding. IN is on a keyframe, so the cut is exact."
                            : $"Copies the video without re-encoding. Starts at the keyframe at {Timecode.Precise(before)}, {Timecode.Seconds((i.InUs - before) / 1e6)} before IN.",
                        $"Stream copy · {codec} · {size} · {rate}", false);
                case TrimMode.Smart when next == i.InUs:
                    return ("IN is on a keyframe: the video is copied without re-encoding.", $"Stream copy · {codec} · {size} · {rate}", false);
                case TrimMode.Smart when next > i.InUs:
                    return ($"Re-encodes {Timecode.Seconds((next - i.InUs) / 1e6)} up to the keyframe at {Timecode.Precise(next)} with {encoder} and copies the remaining {Timecode.Seconds((i.OutUs - next) / 1e6)}. Falls back to Exact Cut if the stream cannot be spliced safely.",
                        $"Copy, first {Timecode.Seconds((next - i.InUs) / 1e6)} re-encoded · {codec} · {size} · {rate}", false);
                case TrimMode.Smart:
                    return ($"No keyframe inside the selection, so all {Timecode.Seconds(total)} are re-encoded with {encoder}, keeping the source's codec, size and frame timing.",
                        $"Re-encode close to the source · {codec} · {size} · {rate}", true);
                default:
                    return ($"Re-encodes all {Timecode.Seconds(total)} with {encoder}, keeping the source's codec, size and frame timing. Frame-accurate start and end; a new encode, not a copy of the original.",
                        $"Re-encode close to the source · {codec} · {size} · {rate}", true);
            }
        }

        bool lossless = o.Video == TrimVideo.Lossless;
        string? target = TargetCodec(i, v, o.VideoCodec, container, lossless, ref blocker);
        string targetName = target != null ? CodecName(target) : codec;
        string label = (target != null ? EncoderLabel(i, target, lossless) : null) ?? "the GPU encoder when available";
        if (target != null && i.Encoders.Count > 0 && !i.Encoders.Any(e => e.Codec == target && e.Hardware && (!lossless || e.Lossless) && i.AllowHardwareEncode))
            notes.Add($"No hardware {targetName} encoder is used, so the CPU encodes it: this takes longer.");
        string why = lossless ? "Lossless" : ProfileName(i.Profile);
        if (i.Mode != TrimMode.Exact)
            notes.Add($"{why} re-encodes the video, so {ModeName(i.Mode)} cannot copy it: the cut is frame-accurate and takes longer than a copy.");
        if (lossless)
        {
            notes.Add("Lossless video is very large: often several times the size of the source.");
            if (target == "h264")
                notes.Add("Lossless H.264 uses the High 4:4:4 Predictive profile, which many players and GPUs cannot decode. Lossless HEVC plays more widely.");
            return ($"Re-encodes all {Timecode.Seconds(total)} losslessly with {label}: every frame decodes to exactly the source's pixels.",
                $"Lossless re-encode · {targetName} · {size} · {rate}", true);
        }

        string outSize = size;
        if (o.VideoHeight > 0 && o.VideoHeight < v.Height)
        {
            int h = o.VideoHeight & ~1;
            int w = Math.Max(2, (int)Math.Round((double)v.Width * h / v.Height / 2.0) * 2);
            outSize = $"{w}×{h}";
        }
        string outRate = o.VideoFps > 0 && o.VideoFps < v.FpsAvg - 0.01 ? $"{o.VideoFps} fps" : rate;
        string quality = o.VideoBitrateKbps > 0
            ? $"{o.VideoBitrateKbps / 1000.0:0.#} Mb/s" + (o.VideoMaxBitrateKbps > 0 ? $", max {o.VideoMaxBitrateKbps / 1000.0:0.#} Mb/s" : "")
            : $"CQ {(o.VideoQuality > 0 ? o.VideoQuality : BalancedQuality)}";
        return ($"Re-encodes the whole selection ({Timecode.Seconds(total)}) with {label}: frame-accurate from IN to OUT.",
            $"Re-encode · {targetName} · {quality} · {outSize} · {outRate}", true);
    }

    // The codec a re-encode produces, in the engine's order: the requested one, or the source's and
    // then the most compatible ones the container takes and an encoder here can make.
    private static string? TargetCodec(TrimPlanInput i, VideoStreamInfo v, VideoCodec requested, OutputContainer container, bool lossless, ref string? blocker)
    {
        bool Usable(string codec) =>
            (container != OutputContainer.WebM || codec is "vp9" or "av1") &&
            !(codec == "h264" && v.BitDepth > 8) &&
            (i.Encoders.Count == 0 || EncoderLabel(i, codec, lossless) != null);
        if (requested != VideoCodec.Source)
        {
            string codec = EngineCodec(requested);
            if (container == OutputContainer.WebM && codec is not ("vp9" or "av1"))
                blocker = $"WebM cannot store {CodecName(codec)} video. Choose VP9 or AV1, or save as .mp4 or .mkv.";
            else if (codec == "h264" && v.BitDepth > 8)
                blocker = "There is no 10-bit H.264 encoder. Choose HEVC or AV1 for this 10-bit video.";
            else if (i.Encoders.Count > 0 && EncoderLabel(i, codec, lossless) == null)
                blocker = lossless ? $"No encoder on this PC can encode {CodecName(codec)} losslessly." : $"No {CodecName(codec)} encoder is available on this PC.";
            return codec;
        }
        foreach (string codec in new[] { v.Codec, "h264", "hevc", "av1", "vp9" }.Distinct())
            if (Usable(codec)) return codec;
        blocker = lossless ? "No encoder on this PC can encode this video losslessly." : "No suitable video encoder is available on this PC.";
        return null;
    }

    private static string? EncoderLabel(TrimPlanInput i, string codec, bool lossless) =>
        i.Encoders.FirstOrDefault(e => e.Codec == codec && (i.AllowHardwareEncode || !e.Hardware) && (!lossless || e.Lossless))?.Label;

    private static string PlanAudio(TrimPlanInput i, TrimOutput o, OutputContainer container, int tracks, List<string> notes, ref string? blocker)
    {
        if (tracks == 0) return "No audio";
        AudioTrackInfo[] audio = i.Source!.Audio;
        string format = (o.AudioSampleRate > 0 ? $" · {o.AudioSampleRate / 1000.0:0.#} kHz" : "") +
                        (o.AudioChannels == 1 ? " · mono" : o.AudioChannels == 2 ? " · stereo" : "");
        if (o.AudioCodec != AudioCodec.Auto && !Accepts(container, AudioCodecName(o.AudioCodec)))
            blocker ??= $"WebM cannot store {CodecName(AudioCodecName(o.AudioCodec))} audio. Choose Opus, or save as .mp4 or .mkv.";
        if (o.AudioCodec == AudioCodec.Flac && container == OutputContainer.Mp4)
            notes.Add("Windows' own player and some editors cannot play FLAC inside MP4; AAC plays everywhere, or save as .mkv.");
        string mixer = MixerSummary(i);
        if (mixer.Length > 0) notes.Add($"Mixer applied: {mixer}.");

        if (o.Audio == TrimAudio.Mix && tracks > 1)
        {
            string encoded = Describe(o.AudioCodec == AudioCodec.Auto ? AutoCodec(container, audio[0].Codec) : o.AudioCodec, o, Channels(audio, o));
            if (i.Profile == TrimProfile.Original && container == OutputContainer.Mp4)
                notes.Add("Mixing makes a new track, so it is encoded. AAC at 320 kb/s is used because Windows cannot play lossless FLAC inside MP4; save as .mkv for a lossless mix.");
            return $"{tracks} tracks mixed into one · {encoded}{format} (encoded)";
        }

        // Tracks stay separate (or there is only one): copied unless the mixer changes them, a format
        // was asked for, or the container cannot store the source's codec.
        bool keepFormat = o.AudioCodec == AudioCodec.Auto && o.AudioSampleRate == 0 && o.AudioChannels == 0;
        bool[] copies = audio.Select(a => keepFormat && Accepts(container, a.Codec) && o.Tracks.FirstOrDefault(t => t.StreamIndex == a.StreamIndex)?.Adjusted != true).ToArray();
        int copied = copies.Count(c => c);
        string what = tracks == 1 ? "1 track" : $"{tracks} tracks kept separate";
        if (copied == tracks) return $"{what} · copied without re-encoding";
        AudioTrackInfo first = audio[Array.IndexOf(copies, false)];
        var codec = o.AudioCodec == AudioCodec.Auto ? AutoCodec(container, first.Codec) : o.AudioCodec;
        string encodedAs = Describe(codec, o, first.Channels);
        if (keepFormat && audio.Any(a => !Accepts(container, a.Codec)))
            notes.Add($"{(container == OutputContainer.WebM ? "WebM" : "This format")} cannot store the source's audio codec, so the audio is re-encoded.");
        return copied > 0
            ? $"{what} · {copied} copied, {tracks - copied} re-encoded as {encodedAs}{format}"
            : $"{what} · re-encoded as {encodedAs}{format}";
    }

    // "System sounds at 30%, Microphone muted": what the mixer changes, by the names the clip shows.
    private static string MixerSummary(TrimPlanInput i)
    {
        if (i.Mixer is not { } m) return "";
        AudioTrackInfo[] audio = i.Source!.Audio;
        bool anySolo = m.Solo.Any(s => s);
        var parts = new List<string>();
        for (int k = 0; k < audio.Length; k++)
        {
            string name = k < i.TrackNames.Count && i.TrackNames[k].Length > 0 ? i.TrackNames[k] : audio[k].Label.Length > 0 ? audio[k].Label : $"Track {k + 1}";
            bool muted = k < m.Mute.Length && m.Mute[k], soloed = k < m.Solo.Length && m.Solo[k];
            double gain = m.EffectiveGain(k);
            int offset = m.Offset(k);
            string what;
            if (muted) what = "muted";
            else if (anySolo && !soloed) what = "silent";  // another track is soloed
            else
            {
                string level = string.Join(" ", new[] { soloed ? "soloed" : "", Math.Abs(gain - 1) > 1e-4 ? $"at {Math.Round(gain * 100)}%" : "" }.Where(s => s.Length > 0));
                string timing = offset > 0 ? $"delayed {offset} ms" : offset < 0 ? $"advanced {-offset} ms" : "";
                what = string.Join(" and ", new[] { level, timing }.Where(s => s.Length > 0));
            }
            if (what.Length > 0) parts.Add($"{name} {what}");
        }
        return string.Join(", ", parts);
    }

    private static int Channels(AudioTrackInfo[] audio, TrimOutput o) => o.AudioChannels > 0 ? o.AudioChannels : Math.Max(1, audio.Max(a => a.Channels));

    // The engine's automatic choice: the source's codec when the container takes it, then AAC, then Opus.
    private static AudioCodec AutoCodec(OutputContainer container, string sourceCodec) =>
        sourceCodec == "opus" && Accepts(container, "opus") ? AudioCodec.Opus
        : sourceCodec == "flac" && Accepts(container, "flac") ? AudioCodec.Flac
        : Accepts(container, "aac") ? AudioCodec.Aac : AudioCodec.Opus;

    private static string Describe(AudioCodec codec, TrimOutput o, int channels)
    {
        // Engine defaults per channel when no bitrate is given: AAC 128 kb/s, Opus 80 kb/s.
        int kbps = o.AudioBitrateKbps > 0 ? o.AudioBitrateKbps : codec == AudioCodec.Opus ? 80 * channels : 128 * channels;
        return codec switch
        {
            AudioCodec.Flac => "FLAC, lossless",
            AudioCodec.Opus => $"Opus {kbps} kb/s",
            _ => $"AAC {kbps} kb/s",
        };
    }

    private static string AudioCodecName(AudioCodec c) => c switch { AudioCodec.Aac => "aac", AudioCodec.Opus => "opus", AudioCodec.Flac => "flac", _ => "" };

    public static bool Accepts(OutputContainer container, string codec) => container switch
    {
        OutputContainer.WebM => codec is "opus" or "vorbis",
        OutputContainer.Matroska => true,
        _ => codec is "aac" or "mp3" or "opus" or "flac" or "alac" or "ac3" or "eac3",
    };
}
