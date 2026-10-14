using System.Text.Json;

namespace Syna.Core.Engine;

/// <summary>
/// The native playback surface. Create and attach on the UI thread; every other call only enqueues
/// work or reads state, so none of them block on media work.
/// </summary>
public sealed class EnginePlayer : IDisposable
{
    public const int MaxAudioTracks = Native.MaxAudioTracks;

    private nint _handle;

    private EnginePlayer(nint handle) => _handle = handle;

    public static EnginePlayer Create()
    {
        MediaEngine.Check(Native.PlayerCreate(out nint h));
        return new EnginePlayer(h);
    }

    private nint H => _handle != 0 ? _handle : throw new ObjectDisposedException(nameof(EnginePlayer));

    /// <param name="swapChainPanel">IUnknown of a Microsoft.UI.Xaml.Controls.SwapChainPanel.</param>
    public void AttachPanel(nint swapChainPanel) => MediaEngine.Check(Native.PlayerAttachPanel(H, swapChainPanel));

    public void Resize(int widthPx, int heightPx, float scaleX, float scaleY) => Native.PlayerResize(H, widthPx, heightPx, scaleX, scaleY);

    public void SetBackground(float r, float g, float b) => Native.PlayerSetBackground(H, r, g, b);

    public long Open(string path, TimeSpan start, bool autoplay, bool hardwareDecode)
    {
        var options = new Native.OpenOptions
        {
            StartUs = start.Ticks / 10,
            Autoplay = autoplay ? 1 : 0,
            HwDecode = hardwareDecode ? 1 : 0,
        };
        return Native.PlayerOpen(H, path, in options);
    }

    public void Close() => Native.PlayerClose(H);
    public void Play() => Native.PlayerPlay(H);
    public void Pause() => Native.PlayerPause(H);
    public void Seek(long positionUs, SeekMode mode) => Native.PlayerSeek(H, positionUs, (int)mode);
    public void Step(int frames) => Native.PlayerStep(H, frames);
    public void SetRate(double rate) => Native.PlayerSetRate(H, rate);
    public void SetLoop(bool enabled, long aUs, long bUs) => Native.PlayerSetLoop(H, enabled ? 1 : 0, aUs, bUs);

    public void SetTrackGain(int track, float gain) => Native.PlayerSetTrackGain(H, track, gain);
    public void SetTrackMute(int track, bool muted) => Native.PlayerSetTrackMute(H, track, muted ? 1 : 0);
    public void SetTrackSolo(int track, bool solo) => Native.PlayerSetTrackSolo(H, track, solo ? 1 : 0);
    public void SetTrackOffset(int track, long offsetUs) => Native.PlayerSetTrackOffset(H, track, offsetUs);
    public void SetMasterGain(float gain) => Native.PlayerSetMasterGain(H, gain);
    public void SetMasterMute(bool muted) => Native.PlayerSetMasterMute(H, muted ? 1 : 0);
    public void SetLimiter(bool enabled) => Native.PlayerSetLimiter(H, enabled ? 1 : 0);
    public void SetAvOffset(long offsetUs) => Native.PlayerSetAvOffset(H, offsetUs);

    public PlayerState GetState()
    {
        Native.PlayerGetState(H, out PlayerState s);
        return s;
    }

    public unsafe int PollEvents(Span<EngineEvent> buffer)
    {
        fixed (EngineEvent* p = buffer) return Native.PlayerPollEvents(H, p, buffer.Length);
    }

    public MediaInfo? GetMediaInfo()
    {
        if (Native.PlayerMediaJson(H, out nint json) != 0) return null;
        return JsonSerializer.Deserialize(Native.TakeString(json)!, EngineJson.Default.MediaInfo);
    }

    public PlayerDiagnostics? GetDiagnostics()
    {
        if (Native.PlayerDiagnosticsJson(H, out nint json) != 0) return null;
        return JsonSerializer.Deserialize(Native.TakeString(json)!, EngineJson.Default.PlayerDiagnostics);
    }

    /// <summary>Blocking (GPU readback + encode); call from a worker thread.</summary>
    public void SaveFrame(string path, bool jpeg) => MediaEngine.Check(Native.PlayerSaveFrame(H, path, jpeg ? 1 : 0));

    public void Dispose()
    {
        nint h = Interlocked.Exchange(ref _handle, 0);
        if (h != 0) Native.PlayerDestroy(h);
    }
}
