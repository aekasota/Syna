using System.Text.Json;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;
using Syna.Services;
using Microsoft.UI.Xaml.Media;

namespace Syna.ViewModels;

public sealed partial class MainViewModel
{
    /// <summary>Opens clips as sessions (or switches to them if already open) and navigates folders.</summary>
    public void OpenPaths(IEnumerable<string> paths, bool activate = true)
    {
        SessionViewModel? first = null;
        foreach (string raw in paths)
        {
            string path;
            try { path = Path.GetFullPath(raw); }
            catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException) { continue; }
            if (Directory.Exists(path))
            {
                ExplorerVisible = true;
                _ = Explorer.NavigateAsync(path);
                continue;
            }
            if (!MediaFiles.IsSupported(path))
            {
                Toast($"{Path.GetFileName(path)} is not a supported video file.");
                continue;
            }
            var session = Sessions.FirstOrDefault(s => string.Equals(s.Path, path, StringComparison.OrdinalIgnoreCase));
            if (session == null)
            {
                session = CreateSession(path);
                Sessions.Add(session);
            }
            first ??= session;
        }
        if (first != null && activate) Activate(first);
        PersistSoon();
    }

    private SessionViewModel CreateSession(string path)
    {
        var session = new SessionViewModel(path);
        var record = Db.GetFile(path);
        session.IsFavorite = record?.Favorite ?? false;
        session.PositionUs = Settings.RememberPosition ? record?.LastPositionUs ?? 0 : 0;
        session.IsMissing = !File.Exists(path);
        if (!session.IsMissing) RequestThumbnail(session);
        return session;
    }

    private void RequestThumbnail(SessionViewModel session)
    {
        if (!Settings.Thumbnails) return;
        _ = Thumbnails.GetAsync(session.Path).ContinueWith(t =>
        {
            string? file = t.Result;
            _ui.TryEnqueue(() => { if (!session.IsClosed) session.Thumbnail = LoadThumbnail(file); });
        }, TaskScheduler.Default);
        // Duration for the rail without opening the file in the player.
        _indexWorker.Post(() =>
        {
            if (session.IsClosed) return;  // closed before its turn came
            if (MediaEngine.TryProbe(session.Path, full: false, out var info, out _) && info != null)
                _ui.TryEnqueue(() => { if (!session.IsClosed && session.DurationUs == 0) session.DurationUs = info.DurationUs; });
        });
    }

    public void Activate(SessionViewModel session)
    {
        if (!Playback.IsReady) return;
        var previous = _active;
        bool autoplay = Settings.Autoplay;
        if (previous != null && previous != session)
        {
            previous.PositionUs = PositionUs;
            previous.WasPlaying = IsPlaying;
            autoplay = IsPlaying;
            SavePosition(previous, PositionUs);
            PersistNow();
            previous.IsActive = false;
            foreach (var t in previous.Tracks) t.ResetLevel();
        }
        else if (previous == session && Stage != StageState.Error)
        {
            return;
        }
        ActiveSession = session;
        session.IsActive = true;
        session.Error = null;
        StageMessage = "";
        _waveformCts?.Cancel();
        PositionUs = session.PositionUs;
        DurationUs = session.DurationUs;
        ApplyTrackSettings(session);
        long start = session.PositionUs;
        if (session.DurationUs > 0 && start >= session.DurationUs - 1_000_000) start = 0;
        Stage = StageState.Loading;
        Playback.Open(session.Path, start, autoplay, Settings.HardwareDecoding);
        Db.MarkOpened(session.Path);
        PersistSoon();
    }

    public void CloseSession(SessionViewModel session)
    {
        int index = Sessions.IndexOf(session);
        if (index < 0) return;
        SavePosition(session, session == _active ? PositionUs : session.PositionUs);
        session.IsClosed = true;
        if (session == _active)
        {
            PersistNow();
            Sessions.RemoveAt(index);
            ActiveSession = null;
            if (Sessions.Count > 0) Activate(Sessions[Math.Min(index, Sessions.Count - 1)]);
            else
            {
                Playback.Close();
                Stage = StageState.Empty;
                PositionUs = 0;
                DurationUs = 0;
            }
        }
        else
        {
            Sessions.RemoveAt(index);
        }
        PersistSoon();
    }

    public void CloseOthers(SessionViewModel keep)
    {
        foreach (var s in Sessions.Where(s => s != keep).ToList()) CloseSession(s);
    }

    /// <summary>Asks once (when more than one clip is open), then closes every clip.</summary>
    public async Task CloseAllSessionsAsync()
    {
        int n = Sessions.Count;
        if (n == 0) return;
        if (n > 1 && !await _shell.ConfirmAsync($"Close all {n} clips?", "They are removed from the clip list. The files, stars and history stay as they are.", "Close all"))
            return;
        CloseAllSessions();
    }

    /// <summary>
    /// Closes every open clip in one step. Playback stops and releases the file and its decoders,
    /// waveform work stops, queued thumbnail and metadata work for the clips is skipped, and the
    /// rail is cleared with a single reset (closing them one by one would refresh the list once per
    /// clip). Only the open sessions go: files, stars, history and caches are untouched.
    /// </summary>
    public void CloseAllSessions()
    {
        int n = Sessions.Count;
        if (n == 0) return;
        if (_active != null) SavePosition(_active, PositionUs);  // the others were saved when they were last active
        _waveformCts?.Cancel();
        Playback.Close();
        foreach (var s in Sessions) s.IsClosed = true;
        if (_active != null)
        {
            _active.IsActive = false;
            foreach (var t in _active.Tracks) t.ResetLevel();
        }
        ActiveSession = null;
        Sessions.Clear();
        Stage = StageState.Empty;
        StageMessage = "";
        PositionUs = 0;
        DurationUs = 0;
        IsPlaying = false;
        PersistNow();
        Log.Info("sessions", n == 1 ? "closed the only clip" : $"closed all {n} clips");
    }

    public void MoveSession(SessionViewModel session, int delta)
    {
        int i = Sessions.IndexOf(session), j = i + delta;
        if (i < 0 || j < 0 || j >= Sessions.Count) return;
        Sessions.Move(i, j);
        PersistSoon();
    }

    public void CycleSession(int delta)
    {
        if (Sessions.Count < 2 || _active == null) return;
        int i = Sessions.IndexOf(_active);
        Activate(Sessions[(i + delta + Sessions.Count) % Sessions.Count]);
    }

    public void RestoreSessionsWithoutActivating()
    {
        if (!Settings.RestoreSessions) return;
        foreach (string p in Db.LoadSessions().Paths)
        {
            if (Sessions.Any(s => string.Equals(s.Path, p, StringComparison.OrdinalIgnoreCase))) continue;
            Sessions.Add(CreateSession(p));
        }
    }

    public void RestoreSessions()
    {
        if (!Settings.RestoreSessions) return;
        int active = Db.LoadSessions().ActiveIndex;
        RestoreSessionsWithoutActivating();
        if (Sessions.Count == 0) return;
        var target = active >= 0 && active < Sessions.Count ? Sessions[active] : Sessions[0];
        bool autoplay = Settings.Autoplay;
        Settings.Autoplay = false;  // restored sessions open paused
        Activate(target);
        Settings.Autoplay = autoplay;
    }

    private void BuildTracks(SessionViewModel session)
    {
        var info = session.Info;
        if (info == null) return;
        var record = Db.GetFile(session.Path);
        MixerState? saved = null;
        string[]? labels = null;
        try
        {
            if (record?.MixerJson != null) saved = JsonSerializer.Deserialize(record.MixerJson, AppJson.Default.MixerState);
            if (record?.LabelsJson != null) labels = JsonSerializer.Deserialize(record.LabelsJson, AppJson.Default.StringArray);
        }
        catch (JsonException)
        {
        }
        session.Tracks.Clear();
        for (int i = 0; i < Math.Min(info.Audio.Length, EnginePlayer.MaxAudioTracks); i++)
        {
            var a = info.Audio[i];
            string detail = $"{a.Codec.ToUpperInvariant()} · {a.SampleRate / 1000.0:0.#} kHz · {a.ChannelLayout}";
            var track = new AudioTrackViewModel(i, a.Label, a.LabelIsSuggestion, detail, new SolidColorBrush(ThemeColors.Track(i)), OnTrackChanged);
            double gain = saved != null && i < saved.Gain.Length ? saved.Gain[i] : Settings.DefaultGain(i);
            track.Load(gain,
                saved != null && i < saved.Mute.Length && saved.Mute[i],
                saved != null && i < saved.Solo.Length && saved.Solo[i],
                saved != null && i < saved.OffsetMs.Length ? saved.OffsetMs[i] : 0,
                labels != null && i < labels.Length ? labels[i] : null);
            session.Tracks.Add(track);
        }
        ApplyTrackSettings(session);
    }

    private void ApplyTrackSettings(SessionViewModel session)
    {
        var p = Playback.Player;
        if (p == null) return;
        for (int i = 0; i < EnginePlayer.MaxAudioTracks; i++)
        {
            var t = i < session.Tracks.Count ? session.Tracks[i] : null;
            p.SetTrackGain(i, (float)(t?.Gain ?? Settings.DefaultGain(i)));
            p.SetTrackMute(i, t?.Muted ?? false);
            p.SetTrackSolo(i, t?.Soloed ?? false);
            p.SetTrackOffset(i, (t?.OffsetMs ?? 0) * 1000L);
        }
    }

    private void OnTrackChanged(AudioTrackViewModel track)
    {
        var p = Playback.Player;
        if (p != null)
        {
            p.SetTrackGain(track.Index, (float)track.Gain);
            p.SetTrackMute(track.Index, track.Muted);
            p.SetTrackSolo(track.Index, track.Soloed);
            p.SetTrackOffset(track.Index, track.OffsetMs * 1000L);
        }
        PersistSoon();
    }

    public void RefreshThemeColors()
    {
        foreach (var s in Sessions)
            foreach (var t in s.Tracks) t.SetColor(new SolidColorBrush(ThemeColors.Track(t.Index)));
    }

    private void LoadWaveform(SessionViewModel session)
    {
        if (!Settings.Waveforms || session.Waveform != null || session.Tracks.Count == 0) return;
        _waveformCts = new CancellationTokenSource();
        var token = _waveformCts.Token;
        _ = Waveforms.GetAsync(session.Path, token).ContinueWith(t =>
        {
            if (t.IsCompletedSuccessfully && t.Result != null) _ui.TryEnqueue(() => { if (!session.IsClosed) session.Waveform = t.Result; });
        }, TaskScheduler.Default);
    }

    public void StartWatching() => Watch.SetFolders(Db.WatchFolders());

    /// <summary>Probes the encoders once in the background (opening each takes up to ~0.5 s), so the
    /// first trim of the session does not wait for it and the trim sheet knows what this PC can encode.</summary>
    public void WarmUpEncoders() => _indexWorker.Post(() =>
    {
        try
        {
            SystemInfo info = MediaEngine.QuerySystem();
            _ui.TryEnqueue(() => Trim.Encoders = info.VideoEncoders);
        }
        catch (EngineException ex) { Log.Warn("engine", "encoder probe failed: " + ex.Message); }
    });

    private void OnNewClip(string path)
    {
        Explorer.MarkNew(path);
        if (Explorer.IsFolderMode && string.Equals(Path.GetDirectoryName(path), Explorer.CurrentFolder, StringComparison.OrdinalIgnoreCase))
            Explorer.Refresh();
        if (Settings.Thumbnails) _ = Thumbnails.GetAsync(path);
        Toast($"New clip · {Path.GetFileName(path)}", "Open", () => OpenPaths([path]));
    }

    public void ToggleFavorite(string path)
    {
        bool now = !(Db.GetFile(path)?.Favorite ?? false);
        Db.SetFavorite(path, now);
        foreach (var s in Sessions.Where(s => string.Equals(s.Path, path, StringComparison.OrdinalIgnoreCase))) s.IsFavorite = now;
        foreach (var i in Explorer.Items.Where(i => string.Equals(i.Path, path, StringComparison.OrdinalIgnoreCase))) i.IsFavorite = now;
    }

    public async Task RenameAsync(string path)
    {
        string? name = await _shell.PromptAsync("Rename", Path.GetFileName(path), "Rename");
        if (name == null) return;
        try
        {
            var session = Sessions.FirstOrDefault(s => string.Equals(s.Path, path, StringComparison.OrdinalIgnoreCase));
            bool wasActive = session != null && session == _active;
            long at = PositionUs;
            if (wasActive) Playback.Close();  // release the file handle before moving it
            string target = FileOperations.Rename(path, name);
            Db.RenamePath(path, target);
            if (session != null)
            {
                session.Path = target;
                if (wasActive)
                {
                    session.PositionUs = at;
                    ActiveSession = null;
                    Activate(session);
                }
            }
            Explorer.Refresh();
        }
        catch (IOException ex)
        {
            Toast(ex.Message);
        }
        catch (UnauthorizedAccessException)
        {
            Toast("Windows denied renaming this file.");
        }
    }

    public async Task RecycleAsync(string path)
    {
        if (!await _shell.ConfirmAsync("Move to Recycle Bin?", $"{Path.GetFileName(path)} will be moved to the Recycle Bin. You can restore it from there.", "Move to Recycle Bin"))
            return;
        var session = Sessions.FirstOrDefault(s => string.Equals(s.Path, path, StringComparison.OrdinalIgnoreCase));
        if (session != null) CloseSession(session);
        if (FileOperations.Recycle(path, out string? error))
        {
            Db.Forget(path);
            Explorer.Refresh();
            Toast($"Moved {Path.GetFileName(path)} to the Recycle Bin.");
        }
        else
        {
            Toast(error ?? "The file could not be moved to the Recycle Bin.");
        }
    }
}
