using System.Runtime.InteropServices;

namespace Syna.Core.Engine;

// Mirrors include/syna_api.h. Every call is command/state oriented; nothing here runs per frame
// or per audio buffer.
internal static unsafe partial class Native
{
    private const string Lib = "Syna.Native";

    public const int MaxAudioTracks = 8;

    [LibraryImport(Lib, EntryPoint = "syna_api_version")]
    public static partial uint ApiVersion();

    [LibraryImport(Lib, EntryPoint = "syna_initialize", StringMarshalling = StringMarshalling.Utf16)]
    public static partial int Initialize(string? logDir, int verbose);

    [LibraryImport(Lib, EntryPoint = "syna_shutdown")]
    public static partial void Shutdown();

    [LibraryImport(Lib, EntryPoint = "syna_last_error")]
    public static partial nint LastError();

    [LibraryImport(Lib, EntryPoint = "syna_free_string")]
    public static partial void FreeString(nint s);

    [LibraryImport(Lib, EntryPoint = "syna_query_system_json")]
    public static partial int QuerySystemJson(out nint json);

    [LibraryImport(Lib, EntryPoint = "syna_probe_json", StringMarshalling = StringMarshalling.Utf16)]
    public static partial int ProbeJson(string path, int mode, out nint json);

    [LibraryImport(Lib, EntryPoint = "syna_make_thumbnail", StringMarshalling = StringMarshalling.Utf16)]
    public static partial int MakeThumbnail(string path, string outJpeg, int maxWidth, long atUs);

    [LibraryImport(Lib, EntryPoint = "syna_player_create")]
    public static partial int PlayerCreate(out nint player);

    [LibraryImport(Lib, EntryPoint = "syna_player_destroy")]
    public static partial void PlayerDestroy(nint player);

    [LibraryImport(Lib, EntryPoint = "syna_player_attach_panel")]
    public static partial int PlayerAttachPanel(nint player, nint panel);

    [LibraryImport(Lib, EntryPoint = "syna_player_resize")]
    public static partial int PlayerResize(nint player, int width, int height, float scaleX, float scaleY);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_background")]
    public static partial int PlayerSetBackground(nint player, float r, float g, float b);

    [LibraryImport(Lib, EntryPoint = "syna_player_open", StringMarshalling = StringMarshalling.Utf16)]
    public static partial long PlayerOpen(nint player, string path, in OpenOptions options);

    [LibraryImport(Lib, EntryPoint = "syna_player_close")]
    public static partial int PlayerClose(nint player);

    [LibraryImport(Lib, EntryPoint = "syna_player_media_json")]
    public static partial int PlayerMediaJson(nint player, out nint json);

    [LibraryImport(Lib, EntryPoint = "syna_player_play")]
    public static partial int PlayerPlay(nint player);

    [LibraryImport(Lib, EntryPoint = "syna_player_pause")]
    public static partial int PlayerPause(nint player);

    [LibraryImport(Lib, EntryPoint = "syna_player_seek")]
    public static partial int PlayerSeek(nint player, long positionUs, int mode);

    [LibraryImport(Lib, EntryPoint = "syna_player_step")]
    public static partial int PlayerStep(nint player, int frames);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_rate")]
    public static partial int PlayerSetRate(nint player, double rate);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_loop")]
    public static partial int PlayerSetLoop(nint player, int enabled, long aUs, long bUs);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_track_gain")]
    public static partial int PlayerSetTrackGain(nint player, int track, float gain);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_track_mute")]
    public static partial int PlayerSetTrackMute(nint player, int track, int muted);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_track_solo")]
    public static partial int PlayerSetTrackSolo(nint player, int track, int solo);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_track_offset")]
    public static partial int PlayerSetTrackOffset(nint player, int track, long offsetUs);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_master_gain")]
    public static partial int PlayerSetMasterGain(nint player, float gain);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_master_mute")]
    public static partial int PlayerSetMasterMute(nint player, int muted);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_limiter")]
    public static partial int PlayerSetLimiter(nint player, int enabled);

    [LibraryImport(Lib, EntryPoint = "syna_player_set_av_offset")]
    public static partial int PlayerSetAvOffset(nint player, long offsetUs);

    [LibraryImport(Lib, EntryPoint = "syna_player_get_state")]
    public static partial int PlayerGetState(nint player, out PlayerState state);

    [LibraryImport(Lib, EntryPoint = "syna_player_diagnostics_json")]
    public static partial int PlayerDiagnosticsJson(nint player, out nint json);

    [LibraryImport(Lib, EntryPoint = "syna_player_poll_events")]
    public static partial int PlayerPollEvents(nint player, EngineEvent* events, int capacity);

