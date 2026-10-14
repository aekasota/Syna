using Syna.Controls;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Services;

namespace Syna.ViewModels;

public sealed class AppCommand
{
    public required string Title { get; init; }
    public string Group { get; init; } = "";
    public string Shortcut { get; init; } = "";
    public IconKind Icon { get; init; } = IconKind.None;
    public required Action Execute { get; init; }
    public Func<bool>? CanExecute { get; init; }
    public bool IsEnabled => CanExecute?.Invoke() ?? true;
}

public sealed partial class MainViewModel
{
    private static readonly double[] Rates = [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0];
    private List<AppCommand> _commands = [];

    public IReadOnlyList<AppCommand> Commands => _commands;

    public void TogglePlay()
    {
        if (_active == null || Stage != StageState.Video) return;
        if (IsPlaying) Playback.Pause();
        else Playback.Play();
        IsPlaying = !IsPlaying;
    }

    public void Pause()
    {
        Playback.Pause();
        IsPlaying = false;
    }

    public void Play()
    {
        if (_active == null) return;
        Playback.Play();
        IsPlaying = true;
    }

    public void SeekTo(long us, bool exact)
    {
        if (_active == null) return;
        us = Math.Clamp(us, 0, Math.Max(0, DurationUs - 1));
        PositionUs = us;
        Playback.Seek(us, exact);
    }

    public void StepFrame(int direction)
    {
        if (_active == null) return;
        IsPlaying = false;
        Playback.Step(direction);
    }

    public void Skip(int milliseconds) => SeekTo(PositionUs + milliseconds * 1000L, exact: true);

    public void SetRate(double rate)
    {
        Rate = rate;
        Playback.SetRate(rate);
    }

    public void StepRate(int direction)
    {
        int i = Array.IndexOf(Rates, _rate);
        if (i < 0) i = 3;
        SetRate(Rates[Math.Clamp(i + direction, 0, Rates.Length - 1)]);
    }

    /// <summary>J/K/L: K pauses; L plays and speeds up while playing; J slows down while playing, or steps back 1 s when paused.</summary>
    public void Jkl(char key)
    {
        switch (key)
        {
            case 'K':
                Pause();
                break;
            case 'L':
                if (!IsPlaying)
                {
                    if (_rate != 1.0) SetRate(1.0);
                    Play();
                }
                else StepRate(+1);
                break;
            case 'J':
                if (IsPlaying && _rate > 0.25) StepRate(-1);
                else Skip(-Settings.ShortSkipMs);
                break;
        }
    }

    public void ToggleLoop() => LoopEnabled = !LoopEnabled;

    private void ApplyLoop()
    {
        var s = _active;
        if (s == null) return;
        var (a, b) = s.HasRange ? s.Range : (0L, DurationUs);
        Playback.SetLoop(_loop, a, b);
    }

    public void ToggleMasterMute() => MasterMuted = !MasterMuted;

    public void ToggleTrackMute(int index)
    {
        if (_active != null && index < _active.Tracks.Count) _active.Tracks[index].Muted = !_active.Tracks[index].Muted;
    }

    public void ToggleTrackSolo(int index)
    {
        if (_active != null && index < _active.Tracks.Count) _active.Tracks[index].Soloed = !_active.Tracks[index].Soloed;
    }

    private long CurrentFrameStart()
    {
        var s = Playback.State;
        return !IsPlaying && s.HasFrame ? s.FramePtsUs : PositionUs;
    }

    private long CurrentFrameEnd()
    {
        var s = Playback.State;
        return !IsPlaying && s.HasFrame ? s.FramePtsUs + Math.Max(1, s.FrameDurationUs) : PositionUs;
    }

    public void SetIn()
    {
        var s = _active;
        if (s == null) return;
        s.InUs = CurrentFrameStart();
        if (s.HasOut && s.OutUs <= s.InUs) s.OutUs = -1;
        ApplyLoop();
    }

