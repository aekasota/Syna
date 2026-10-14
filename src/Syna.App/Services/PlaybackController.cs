using System.Diagnostics;
using Syna.Core.Engine;
using Microsoft.UI.Xaml.Controls;

namespace Syna.Services;

/// <summary>
/// Owns the native player for the video surface. The UI calls Poll() once per UI frame (or slower
/// when paused); that single call is the only per-frame interop.
/// </summary>
public sealed class PlaybackController : IDisposable
{
    private EnginePlayer? _player;
    private SwapChainPanel? _panel;
    private readonly EngineEvent[] _events = new EngineEvent[32];
    private long _openStarted;          // Stopwatch timestamp of an open whose first frame is not shown yet
    private static bool s_launchLogged;

    public PlayerState State { get; private set; }
    public long OpenId { get; private set; }
    public bool IsReady => _player != null;

    public event Action<EngineEvent>? Event;

    public void Attach(SwapChainPanel panel)
    {
        _panel = panel;
        _player = EnginePlayer.Create();
        nint unknown = ((WinRT.IWinRTObject)panel).NativeObject.ThisPtr;
        _player.AttachPanel(unknown);
        GC.KeepAlive(panel);
    }

    /// <summary>Recovers from GPU device loss: a fresh device, swap chain and player.</summary>
    public void Recreate()
    {
        _player?.Dispose();
        _player = null;
        if (_panel != null) Attach(_panel);
        Log.Warn("playback", "player recreated after device loss");
    }

    public void Resize(int widthPx, int heightPx, float scaleX, float scaleY) => _player?.Resize(widthPx, heightPx, scaleX, scaleY);

    public void SetBackground(Windows.UI.Color c) => _player?.SetBackground(c.R / 255f, c.G / 255f, c.B / 255f);

    public long Open(string path, long startUs, bool autoplay, bool hardwareDecode)
    {
        if (_player == null) return 0;
        _openStarted = Stopwatch.GetTimestamp();
        OpenId = _player.Open(path, TimeSpan.FromTicks(startUs * 10), autoplay, hardwareDecode);
        Log.Info("playback", $"open id={OpenId} start_us={startUs} \"{path}\"");
        return OpenId;
    }

    public void Poll()
    {
        if (_player == null) return;
        State = _player.GetState();
        int n = _player.PollEvents(_events);
        for (int i = 0; i < n; i++)
        {
            if (_events[i].Type == EngineEventType.SeekDone && _events[i].OpenId == OpenId && _openStarted != 0) LogFirstFrame();
            Event?.Invoke(_events[i]);
        }
    }

    // Time to first frame, for docs/PERFORMANCE.md and field diagnostics.
    private void LogFirstFrame()
    {
        double ms = Stopwatch.GetElapsedTime(_openStarted).TotalMilliseconds;
        _openStarted = 0;
        string launch = "";
        if (!s_launchLogged)
        {
            s_launchLogged = true;
            using var self = Process.GetCurrentProcess();
            launch = $", {(DateTime.Now - self.StartTime).TotalMilliseconds:0} ms after process start";
        }
        Log.Info("playback", $"first frame id={OpenId} {ms:0} ms after open{launch}");
    }

    public EnginePlayer? Player => _player;

    public void Close() => _player?.Close();
    public void Play() => _player?.Play();
    public void Pause() => _player?.Pause();
    public void Seek(long us, bool exact) => _player?.Seek(us, exact ? SeekMode.Exact : SeekMode.Fast);
    public void Step(int frames) => _player?.Step(frames);
    public void SetRate(double rate) => _player?.SetRate(rate);
    public void SetLoop(bool enabled, long aUs, long bUs) => _player?.SetLoop(enabled, aUs, bUs);
    public MediaInfo? MediaInfo() => _player?.GetMediaInfo();
    public PlayerDiagnostics? Diagnostics() => _player?.GetDiagnostics();

    public void Dispose()
    {
        _player?.Dispose();
        _player = null;
    }
}
