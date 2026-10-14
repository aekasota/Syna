using System.Text.Json;

namespace Syna.Core.Engine;

public enum EngineError
{
    Ok = 0,
    InvalidArgument = -1,
    Failed = -2,
    CannotOpen = -3,
    Unsupported = -4,
    NoVideo = -5,
    Device = -6,
    Cancelled = -7,
    Io = -8,
    DiskFull = -9,
    AccessDenied = -10,
    NotFound = -11,
    Busy = -12,
    Validation = -13,
}

public sealed class EngineException(EngineError error, string message) : Exception(message)
{
    public EngineError Error { get; } = error;
}

public sealed record SystemInfo
{
    public string Adapter { get; init; } = "";
    public string Vendor { get; init; } = "";
    public long DedicatedVideoMemory { get; init; }
    public string[] HardwareDecoders { get; init; } = [];
    public string[] HardwareEncoders { get; init; } = [];
    /// <summary>Every video encoder that opens on this machine, per codec in the order trims use them.</summary>
    public VideoEncoderInfo[] VideoEncoders { get; init; } = [];
    public string FfmpegVersion { get; init; } = "";
    public string FfmpegLicense { get; init; } = "";
}

public sealed record VideoEncoderInfo
{
    public string Codec { get; init; } = "";   // "h264", "hevc", "av1", "vp9"
    public string Name { get; init; } = "";    // "hevc_nvenc"
    public string Label { get; init; } = "";   // "NVIDIA NVENC (HEVC)"
    public bool Hardware { get; init; }
    public bool Lossless { get; init; }
}

public static class MediaEngine
{
    private static int _initialized;

    public static void Initialize(string? logDirectory, bool verbose)
    {
        if (Interlocked.Exchange(ref _initialized, 1) == 1) return;
        Check(Native.Initialize(logDirectory, verbose ? 1 : 0));
    }

    public static void Shutdown() => Native.Shutdown();

    public static uint ApiVersion => Native.ApiVersion();

    /// <summary>Blocking; call from a worker thread. Fast mode reads only container headers.</summary>
    public static MediaInfo Probe(string path, bool full)
    {
        Check(Native.ProbeJson(path, full ? 1 : 0, out nint json));
        return JsonSerializer.Deserialize(Native.TakeString(json)!, EngineJson.Default.MediaInfo)!;
    }

    public static bool TryProbe(string path, bool full, out MediaInfo? info, out EngineError error)
    {
        int r = Native.ProbeJson(path, full ? 1 : 0, out nint json);
        error = (EngineError)r;
        info = r == 0 ? JsonSerializer.Deserialize(Native.TakeString(json)!, EngineJson.Default.MediaInfo) : null;
        return r == 0;
    }

    /// <summary>Blocking; decodes the keyframe near <paramref name="at"/> (default: a representative moment) and writes a JPEG.</summary>
    public static void MakeThumbnail(string path, string outJpeg, int maxWidth, TimeSpan? at = null)
        => Check(Native.MakeThumbnail(path, outJpeg, maxWidth, at is { } t ? t.Ticks / 10 : -1));

    public static SystemInfo QuerySystem()
    {
        Check(Native.QuerySystemJson(out nint json));
        return JsonSerializer.Deserialize(Native.TakeString(json)!, EngineJson.Default.SystemInfo)!;
    }

    public static ProcessStats SampleProcessStats()
    {
        Native.SampleProcessStats(out ProcessStats s);
        return s;
    }

    internal static void Check(int result)
    {
        if (result >= 0) return;
        string detail = Native.LastErrorMessage();
        throw new EngineException((EngineError)result, string.IsNullOrEmpty(detail) ? Describe((EngineError)result) : detail);
    }

    public static string Describe(EngineError error) => error switch
    {
        EngineError.CannotOpen => "The file could not be read as media.",
        EngineError.Unsupported => "This codec or format is not supported.",
        EngineError.NoVideo => "The file has no video stream.",
        EngineError.Device => "The graphics or audio device failed.",
        EngineError.Cancelled => "Cancelled.",
        EngineError.Io => "A read or write error occurred.",
        EngineError.DiskFull => "The disk is full.",
        EngineError.AccessDenied => "Access was denied.",
        EngineError.NotFound => "The file no longer exists.",
        EngineError.Validation => "The output failed validation.",
        _ => "The operation failed.",
    };
}
