using System.Runtime.InteropServices;
using System.Text.Json;

namespace Syna.Core.Engine;

public enum JobState { Running = 0, Succeeded = 1, Failed = 2, Cancelled = 3 }

public enum TrimMode { Quick = 0, Smart = 1, Exact = 2 }

/// <summary>What happens to the video (syna_trim_request.video).</summary>
public enum TrimVideo { Original = 0, Encode = 1, Lossless = 2 }

public enum VideoCodec { Source = 0, H264 = 1, Hevc = 2, Av1 = 3, Vp9 = 4 }

public enum EncoderSpeed { Faster = 0, Balanced = 1, Smaller = 2 }

/// <summary>How the audio tracks are arranged in the output.</summary>
public enum TrimAudio { Mix = 0, Separate = 1 }

public enum AudioCodec { Auto = 0, Aac = 1, Opus = 2, Flac = 3 }

/// <summary>A native background job (waveform extraction, trim). Runs on a native worker thread.</summary>
public abstract class EngineJob : IDisposable
{
    private nint _job;

    protected EngineJob(nint job) => _job = job;

    public (JobState State, EngineError Error, double Progress) Status
    {
        get
        {
            nint j = _job;
            if (j == 0) return (JobState.Cancelled, EngineError.Cancelled, 0);
            Native.JobStatus(j, out var s);
            return ((JobState)s.State, (EngineError)s.Error, s.Progress);
        }
    }

    public void Cancel()
    {
        nint j = _job;
        if (j != 0) Native.JobCancel(j);
    }

    /// <summary>Polls until the job finishes. Cancelling the token cancels the native job.</summary>
    public async Task<(JobState State, EngineError Error)> WaitAsync(IProgress<double>? progress = null, CancellationToken token = default)
    {
        using var reg = token.Register(Cancel);
        for (;;)
        {
            var (state, error, p) = Status;
            progress?.Report(p);
            if (state != JobState.Running) return (state, error);
            await Task.Delay(50, CancellationToken.None).ConfigureAwait(false);
        }
    }

    protected string? ResultJson()
    {
        nint j = _job;
        if (j == 0 || Native.JobResultJson(j, out nint json) != 0) return null;
        return Native.TakeString(json);
    }

    public void Dispose()
    {
        nint j = Interlocked.Exchange(ref _job, 0);
        if (j != 0) Native.JobDestroy(j);
        GC.SuppressFinalize(this);
    }
}

public sealed class WaveformJob : EngineJob
{
    private WaveformJob(nint job) : base(job) { }

    public static WaveformJob Start(string mediaPath, string outputFile, int bucketsPerSecond)
    {
        MediaEngine.Check(Native.WaveformStart(mediaPath, outputFile, bucketsPerSecond, out nint job));
        return new WaveformJob(job);
    }
}

/// <summary>The engine's output settings (syna_trim_request video_* and audio_* fields); zero keeps the source's.</summary>
public sealed record TrimOutput
{
    public TrimVideo Video { get; init; }
    public VideoCodec VideoCodec { get; init; }
    public int VideoHeight { get; init; }
    public int VideoFps { get; init; }
    /// <summary>Constant quality on the H.264 CQ scale (lower is better); 0 = the engine's default.</summary>
    public int VideoQuality { get; init; }
    public int VideoBitrateKbps { get; init; }
    public int VideoMaxBitrateKbps { get; init; }
    public EncoderSpeed VideoSpeed { get; init; } = EncoderSpeed.Balanced;
    public TrimAudio Audio { get; init; }
    public AudioCodec AudioCodec { get; init; }
    public int AudioBitrateKbps { get; init; }
    public int AudioSampleRate { get; init; }
    public int AudioChannels { get; init; }
    /// <summary>The player's mixer per source audio track (syna_trim_track); empty uses every track as recorded.</summary>
    public IReadOnlyList<TrimTrackMix> Tracks { get; init; } = [];
}

/// <summary>A source audio track as the player's mixer plays it: its effective gain (0 when muted or
/// silenced by a solo) and timing offset (positive = later).</summary>
public sealed record TrimTrackMix(int StreamIndex, double Gain, int OffsetMs)
{
    /// <summary>The track is not used as recorded, so it is decoded, adjusted and encoded.</summary>
    public bool Adjusted => Math.Abs(Gain - 1) > 1e-4 || OffsetMs != 0;
}

public sealed record TrimRequest(string Source, string Destination, long InUs, long OutUs, TrimMode Mode, bool AllowHardwareEncode, TrimOutput Output);

public sealed class TrimJob : EngineJob
{
    private TrimJob(nint job) : base(job) { }