    public void SetOut()
    {
        var s = _active;
        if (s == null) return;
        s.OutUs = Math.Min(CurrentFrameEnd(), DurationUs);
        if (s.HasIn && s.InUs >= s.OutUs) s.InUs = -1;
        ApplyLoop();
    }

    public void SetRange(long inUs, long outUs)
    {
        var s = _active;
        if (s == null) return;
        s.InUs = inUs;
        s.OutUs = outUs;
        ApplyLoop();
    }

    public void ClearRange()
    {
        var s = _active;
        if (s == null) return;
        s.InUs = -1;
        s.OutUs = -1;
        ApplyLoop();
    }

    public void GoToIn()
    {
        if (_active?.HasIn == true) SeekTo(_active.InUs, true);
    }

    public void GoToOut()
    {
        if (_active?.HasOut == true) SeekTo(Math.Max(0, _active.OutUs - _frameDurationUs), true);
    }

    public void OpenTrim()
    {
        var s = _active;
        if (s == null || Stage != StageState.Video) return;
        if (IsPlaying) Pause();
        Trim.Open(s);
    }

    public async Task SaveFrameAsync(bool toClipboard)
    {
        var s = _active;
        var player = Playback.Player;
        if (s == null || player == null || Stage != StageState.Video) return;
        string stem = $"{Path.GetFileNameWithoutExtension(s.FileName)}_{Timecode.Precise(CurrentFrameStart()).Replace(':', '-').Replace('.', '_')}";
        string folder = toClipboard ? Path.Combine(Path.GetTempPath(), "Syna") : s.Folder;
        try
        {
            Directory.CreateDirectory(folder);
            string target = toClipboard ? Path.Combine(folder, "frame.png") : MediaFiles.NextAvailable(folder, stem + ".png", "", ".png");
            try
            {
                await Task.Run(() => player.SaveFrame(target, jpeg: false));
            }
            catch (EngineException ex) when (!toClipboard && ex.Error == EngineError.AccessDenied)
            {
                string pictures = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "Syna");
                Directory.CreateDirectory(pictures);
                target = MediaFiles.NextAvailable(pictures, stem + ".png", "", ".png");
                await Task.Run(() => player.SaveFrame(target, jpeg: false));
            }
            if (toClipboard)
            {
                await FileOperations.CopyImageFileAsync(target);
                Toast("Frame copied to the clipboard.");
            }
            else
            {
                Toast($"Saved {Path.GetFileName(target)}", "Reveal", () => FileOperations.Reveal(target));
            }
        }
        catch (Exception ex) when (ex is EngineException or IOException or UnauthorizedAccessException)
        {
            Toast($"The frame could not be saved: {ex.Message}");
        }
    }

    public void RevealActive()
    {
        if (_active != null) FileOperations.Reveal(_active.Path);
    }

    public void CopyActivePath()
    {
        if (_active == null) return;
        FileOperations.CopyText(_active.Path);
        Toast("Path copied.");
    }

    public void OpenInExternalEditor(string? path = null)
    {
        path ??= _active?.Path;
        if (path == null) return;
        if (string.IsNullOrWhiteSpace(Settings.ExternalEditor) || !File.Exists(Settings.ExternalEditor))
        {
            Toast("Choose an external editor in Settings first.", "Settings", () => SettingsOpen = true);
            return;
        }
        try
        {
            FileOperations.OpenWith(Settings.ExternalEditor, path);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            Toast($"The editor could not be started: {ex.Message}");
        }
    }

    public async Task PickAndOpenFilesAsync()
    {
        var files = await _shell.PickFilesAsync();
        if (files.Count > 0) OpenPaths(files);
    }

    public async Task PickAndOpenFolderAsync()
    {
        string? folder = await _shell.PickFolderAsync();
        if (folder == null) return;
        ExplorerVisible = true;
        await Explorer.NavigateAsync(folder);
    }

    public void ToggleFullscreen() => _shell.SetFullscreen(!_shell.IsFullscreen);

    private void BuildCommands()
    {
        bool hasVideo() => _active != null && Stage == StageState.Video;
        _commands =
        [
            new() { Title = "Open file…", Group = "File", Shortcut = "Ctrl+O", Icon = IconKind.VideoFile, Execute = () => _ = PickAndOpenFilesAsync() },
            new() { Title = "Open folder…", Group = "File", Shortcut = "Ctrl+Shift+O", Icon = IconKind.Folder, Execute = () => _ = PickAndOpenFolderAsync() },
            new() { Title = "Play / pause", Group = "Playback", Shortcut = "Space", Icon = IconKind.Play, Execute = TogglePlay, CanExecute = hasVideo },
            new() { Title = "Next frame", Group = "Playback", Shortcut = "→", Icon = IconKind.FrameForward, Execute = () => StepFrame(1), CanExecute = hasVideo },
            new() { Title = "Previous frame", Group = "Playback", Shortcut = "←", Icon = IconKind.FrameBack, Execute = () => StepFrame(-1), CanExecute = hasVideo },
            new() { Title = "Speed up", Group = "Playback", Shortcut = "]", Icon = IconKind.Speed, Execute = () => StepRate(1), CanExecute = hasVideo },
            new() { Title = "Slow down", Group = "Playback", Shortcut = "[", Icon = IconKind.Speed, Execute = () => StepRate(-1), CanExecute = hasVideo },
            new() { Title = "Normal speed", Group = "Playback", Shortcut = "Backspace", Icon = IconKind.Speed, Execute = () => SetRate(1.0), CanExecute = hasVideo },
            new() { Title = "Loop selection", Group = "Playback", Shortcut = "R", Icon = IconKind.Loop, Execute = ToggleLoop, CanExecute = hasVideo },
            new() { Title = "Fullscreen", Group = "View", Shortcut = "F", Icon = IconKind.Fullscreen, Execute = ToggleFullscreen },
            new() { Title = "Set IN", Group = "Selection", Shortcut = "I", Icon = IconKind.MarkIn, Execute = SetIn, CanExecute = hasVideo },
            new() { Title = "Set OUT", Group = "Selection", Shortcut = "O", Icon = IconKind.MarkOut, Execute = SetOut, CanExecute = hasVideo },
            new() { Title = "Clear selection", Group = "Selection", Shortcut = "Ctrl+Shift+X", Icon = IconKind.Close, Execute = ClearRange, CanExecute = () => _active?.HasRange == true },
            new() { Title = "Go to IN", Group = "Selection", Shortcut = "Shift+I", Icon = IconKind.MarkIn, Execute = GoToIn, CanExecute = () => _active?.HasIn == true },
            new() { Title = "Go to OUT", Group = "Selection", Shortcut = "Shift+O", Icon = IconKind.MarkOut, Execute = GoToOut, CanExecute = () => _active?.HasOut == true },
            new() { Title = "Trim selection…", Group = "Clip", Shortcut = "Ctrl+T", Icon = IconKind.Trim, Execute = OpenTrim, CanExecute = hasVideo },
            new() { Title = "Save frame as PNG", Group = "Clip", Shortcut = "Ctrl+Shift+S", Icon = IconKind.Camera, Execute = () => _ = SaveFrameAsync(false), CanExecute = hasVideo },
            new() { Title = "Copy frame", Group = "Clip", Shortcut = "Ctrl+Shift+C", Icon = IconKind.Copy, Execute = () => _ = SaveFrameAsync(true), CanExecute = hasVideo },
            new() { Title = "Copy path", Group = "Clip", Shortcut = "Ctrl+Alt+C", Icon = IconKind.Copy, Execute = CopyActivePath, CanExecute = () => _active != null },
            new() { Title = "Reveal in Explorer", Group = "Clip", Shortcut = "Ctrl+E", Icon = IconKind.Folder, Execute = RevealActive, CanExecute = () => _active != null },
            new() { Title = "Star clip", Group = "Clip", Shortcut = "Ctrl+D", Icon = IconKind.Star, Execute = () => { if (_active != null) ToggleFavorite(_active.Path); }, CanExecute = () => _active != null },
            new() { Title = "Open in external editor", Group = "Clip", Icon = IconKind.External, Execute = () => OpenInExternalEditor(), CanExecute = () => _active != null },
            new() { Title = "Media info", Group = "View", Shortcut = "Ctrl+I", Icon = IconKind.Info, Execute = () => InfoVisible = !InfoVisible, CanExecute = () => _active != null },
            new() { Title = "Mute all", Group = "Audio", Shortcut = "M", Icon = IconKind.VolumeMute, Execute = ToggleMasterMute },
            new() { Title = "Mute track 1", Group = "Audio", Shortcut = "Alt+1", Icon = IconKind.VolumeMute, Execute = () => ToggleTrackMute(0), CanExecute = () => _active?.Tracks.Count > 0 },
            new() { Title = "Mute track 2", Group = "Audio", Shortcut = "Alt+2", Icon = IconKind.VolumeMute, Execute = () => ToggleTrackMute(1), CanExecute = () => _active?.Tracks.Count > 1 },
            new() { Title = "Solo track 1", Group = "Audio", Shortcut = "Alt+Shift+1", Icon = IconKind.Audio, Execute = () => ToggleTrackSolo(0), CanExecute = () => _active?.Tracks.Count > 0 },
            new() { Title = "Solo track 2", Group = "Audio", Shortcut = "Alt+Shift+2", Icon = IconKind.Audio, Execute = () => ToggleTrackSolo(1), CanExecute = () => _active?.Tracks.Count > 1 },
            new() { Title = "Next clip", Group = "Clips", Shortcut = "Ctrl+Tab", Icon = IconKind.ChevronDown, Execute = () => CycleSession(1), CanExecute = () => Sessions.Count > 1 },
            new() { Title = "Previous clip", Group = "Clips", Shortcut = "Ctrl+Shift+Tab", Icon = IconKind.ChevronUp, Execute = () => CycleSession(-1), CanExecute = () => Sessions.Count > 1 },
            new() { Title = "Close clip", Group = "Clips", Shortcut = "Ctrl+W", Icon = IconKind.Close, Execute = () => { if (_active != null) CloseSession(_active); }, CanExecute = () => _active != null },
            new() { Title = "Close all clips", Group = "Clips", Shortcut = "Ctrl+Shift+W", Icon = IconKind.CloseAll, Execute = () => _ = CloseAllSessionsAsync(), CanExecute = () => Sessions.Count > 0 },
            new() { Title = "Toggle explorer", Group = "View", Shortcut = "Ctrl+Shift+E", Icon = IconKind.PanelLeft, Execute = () => ExplorerVisible = !ExplorerVisible },
            new() { Title = "Toggle clip list", Group = "View", Shortcut = "Ctrl+Shift+L", Icon = IconKind.PanelRight, Execute = () => RailVisible = !RailVisible },
            new() { Title = "Toggle audio tracks", Group = "View", Shortcut = "Ctrl+Shift+A", Icon = IconKind.PanelBottom, Execute = () => AudioPanelVisible = !AudioPanelVisible },
            new() { Title = "Diagnostics", Group = "View", Shortcut = "Ctrl+Shift+D", Icon = IconKind.Info, Execute = () => DiagnosticsVisible = !DiagnosticsVisible },
            new() { Title = "Recent clips", Group = "File", Icon = IconKind.Clock, Execute = () => { ExplorerVisible = true; _ = Explorer.ShowListAsync(ExplorerMode.Recent); } },
            new() { Title = "Starred clips", Group = "File", Icon = IconKind.Star, Execute = () => { ExplorerVisible = true; _ = Explorer.ShowListAsync(ExplorerMode.Starred); } },
            new() { Title = "Settings", Group = "App", Shortcut = "Ctrl+,", Icon = IconKind.Settings, Execute = () => SettingsOpen = true },
            new() { Title = "Set as default video player…", Group = "App", Icon = IconKind.VideoFile, Execute = () => _ = ShellIntegration.OpenDefaultAppsSettingsAsync() },
        ];
    }
}
