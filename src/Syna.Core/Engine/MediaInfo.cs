using System.Text.Json.Serialization;

namespace Syna.Core.Engine;

// JSON schema produced by media/media_info.cpp. Times are on the file timeline (container start = 0).
public sealed record MediaInfo
{
    public string Path { get; init; } = "";
    public string Container { get; init; } = "";
    public string ContainerLong { get; init; } = "";
    public long DurationUs { get; init; }
    public long SizeBytes { get; init; }
    public long BitRate { get; init; }
    public long StartUs { get; init; }
    public VideoStreamInfo? Video { get; init; }
    public AudioTrackInfo[] Audio { get; init; } = [];

    public TimeSpan Duration => TimeSpan.FromTicks(DurationUs * 10);
}

public sealed record VideoStreamInfo
{
    public int StreamIndex { get; init; }
    public string Codec { get; init; } = "";
    public string CodecLong { get; init; } = "";
    public string Profile { get; init; } = "";
    public int Level { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int SarNum { get; init; } = 1;
    public int SarDen { get; init; } = 1;
    public string PixelFormat { get; init; } = "";
    public int BitDepth { get; init; }
    public string Chroma { get; init; } = "";
    public string ColorRange { get; init; } = "";
    public string ColorSpace { get; init; } = "";
    public string ColorPrimaries { get; init; } = "";
    public string ColorTransfer { get; init; } = "";
    public string Hdr { get; init; } = "";
    public double FpsAvg { get; init; }
    public double FpsNominal { get; init; }
    public bool Vfr { get; init; }
    public double FrameIntervalMinMs { get; init; }
    public double FrameIntervalMaxMs { get; init; }
    public long FrameCount { get; init; }
    public long BitRate { get; init; }
    public int HasBFrames { get; init; }
    public double Rotation { get; init; }
    public double KeyframeIntervalS { get; init; }
    public long StartUs { get; init; }
    public long[] KeyframesUs { get; init; } = [];
}

public sealed record AudioTrackInfo
{
    public int StreamIndex { get; init; }
    public string Codec { get; init; } = "";
    public string Profile { get; init; } = "";
    public int SampleRate { get; init; }
    public int Channels { get; init; }
    public string ChannelLayout { get; init; } = "";
    public string SampleFormat { get; init; } = "";
    public int BitsPerSample { get; init; }
    public long BitRate { get; init; }
    public string Title { get; init; } = "";
    public string Language { get; init; } = "";
    public string Label { get; init; } = "";
    public bool LabelIsSuggestion { get; init; }
    public long StartUs { get; init; }
    public long DurationUs { get; init; }
}

public sealed record PlayerDiagnostics
{
    public string Decoder { get; init; } = "";
    public string Gpu { get; init; } = "";
    public string GpuVendor { get; init; } = "";
    public string Renderer { get; init; } = "";
    public string AudioFormat { get; init; } = "";
    public bool AudioDevice { get; init; }
    public double AudioBufferMs { get; init; }
    public double AudioQueuedMs { get; init; }
    public long AudioUnderruns { get; init; }
    public long AudioStarvations { get; init; }
    public AudioTrackDiagnostics[] AudioTracks { get; init; } = [];
}

// Per-track playback health (media/pipeline.h AudioTrackStats).
public sealed record AudioTrackDiagnostics
{
    public int Ordinal { get; init; }
    public string State { get; init; } = "";
    public long? PtsUs { get; init; }
    public long BufferedUs { get; init; }
    public double DriftMs { get; init; }
    public long Discontinuities { get; init; }
    public long GapUs { get; init; }
    public long OverlapUs { get; init; }
    public long StretchedUs { get; init; }
    public long Underruns { get; init; }
    public string Codec { get; init; } = "";
    public int SampleRate { get; init; }
    public int Channels { get; init; }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true)]
[JsonSerializable(typeof(MediaInfo))]
[JsonSerializable(typeof(PlayerDiagnostics))]
[JsonSerializable(typeof(TrimReport))]
[JsonSerializable(typeof(SystemInfo))]
internal sealed partial class EngineJson : JsonSerializerContext;