    [LibraryImport(Lib, EntryPoint = "syna_player_save_frame", StringMarshalling = StringMarshalling.Utf16)]
    public static partial int PlayerSaveFrame(nint player, string path, int format);

    [LibraryImport(Lib, EntryPoint = "syna_waveform_start", StringMarshalling = StringMarshalling.Utf16)]
    public static partial int WaveformStart(string path, string outFile, int bucketsPerSecond, out nint job);

    [LibraryImport(Lib, EntryPoint = "syna_trim_start")]
    public static partial int TrimStart(in TrimRequestNative request, out nint job);

    [LibraryImport(Lib, EntryPoint = "syna_job_status_get")]
    public static partial int JobStatus(nint job, out JobStatusNative status);

    [LibraryImport(Lib, EntryPoint = "syna_job_result_json")]
    public static partial int JobResultJson(nint job, out nint json);

    [LibraryImport(Lib, EntryPoint = "syna_job_cancel")]
    public static partial void JobCancel(nint job);

    [LibraryImport(Lib, EntryPoint = "syna_job_destroy")]
    public static partial void JobDestroy(nint job);

    [LibraryImport(Lib, EntryPoint = "syna_sample_process_stats")]
    public static partial int SampleProcessStats(out ProcessStats stats);

    public static string? TakeString(nint p)
    {
        if (p == 0) return null;
        try { return Marshal.PtrToStringUTF8(p); }
        finally { FreeString(p); }
    }

    public static string LastErrorMessage() => Marshal.PtrToStringUTF8(LastError()) ?? string.Empty;

    [StructLayout(LayoutKind.Sequential)]
    public struct OpenOptions
    {
        public long StartUs;
        public int Autoplay;
        public int HwDecode;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TrimRequestNative
    {
        public nint Source;
        public nint Destination;
        public long InUs;
        public long OutUs;
        public int Mode;
        public int AllowHwEncode;
        public int Video;
        public int VideoCodec;
        public int VideoHeight;
        public int VideoFpsNum;
        public int VideoFpsDen;
        public int VideoQuality;
        public int VideoBitrateKbps;
        public int VideoMaxBitrateKbps;
        public int VideoSpeed;
        public int Audio;
        public int AudioCodec;
        public int AudioBitrateKbps;
        public int AudioSampleRate;
        public int AudioChannels;
        public nint Tracks;  // TrimTrackNative[TrackCount]
        public int TrackCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TrimTrackNative
    {
        public int StreamIndex;
        public float Gain;
        public long OffsetUs;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JobStatusNative
    {
        public int State;
        public int Error;
        public double Progress;
    }
}

public enum PlaybackStatus { Idle = 0, Opening = 1, Paused = 2, Playing = 3, Ended = 4, Error = 5 }

public enum DecoderBackend { None = 0, Software = 1, D3D11VA = 2 }

public enum SeekMode { Exact = 0, Fast = 1 }

public enum EngineEventType
{
    Opened = 1,
    OpenFailed = 2,
    SeekDone = 3,
    Ended = 4,
    Error = 5,
    AudioDevice = 6,
    DecoderFallback = 7,
    DeviceLost = 8,
    SourceError = 9,
    Looped = 10,
}

[StructLayout(LayoutKind.Sequential)]
public struct EngineEvent
{
    public EngineEventType Type;
    public int Code;
    public long OpenId;
    public long Value;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PlayerState
{
    public PlaybackStatus Status;
    public DecoderBackend Decoder;
    public long OpenId;
    public long PositionUs;
    public long DurationUs;
    public long FramePtsUs;
    public long FrameDurationUs;
    public double Rate;
    public int Seeking;
    public int AudioTracks;
    public int AudioDeviceOk;
    public int LoopEnabled;
    public fixed float TrackPeak[Native.MaxAudioTracks];
    public fixed float MasterPeak[2];
    public float LimiterGainReductionDb;
    public long FramesPresented;
    public long FramesDropped;
    public long AudioUnderruns;
    public double PresentedFps;
    public double AvDriftMs;
    public double AudioBufferMs;
    public double AudioLatencyMs;
    public double RenderMs;
    public int VideoQueue;
    public int VideoQueueMax;

    public readonly bool HasFrame => FramePtsUs != long.MinValue;
}

[StructLayout(LayoutKind.Sequential)]
public struct ProcessStats
{
    public double CpuPercent;
    public double GpuDecodePercent;
    public double Gpu3DPercent;
    public double GpuEncodePercent;
    public long PrivateBytes;
    public long WorkingSetBytes;
    public long GpuDedicatedBytes;
}
