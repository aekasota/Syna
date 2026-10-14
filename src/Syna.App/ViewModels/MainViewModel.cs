using System.Collections.ObjectModel;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;
using Syna.Services;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Syna.ViewModels;

public enum StageState { Empty, Loading, Video, Error }

/// <summary>Window-level operations the view model needs but does not own.</summary>
public interface IShell
{
    bool IsFullscreen { get; }
    void SetFullscreen(bool on);
    Task<IReadOnlyList<string>> PickFilesAsync();
    Task<string?> PickFolderAsync();
    Task<bool> ConfirmAsync(string title, string message, string confirm);
    Task<string?> PromptAsync(string title, string initial, string confirm);
    void Toast(string message, string? actionLabel = null, Action? action = null);
    void ApplyTheme(ThemePreference theme);
}

[JsonSerializable(typeof(MixerState))]
[JsonSerializable(typeof(string[]))]
internal sealed partial class AppJson : JsonSerializerContext;

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private readonly DispatcherQueue _ui;
    private readonly SettingsStore _settingsStore = new(AppPaths.Settings);
    private readonly LowPriorityWorker _indexWorker = new("Syna metadata");
    private readonly LowPriorityWorker _thumbWorker = new("Syna thumbnails");
    private readonly DispatcherQueueTimer _persistTimer;
    private readonly DispatcherQueueTimer _settingsTimer;
    private IShell _shell = null!;
    private SessionViewModel? _active;
    private CancellationTokenSource? _waveformCts;
    private bool _disposed;

    private StageState _stage = StageState.Empty;
    private string _stageMessage = "";
    private long _positionUs;
    private long _durationUs;
    private bool _isPlaying;
    private bool _isScrubbing;
    private double _rate = 1.0;
    private bool _loop;
    private bool _masterMuted;
    private double _masterVolume = 1.0;
    private bool _explorerVisible;
    private bool _railVisible;
    private bool _audioVisible;
    private bool _infoVisible;
    private bool _diagnosticsVisible;
    private bool _paletteOpen;
    private bool _settingsOpen;
    private string _diagnosticsText = "";
    private bool _audioDeviceOk = true;
    private long _frameDurationUs = 16667;

    public MainViewModel(DispatcherQueue ui)
    {
        _ui = ui;
        Settings = _settingsStore.Load();
        Db = new AppDatabase(AppPaths.Database);
        Thumbnails = new ThumbnailService(_thumbWorker);
        Waveforms = new WaveformService();
        Explorer = new ExplorerViewModel(Db, new MediaIndex(_indexWorker, Db), ui);
        Explorer.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName != nameof(ExplorerViewModel.CurrentFolder)) return;
            string folder = Explorer.CurrentFolder;
            if (folder.Length == 0 || string.Equals(folder, Settings.LastFolder, StringComparison.OrdinalIgnoreCase)) return;
            Settings.LastFolder = folder;
            SaveSettingsSoon();
        };
        Trim = new TrimViewModel(Settings, m => Toast(m), SaveSettingsSoon);
        Watch = new WatchFolderService();
        Watch.ClipReady += path => _ui.TryEnqueue(() => OnNewClip(path));
        _persistTimer = ui.CreateTimer();
        _persistTimer.Interval = TimeSpan.FromMilliseconds(600);
        _persistTimer.IsRepeating = false;
        _persistTimer.Tick += (_, _) => PersistNow();
        _settingsTimer = ui.CreateTimer();
        _settingsTimer.Interval = TimeSpan.FromMilliseconds(400);
        _settingsTimer.IsRepeating = false;
        _settingsTimer.Tick += (_, _) => SaveSettingsNow();
        _explorerVisible = Settings.ExplorerVisible;
        _railVisible = Settings.RailVisible;
        _audioVisible = Settings.AudioPanelVisible;
        _diagnosticsVisible = Settings.ShowDiagnostics;
        _masterVolume = Settings.MasterGain;
        Playback.Event += OnEngineEvent;
        BuildCommands();
    }

    public AppSettings Settings { get; }
    public AppDatabase Db { get; }
    public ThumbnailService Thumbnails { get; }
    public WaveformService Waveforms { get; }
    public WatchFolderService Watch { get; }
    public PlaybackController Playback { get; } = new();
    public ExplorerViewModel Explorer { get; }
    public TrimViewModel Trim { get; }
    public ObservableCollection<SessionViewModel> Sessions { get; } = [];

    public void AttachShell(IShell shell) => _shell = shell;

    public SessionViewModel? ActiveSession
    {
        get => _active;
        private set
        {
            if (Set(ref _active, value))
            {
                Raise(nameof(HasSession));
                Raise(nameof(Title));
            }
        }
    }

    public bool HasSession => _active != null;
    public string Title => _active?.FileName ?? "";

    public StageState Stage
    {
        get => _stage;
        private set
        {
            if (Set(ref _stage, value))
            {
                Raise(nameof(IsEmpty));
                Raise(nameof(IsLoading));
                Raise(nameof(HasStageError));
            }
        }
    }

    public bool IsEmpty => _stage == StageState.Empty;
    public bool IsLoading => _stage == StageState.Loading;
    public bool HasStageError => _stage == StageState.Error;

    public string StageMessage
    {
        get => _stageMessage;
        private set => Set(ref _stageMessage, value);
    }

    public long PositionUs
    {
        get => _positionUs;
        private set
        {
            if (Set(ref _positionUs, value)) Raise(nameof(PositionText));
        }
    }

    public long DurationUs
    {
        get => _durationUs;
        private set
        {
            if (Set(ref _durationUs, value)) Raise(nameof(DurationText));
        }
    }

    public string PositionText => Timecode.Precise(_positionUs);
    public string DurationText => Timecode.Precise(_durationUs);

    public bool IsPlaying
    {
        get => _isPlaying;
        private set => Set(ref _isPlaying, value);
    }

    public double Rate
    {
        get => _rate;
        private set
        {
            if (Set(ref _rate, value)) Raise(nameof(RateText));
        }
    }

    public string RateText => $"{_rate:0.##}×";

    public bool LoopEnabled
    {
        get => _loop;
        set
        {
            if (Set(ref _loop, value)) ApplyLoop();
        }
    }

    public bool MasterMuted
    {
        get => _masterMuted;
        set
        {
            if (Set(ref _masterMuted, value)) Playback.Player?.SetMasterMute(value);
        }
    }

    public double MasterVolume
    {
        get => _masterVolume;
        set
        {
            value = Math.Clamp(value, 0, 1.5);
            if (!Set(ref _masterVolume, value)) return;
            Playback.Player?.SetMasterGain((float)value);
            Settings.MasterGain = value;
            SaveSettingsSoon();
            if (value > 0 && _masterMuted) MasterMuted = false;
        }
    }

    public bool ExplorerVisible
    {
        get => _explorerVisible;
        set
        {
            if (Set(ref _explorerVisible, value)) { Settings.ExplorerVisible = value; SaveSettingsSoon(); }
        }
    }

    public bool RailVisible
    {
        get => _railVisible;
        set
        {
            if (Set(ref _railVisible, value)) { Settings.RailVisible = value; SaveSettingsSoon(); }
        }
    }

    public bool AudioPanelVisible
    {
        get => _audioVisible;
        set
        {
            if (Set(ref _audioVisible, value)) { Settings.AudioPanelVisible = value; SaveSettingsSoon(); }
        }
    }

    public bool InfoVisible
    {
        get => _infoVisible;
        set => Set(ref _infoVisible, value);
    }

    public bool DiagnosticsVisible
    {
        get => _diagnosticsVisible;
        set
        {
            if (Set(ref _diagnosticsVisible, value)) { Settings.ShowDiagnostics = value; SaveSettingsSoon(); }
        }
    }

    public bool PaletteOpen
    {
        get => _paletteOpen;
        set => Set(ref _paletteOpen, value);
    }

    public bool SettingsOpen
    {
        get => _settingsOpen;
        set => Set(ref _settingsOpen, value);
    }

    public string DiagnosticsText
    {
        get => _diagnosticsText;
        private set => Set(ref _diagnosticsText, value);
    }

    public bool AudioDeviceOk
    {
        get => _audioDeviceOk;
        private set => Set(ref _audioDeviceOk, value);
    }

    public bool IsScrubbing
    {
        get => _isScrubbing;
        set => Set(ref _isScrubbing, value);
    }

    public long FrameDurationUs => _frameDurationUs;

    private long _lastDiagTicks;

    /// <summary>Called once per UI frame while playing, ~10 Hz otherwise.</summary>
    public void Tick(double seconds)
    {
        if (!Playback.IsReady) return;
        Playback.Poll();
        PlayerState s = Playback.State;
        if (s.OpenId != Playback.OpenId) return;
        IsPlaying = s.Status == PlaybackStatus.Playing;
        if (!_isScrubbing && s.Status is PlaybackStatus.Playing or PlaybackStatus.Paused or PlaybackStatus.Ended) PositionUs = s.PositionUs;
        if (s.DurationUs > 0) DurationUs = s.DurationUs;
        if (s.FrameDurationUs > 0) _frameDurationUs = s.FrameDurationUs;
        AudioDeviceOk = s.AudioDeviceOk != 0;
        if (_active != null)
        {
            int n = Math.Min(_active.Tracks.Count, EnginePlayer.MaxAudioTracks);
            unsafe
            {
                for (int i = 0; i < n; i++) _active.Tracks[i].PushPeak(s.TrackPeak[i], seconds);
            }
        }
        if (_diagnosticsVisible && Environment.TickCount64 - _lastDiagTicks > 250)
        {
            _lastDiagTicks = Environment.TickCount64;
            UpdateDiagnostics(s);
        }
    }

    private PlayerDiagnostics? _diag;
    private ProcessStats _proc;
    private long _lastProcTicks;

    private void UpdateDiagnostics(in PlayerState s)
    {
        _diag = Playback.Diagnostics();
        if (Environment.TickCount64 - _lastProcTicks > 1000)
        {
            _lastProcTicks = Environment.TickCount64;
            _ = Task.Run(() =>
            {
                var stats = MediaEngine.SampleProcessStats();
                _ui.TryEnqueue(() => _proc = stats);
            });
        }
        var v = _active?.Info?.Video;
        var sb = new StringBuilder();
        sb.AppendLine($"Decoder      {(_diag?.Decoder is { Length: > 0 } d ? d : s.Decoder.ToString())}");
        sb.AppendLine($"GPU          {_diag?.Gpu}");
        sb.AppendLine($"Renderer     D3D11 · {v?.Width}×{v?.Height} {v?.PixelFormat}");
        sb.AppendLine($"Source FPS   {v?.FpsAvg:0.###}{(v?.Vfr == true ? " (variable timing)" : "")}");
        sb.AppendLine($"Presented    {s.PresentedFps:0.0} fps");
        sb.AppendLine($"Dropped      {s.FramesDropped} of {s.FramesPresented + s.FramesDropped}");
        sb.AppendLine($"A/V drift    {s.AvDriftMs:+0.0;-0.0;0.0} ms");
        sb.AppendLine($"Audio        {_diag?.AudioFormat} · buffer {s.AudioBufferMs:0} ms · queued {s.AudioLatencyMs:0} ms");
        sb.AppendLine($"Underruns    {s.AudioUnderruns} · device starved {_diag?.AudioStarvations ?? 0}");
        foreach (var t in _diag?.AudioTracks ?? [])
        {
            string state = t.State == "decoding" ? "" : $"{t.State} · ";
            sb.AppendLine($"{$"Track {t.Ordinal + 1}",-13}{state}ahead {t.BufferedUs / 1000.0:0} ms · drift {t.DriftMs:+0.0;-0.0;0.0} ms · " +
                          $"stretch {t.StretchedUs / 1000.0:0} ms · jumps {t.Discontinuities} · underruns {t.Underruns}");
        }
        sb.AppendLine($"Video queue  {s.VideoQueue}/{s.VideoQueueMax} · submit {s.RenderMs:0.00} ms");
        sb.AppendLine($"Limiter      {(s.LimiterGainReductionDb > 0.05 ? $"-{s.LimiterGainReductionDb:0.0} dB" : "idle")}");
        sb.AppendLine($"CPU          {_proc.CpuPercent:0.0} % (all cores)");
        sb.AppendLine($"GPU decode   {_proc.GpuDecodePercent:0.0} %   3D {_proc.Gpu3DPercent:0.0} %   encode {_proc.GpuEncodePercent:0.0} %");
        sb.Append($"Memory       {_proc.PrivateBytes / 1048576.0:0} MB private · {_proc.GpuDedicatedBytes / 1048576.0:0} MB VRAM");
        DiagnosticsText = sb.ToString();
    }

    private void OnEngineEvent(EngineEvent e)
    {
        if (e.OpenId != Playback.OpenId) return;
        var session = _active;
        switch (e.Type)
        {
            case EngineEventType.Opened when session != null:
                session.Error = null;
                session.IsMissing = false;
                session.Info = Playback.MediaInfo();
                DurationUs = e.Value;
                BuildTracks(session);
                Stage = StageState.Video;
                ApplyLoop();
                LoadWaveform(session);
                break;
            case EngineEventType.OpenFailed when session != null:
                var error = (EngineError)e.Code;
                session.IsMissing = error == EngineError.NotFound;
                session.Error = error == EngineError.NotFound ? "This file no longer exists." : MediaEngine.Describe(error);
                StageMessage = session.Error;
                Stage = StageState.Error;
                Log.Warn("playback", $"open failed {error} \"{session.Path}\"");
                break;
            case EngineEventType.DecoderFallback:
                Toast("Hardware decoding is not available for this clip; using the CPU.");
                break;
            case EngineEventType.AudioDevice:
                if (e.Code == 0) Toast("No audio output device. Playback continues without sound.");
                break;
            case EngineEventType.SourceError when session != null:
                session.IsMissing = !File.Exists(session.Path);
                Toast(session.IsMissing ? "The file was moved or deleted while open." : "The file could not be read.");
                break;
            case EngineEventType.DeviceLost:
                RecoverFromDeviceLoss();
                break;
        }
    }

    private void RecoverFromDeviceLoss()
    {
        var session = _active;
        long at = PositionUs;
        Playback.Recreate();
        ApplyAudioSettings();
        if (session != null)
        {
            ApplyTrackSettings(session);
            Playback.Open(session.Path, at, autoplay: false, Settings.HardwareDecoding);
        }
        Toast("The graphics device was reset. Playback was restored.");
    }

    public void ApplyAudioSettings()
    {
        var p = Playback.Player;
        if (p == null) return;
        p.SetMasterGain((float)_masterVolume);
        p.SetMasterMute(_masterMuted);
        p.SetLimiter(Settings.Limiter);
        p.SetAvOffset(Settings.AvOffsetMs * 1000L);
        p.SetRate(_rate);
    }

    private void PersistSoon() => _persistTimer.Start();

    private void PersistNow()
    {
        var s = _active;
        if (s != null && s.Tracks.Count > 0)
        {
            string[] labels = s.Tracks.Select(t => t.LabelIsSuggestion ? "" : t.Label).ToArray();
            Db.SaveMixer(s.Path, JsonSerializer.Serialize(s.Mixer, AppJson.Default.MixerState), JsonSerializer.Serialize(labels, AppJson.Default.StringArray));
        }
        Db.SaveSessions(Sessions.Select(x => x.Path).ToList(), _active == null ? -1 : Sessions.IndexOf(_active));
    }

    public void SaveSettingsSoon() => _settingsTimer.Start();

    // A clip watched to (nearly) its end reopens from the start instead of on its last frame.
    private void SavePosition(SessionViewModel session, long positionUs)
    {
        if (!Settings.RememberPosition) return;
        long duration = session.DurationUs;
        bool finished = duration > 0 && positionUs >= duration - Math.Max(1_000_000, duration * 3 / 100);
        Db.SavePosition(session.Path, finished ? 0 : positionUs);
    }

    private void SaveSettingsNow()
    {
        try { _settingsStore.Save(Settings); }
        catch (IOException ex) { Log.Warn("settings", ex.Message); }
    }

    public void Toast(string message, string? action = null, Action? onAction = null) => _shell?.Toast(message, action, onAction);

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_active != null)
        {
            _active.PositionUs = PositionUs;
            SavePosition(_active, PositionUs);
        }
        PersistNow();
        SaveSettingsNow();
        _waveformCts?.Cancel();
        Watch.Dispose();
        Waveforms.Dispose();
        Playback.Dispose();
        _indexWorker.Dispose();
        _thumbWorker.Dispose();
        Db.Dispose();
    }

    private static ImageSource? LoadThumbnail(string? file)
    {
        if (file == null) return null;
        return new BitmapImage(new Uri(file)) { DecodePixelWidth = 384, DecodePixelType = DecodePixelType.Logical };
    }
}