    public static unsafe TrimJob Start(TrimRequest request)
    {
        nint src = Marshal.StringToHGlobalUni(request.Source);
        nint dst = Marshal.StringToHGlobalUni(request.Destination);
        try
        {
            TrimOutput o = request.Output;
            var tracks = o.Tracks.Select(t => new Native.TrimTrackNative { StreamIndex = t.StreamIndex, Gain = (float)t.Gain, OffsetUs = t.OffsetMs * 1000L }).ToArray();
            var native = new Native.TrimRequestNative
            {
                Source = src,
                Destination = dst,
                InUs = request.InUs,
                OutUs = request.OutUs,
                Mode = (int)request.Mode,
                AllowHwEncode = request.AllowHardwareEncode ? 1 : 0,
                Video = (int)o.Video,
                VideoCodec = (int)o.VideoCodec,
                VideoHeight = o.VideoHeight,
                VideoFpsNum = o.VideoFps,
                VideoFpsDen = o.VideoFps > 0 ? 1 : 0,
                VideoQuality = o.VideoQuality,
                VideoBitrateKbps = o.VideoBitrateKbps,
                VideoMaxBitrateKbps = o.VideoMaxBitrateKbps,
                VideoSpeed = (int)o.VideoSpeed,
                Audio = (int)o.Audio,
                AudioCodec = (int)o.AudioCodec,
                AudioBitrateKbps = o.AudioBitrateKbps,
                AudioSampleRate = o.AudioSampleRate,
                AudioChannels = o.AudioChannels,
                TrackCount = tracks.Length,
            };
            nint job;
            fixed (Native.TrimTrackNative* p = tracks)  // syna_trim_start copies the entries before it returns
            {
                native.Tracks = (nint)p;
                MediaEngine.Check(Native.TrimStart(in native, out job));
            }
            return new TrimJob(job);
        }
        finally
        {
            Marshal.FreeHGlobal(src);
            Marshal.FreeHGlobal(dst);
        }
    }

    public TrimReport? Report => ResultJson() is { } json ? JsonSerializer.Deserialize(json, EngineJson.Default.TrimReport) : null;
}

/// <summary>What the trim engine actually did; produced after the output was re-opened and checked.</summary>
public sealed record TrimReport
{
    public string RequestedMode { get; init; } = "";
    public string ModeUsed { get; init; } = "";
    public string Output { get; init; } = "";
    public bool Success { get; init; }
    public string Error { get; init; } = "";
    public string FallbackReason { get; init; } = "";
    public long RequestedInUs { get; init; }
    public long RequestedOutUs { get; init; }
    public long ActualStartUs { get; init; }
    public long ActualEndUs { get; init; }
    /// <summary>"original", "encode" or "lossless": what the request asked of the video.</summary>
    public string VideoHandling { get; init; } = "";
    /// <summary>"copy", "smart" (head re-encoded, rest copied) or "reencode": what was done.</summary>
    public string VideoMethod { get; init; } = "";
    public string Encoder { get; init; } = "";
    public bool HardwareEncoder { get; init; }
    public bool LosslessVideo { get; init; }
    /// <summary>The first frames of a lossless output were decoded and matched the source's pixels.</summary>
    public bool LosslessVerified { get; init; }
    public double ReencodedSeconds { get; init; }
    public double CopiedSeconds { get; init; }
    /// <summary>"mix" (several tracks mixed into one), "copy", "encode", "copy+encode" or "none".</summary>
    public string AudioMethod { get; init; } = "";
    public int AudioTracks { get; init; }
    public string AudioEncoder { get; init; } = "";
    public string AudioNote { get; init; } = "";
    public TrimAudioOutput[] AudioOutputs { get; init; } = [];
    public double ElapsedSeconds { get; init; }
    public TrimValidation Validation { get; init; } = new();
}

/// <summary>One audio track of the output: "copy", "encode" or "mix", the source tracks in it, its format.</summary>
public sealed record TrimAudioOutput
{
    public string Method { get; init; } = "";
    public int Tracks { get; init; }
    public string Description { get; init; } = "";
    /// <summary>A mixer gain or offset was applied to it.</summary>
    public bool Adjusted { get; init; }
}

public sealed record TrimValidation
{
    public bool Passed { get; init; }
    public double DurationS { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public string VideoCodec { get; init; } = "";
    public double FpsAvg { get; init; }
    public long VideoBitRate { get; init; }
    public long FrameCount { get; init; }
    public long FirstFramePtsUs { get; init; }
    public int AudioStreams { get; init; }
    public string[] AudioCodecs { get; init; } = [];
    public int[] AudioSampleRates { get; init; } = [];
    public string[] AudioLayouts { get; init; } = [];
    public string[] Issues { get; init; } = [];
}
