using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;
using Syna.Services;

namespace Syna.ViewModels;

/// <summary>A choice in one of the trim sheet's menus.</summary>
public sealed record Choice<T>(string Label, T Value, bool Enabled = true);

/// <summary>
/// The trim sheet. Before running, it explains what the chosen mode, profile and audio arrangement
/// will do to this specific file (its real keyframes, tracks and this PC's encoders: see
/// TrimPlanner); afterwards it reports what the engine actually did.
/// </summary>
public sealed class TrimViewModel(AppSettings settings, Action<string> notify, Action saveSettings) : ObservableObject
{
    private SessionViewModel? _session;
    private bool _isOpen;
    private TrimMode _mode = settings.TrimMode;
    private TrimProfile _profile = settings.TrimProfile;
    private TrimAudio _arrangement = settings.TrimAudio;
    private IReadOnlyList<VideoEncoderInfo> _encoders = [];
    private TrimPlan _plan = new();
    private string _outputName = "";
    private string _autoName = "";
    private string _outputFolder = "";
    private bool _folderFollowsClip;
    private bool _folderChosen;  // picked in this sheet (Change folder / Use clip folder), not just offered
    private string _folderNote = "";
    private bool _running;
    private double _progress;
    private TrimReport? _result;
    private string? _error;
    private CancellationTokenSource? _cts;

    private TrimCustomSettings Custom => settings.TrimCustom;

    public bool IsOpen
    {
        get => _isOpen;
        set => Set(ref _isOpen, value);
    }

    public SessionViewModel? Session => _session;
    public long InUs => _session?.Range.In ?? 0;
    public long OutUs => _session?.Range.Out ?? 0;

    public string RangeText => _session == null
        ? ""
        : $"{Timecode.Precise(InUs)}  →  {Timecode.Precise(OutUs)}  ·  {Timecode.Seconds((OutUs - InUs) / 1e6)}";

    /// <summary>Encoders that open on this PC, once the background probe has run.</summary>
    public IReadOnlyList<VideoEncoderInfo> Encoders
    {
        get => _encoders;
        set
        {
            _encoders = value;
            Replan();
        }
    }

    public TrimMode Mode
    {
        get => _mode;
        set
        {
            if (!Set(ref _mode, value)) return;
            settings.TrimMode = value;
            saveSettings();
            Raise(nameof(IsQuick));
            Raise(nameof(IsSmart));
            Raise(nameof(IsExact));
            Replan();
        }
    }

    public bool IsQuick { get => _mode == TrimMode.Quick; set { if (value) Mode = TrimMode.Quick; } }
    public bool IsSmart { get => _mode == TrimMode.Smart; set { if (value) Mode = TrimMode.Smart; } }
    public bool IsExact { get => _mode == TrimMode.Exact; set { if (value) Mode = TrimMode.Exact; } }

    public TrimProfile Profile
    {
        get => _profile;
        set
        {
            if (!Set(ref _profile, value)) return;
            settings.TrimProfile = value;
            saveSettings();
            Raise(nameof(IsOriginalProfile));
            Raise(nameof(IsHighQualityProfile));
            Raise(nameof(IsBalancedProfile));
            Raise(nameof(IsSmallerProfile));
            Raise(nameof(IsCustomProfile));
            Replan();
        }
    }

    public bool IsOriginalProfile { get => _profile == TrimProfile.Original; set { if (value) Profile = TrimProfile.Original; } }
    public bool IsHighQualityProfile { get => _profile == TrimProfile.HighQuality; set { if (value) Profile = TrimProfile.HighQuality; } }
    public bool IsBalancedProfile { get => _profile == TrimProfile.Balanced; set { if (value) Profile = TrimProfile.Balanced; } }
    public bool IsSmallerProfile { get => _profile == TrimProfile.SmallerFile; set { if (value) Profile = TrimProfile.SmallerFile; } }
    public bool IsCustomProfile { get => _profile == TrimProfile.Custom; set { if (value) Profile = TrimProfile.Custom; } }

    public TrimAudio Arrangement
    {
        get => _arrangement;
        set
        {
            if (!Set(ref _arrangement, value)) return;
            settings.TrimAudio = value;
            saveSettings();
            Raise(nameof(IsMix));
            Raise(nameof(IsSeparate));
            Replan();
        }
    }

    public bool IsMix { get => _arrangement == TrimAudio.Mix; set { if (value) Arrangement = TrimAudio.Mix; } }
    public bool IsSeparate { get => _arrangement == TrimAudio.Separate; set { if (value) Arrangement = TrimAudio.Separate; } }

    public int AudioTrackCount => _session?.Info?.Audio.Length ?? 0;
    /// <summary>Mix or keep separate only means something with two or more tracks.</summary>
    public bool CanArrangeAudio => AudioTrackCount > 1;
    public string ArrangementHint => AudioTrackCount switch
    {
        0 => "This clip has no audio.",
        1 => "This clip has one audio track.",
        2 => "",
        int n => $"All {n} tracks are kept: mixed into one, or each as its own track.",
    };

    public TrimVideo CustomVideo
    {
        get => Custom.Video;
        set => SetCustom(() => Custom.Video = value, nameof(CustomVideo), nameof(IsCustomCopy), nameof(IsCustomEncode), nameof(IsCustomLossless), nameof(CustomEncodes));
    }

    public bool IsCustomCopy { get => Custom.Video == TrimVideo.Original; set { if (value) CustomVideo = TrimVideo.Original; } }
    public bool IsCustomEncode { get => Custom.Video == TrimVideo.Encode; set { if (value) CustomVideo = TrimVideo.Encode; } }
    public bool IsCustomLossless { get => Custom.Video == TrimVideo.Lossless; set { if (value) CustomVideo = TrimVideo.Lossless; } }
    public bool CustomEncodes => Custom.Video == TrimVideo.Encode;
    public bool CustomPicksCodec => Custom.Video != TrimVideo.Original;

    public bool CustomUseBitrate
    {
        get => Custom.UseBitrate;
        set => SetCustom(() => Custom.UseBitrate = value, nameof(CustomUseBitrate), nameof(CustomUseQuality));
    }

    public bool CustomUseQuality { get => !Custom.UseBitrate; set => CustomUseBitrate = !value; }

    public double CustomQuality
    {
        get => Custom.Quality;
        set => SetCustom(() => Custom.Quality = (int)Math.Round(Math.Clamp(value, 1, 51)), nameof(CustomQuality), nameof(CustomQualityText));
    }

    public string CustomQualityText => $"CQ {Custom.Quality}";

    public string CustomBitrateText
    {
        get => Custom.BitrateKbps > 0 ? $"{Custom.BitrateKbps / 1000.0:0.##}" : "";
        set
        {
            if (TryMbps(value, out int kbps) && kbps > 0) SetCustom(() => Custom.BitrateKbps = kbps, nameof(CustomBitrateText));
            else Raise(nameof(CustomBitrateText));  // back to the last valid value
        }
    }

    public string CustomMaxBitrateText
    {
        get => Custom.MaxBitrateKbps > 0 ? $"{Custom.MaxBitrateKbps / 1000.0:0.##}" : "";
        set
        {
            if (string.IsNullOrWhiteSpace(value)) SetCustom(() => Custom.MaxBitrateKbps = 0, nameof(CustomMaxBitrateText));
            else if (TryMbps(value, out int kbps)) SetCustom(() => Custom.MaxBitrateKbps = kbps, nameof(CustomMaxBitrateText));
            else Raise(nameof(CustomMaxBitrateText));
        }
    }

    private static bool TryMbps(string text, out int kbps)
    {
        kbps = 0;
        if (!double.TryParse(text.Replace(',', '.'), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double mbps)) return false;
        if (mbps < 0 || mbps > 1000) return false;
        kbps = (int)Math.Round(mbps * 1000);
        return true;
    }

    public IReadOnlyList<Choice<VideoCodec>> CodecChoices
    {
        get
        {
            var source = _session?.Info?.Video;
            string src = source != null ? TrimPlanner.CodecName(source.Codec) : "";
            var choices = new List<Choice<VideoCodec>> { new(src.Length > 0 ? $"Same as source ({src})" : "Same as source", VideoCodec.Source) };
            foreach (var c in new[] { VideoCodec.H264, VideoCodec.Hevc, VideoCodec.Av1, VideoCodec.Vp9 })
            {
                string name = TrimPlanner.EngineCodec(c);
                bool lossless = Custom.Video == TrimVideo.Lossless;
                bool available = _encoders.Count == 0 ||
                                 _encoders.Any(e => e.Codec == name && (settings.HardwareEncoding || !e.Hardware) && (!lossless || e.Lossless));
                choices.Add(new(TrimPlanner.CodecName(name) + (available ? "" : lossless ? " (no lossless encoder)" : " (no encoder)"), c, available));
            }
            return choices;
        }
    }

    public IReadOnlyList<Choice<int>> HeightChoices
    {
        get
        {
            int h = _session?.Info?.Video?.Height ?? 0;
            var list = new List<Choice<int>> { new(h > 0 ? $"Same as source ({h}p)" : "Same as source", 0) };
            list.AddRange(new[] { 1440, 1080, 720, 480 }.Where(x => h == 0 || x < h).Select(x => new Choice<int>($"{x}p", x)));
            return list;
        }
    }

    public IReadOnlyList<Choice<int>> FpsChoices
    {
        get
        {
            var v = _session?.Info?.Video;
            double fps = v?.FpsAvg ?? 0;
            var list = new List<Choice<int>> { new(fps > 0 ? (v!.Vfr ? $"Same as source ({fps:0.##} fps, variable)" : $"Same as source ({fps:0.##} fps)") : "Same as source", 0) };
            list.AddRange(new[] { 60, 50, 30, 25, 24 }.Where(x => fps == 0 || x < fps - 0.5).Select(x => new Choice<int>($"{x} fps", x)));
            return list;
        }
    }

    public static IReadOnlyList<Choice<EncoderSpeed>> SpeedChoices { get; } =
        [new("Faster", EncoderSpeed.Faster), new("Balanced", EncoderSpeed.Balanced), new("Slower", EncoderSpeed.Smaller)];

    public IReadOnlyList<Choice<AudioCodec>> AudioCodecChoices
    {
        get
        {
            var container = TrimPlanner.ContainerOf(OutputExtension);
            return
            [
                new("Automatic", AudioCodec.Auto),
                new("AAC", AudioCodec.Aac, TrimPlanner.Accepts(container, "aac")),
                new("Opus", AudioCodec.Opus, TrimPlanner.Accepts(container, "opus")),
                new("FLAC (lossless)", AudioCodec.Flac, TrimPlanner.Accepts(container, "flac")),
            ];
        }
    }

    public static IReadOnlyList<Choice<int>> AudioBitrateChoices { get; } =
        [new("128 kb/s", 128), new("192 kb/s", 192), new("256 kb/s", 256), new("320 kb/s", 320)];

    public static IReadOnlyList<Choice<int>> SampleRateChoices { get; } = [new("Same as source", 0), new("48 kHz", 48000), new("44.1 kHz", 44100)];

    public static IReadOnlyList<Choice<int>> ChannelChoices { get; } = [new("Same as source", 0), new("Stereo", 2), new("Mono", 1)];

    public VideoCodec CustomCodec { get => Custom.Codec; set => SetCustom(() => Custom.Codec = value, nameof(CustomCodecLabel)); }
    public int CustomHeight { get => Custom.Height; set => SetCustom(() => Custom.Height = value, nameof(CustomHeightLabel)); }
    public int CustomFps { get => Custom.Fps; set => SetCustom(() => Custom.Fps = value, nameof(CustomFpsLabel)); }
    public EncoderSpeed CustomSpeed { get => Custom.Speed; set => SetCustom(() => Custom.Speed = value, nameof(CustomSpeedLabel)); }
    public AudioCodec CustomAudioCodec { get => Custom.AudioCodec; set => SetCustom(() => Custom.AudioCodec = value, nameof(CustomAudioCodecLabel), nameof(CustomAudioLossy)); }
    public int CustomAudioBitrate { get => Custom.AudioBitrateKbps; set => SetCustom(() => Custom.AudioBitrateKbps = value, nameof(CustomAudioBitrateLabel)); }
    public int CustomSampleRate { get => Custom.AudioSampleRate; set => SetCustom(() => Custom.AudioSampleRate = value, nameof(CustomSampleRateLabel)); }
    public int CustomChannels { get => Custom.AudioChannels; set => SetCustom(() => Custom.AudioChannels = value, nameof(CustomChannelsLabel)); }

    public string CustomCodecLabel => Label(CodecChoices, Custom.Codec);
    public string CustomHeightLabel => Label(HeightChoices, Custom.Height);
    public string CustomFpsLabel => Label(FpsChoices, Custom.Fps);
    public string CustomSpeedLabel => Label(SpeedChoices, Custom.Speed);
    public string CustomAudioCodecLabel => Label(AudioCodecChoices, Custom.AudioCodec);
    public string CustomAudioBitrateLabel => Label(AudioBitrateChoices, Custom.AudioBitrateKbps);
    public string CustomSampleRateLabel => Label(SampleRateChoices, Custom.AudioSampleRate);
    public string CustomChannelsLabel => Label(ChannelChoices, Custom.AudioChannels);
    public bool CustomAudioLossy => Custom.AudioCodec is AudioCodec.Aac or AudioCodec.Opus;

    private static string Label<T>(IReadOnlyList<Choice<T>> choices, T value) =>
        choices.FirstOrDefault(c => EqualityComparer<T>.Default.Equals(c.Value, value))?.Label.Split(" (no")[0] ?? $"{value}";

    private void SetCustom(Action change, params string[] changed)
    {
        change();
        saveSettings();
        foreach (string name in changed) Raise(name);
        Raise(nameof(CustomPicksCodec));
        Replan();
    }

    private string OutputExtension
    {
        get
        {
            string ext = Path.GetExtension(_outputName);
            return ext.Length > 0 ? ext : Path.GetExtension(_session?.Path ?? ".mp4");
        }
    }

    public TrimPlan Plan => _plan;
    public string Explanation => _plan.Summary;
    public string PlanVideo => _plan.Video;
    public string PlanAudio => _plan.Audio;
    public string PlanNotes => string.Join("\n", _plan.Notes);
    public bool HasPlanNotes => _plan.Notes.Count > 0;
    public string Blocker => _plan.Blocker ?? "";
    public bool HasBlocker => _plan.Blocker != null;
    public bool CanSave => _session != null && _plan.Blocker == null;

    // The plan reads this clip's own mixer (a copy, taken now), so the sheet shows and the export
    // uses the tracks as the player plays them.
    private void Replan()
    {
        if (_session == null) return;
        _plan = TrimPlanner.Plan(new TrimPlanInput
        {
            Mode = _mode, Profile = _profile, Arrangement = _arrangement, Custom = Custom, Source = _session.Info,
            InUs = InUs, OutUs = OutUs, Keyframes = _session.Keyframes, OutputExtension = OutputExtension,
            Encoders = _encoders, AllowHardwareEncode = settings.HardwareEncoding,
            Mixer = _session.Mixer, TrackNames = [.. _session.Tracks.Select(t => t.Label)],
        });
        foreach (string name in new[] { nameof(Plan), nameof(Explanation), nameof(PlanVideo), nameof(PlanAudio), nameof(PlanNotes), nameof(HasPlanNotes),
                                        nameof(Blocker), nameof(HasBlocker), nameof(CanSave), nameof(CodecChoices), nameof(AudioCodecChoices),
                                        nameof(CustomCodecLabel), nameof(CustomAudioCodecLabel) })
            Raise(name);
    }

    public string OutputName
    {
        get => _outputName;
        set
        {
            if (!Set(ref _outputName, value)) return;
            Raise(nameof(OutputPath));
            Replan();  // the extension decides the container
        }
    }

    public string OutputFolder
    {
        get => _outputFolder;
        private set
        {
            if (!Set(ref _outputFolder, value)) return;
            Raise(nameof(OutputPath));
            Raise(nameof(OutputFolderName));
            Raise(nameof(CanUseClipFolder));
            // A name the user did not edit follows the folder, so it stays free there.
            if (_session != null && _outputName == _autoName) SuggestName();
        }
    }

    public string OutputFolderName => _outputFolder;
    public string OutputPath => Path.Combine(_outputFolder, _outputName);
    /// <summary>Why the folder is not the remembered one, when it is not.</summary>
    public string FolderNote
    {
        get => _folderNote;
        private set
        {
            if (Set(ref _folderNote, value)) Raise(nameof(HasFolderNote));
        }
    }

    public bool HasFolderNote => _folderNote.Length > 0;
    public bool CanUseClipFolder => _session != null && !string.Equals(_outputFolder, _session.Folder, StringComparison.OrdinalIgnoreCase);

    /// <summary>A folder picked in the sheet; remembered for every clip once a trim to it succeeds.</summary>
    public void ChooseFolder(string folder)
    {
        _folderFollowsClip = false;
        _folderChosen = true;
        FolderNote = "";
        OutputFolder = folder;
    }

    /// <summary>Saves next to each source clip again (remembered once a trim succeeds).</summary>
    public void UseClipFolder()
    {
        if (_session == null) return;
        _folderFollowsClip = true;
        _folderChosen = true;
        FolderNote = "";
        OutputFolder = _session.Folder;
    }

    private void SuggestName()
    {
        if (_session == null) return;
        _autoName = Path.GetFileName(MediaFiles.NextAvailable(_outputFolder, _session.FileName, settings.TrimSuffix, Path.GetExtension(_session.Path)));
        _outputName = _autoName;
        Raise(nameof(OutputName));
        Raise(nameof(OutputPath));
    }

    public bool IsRunning
    {
        get => _running;
        private set
        {
            if (Set(ref _running, value)) Raise(nameof(IsEditing));
        }
    }

    public bool IsEditing => !_running && _result == null && _error == null;

    public double Progress
    {
        get => _progress;
        private set => Set(ref _progress, value);
    }

    public TrimReport? Result
    {
        get => _result;
        private set
        {
            if (!Set(ref _result, value)) return;
            Raise(nameof(HasResult));
            Raise(nameof(ResultTitle));
            Raise(nameof(ResultLines));
            Raise(nameof(IsEditing));
        }
    }

    public bool HasResult => _result != null;

    public string? Error
    {
        get => _error;
        private set
        {
            if (!Set(ref _error, value)) return;
            Raise(nameof(HasError));
            Raise(nameof(IsEditing));
        }
    }

    public bool HasError => _error != null;

    public string ResultTitle => _result == null ? "" : $"Saved {Path.GetFileName(_result.Output)}";

    public IReadOnlyList<string> ResultLines
    {
        get
        {
            var r = _result;
            if (r == null) return [];
            var lines = new List<string>();
            string mode = TrimPlanner.ModeName(r.ModeUsed switch { "quick" => TrimMode.Quick, "smart" => TrimMode.Smart, _ => TrimMode.Exact });
            string cpu = r.HardwareEncoder ? "" : " (CPU)";
            lines.Add(r.VideoMethod switch
            {
                "copy" => $"{mode}: video copied without re-encoding ({Timecode.Seconds(r.CopiedSeconds)}).",
                "smart" => $"{mode}: {Timecode.Seconds(r.ReencodedSeconds)} re-encoded with {r.Encoder}, {Timecode.Seconds(r.CopiedSeconds)} copied.",
                "reencode" when r.LosslessVideo => $"{mode}: {Timecode.Seconds(r.ReencodedSeconds)} re-encoded losslessly with {r.Encoder}{cpu}; the first frames match the source's pixels exactly.",
                "reencode" => $"{mode}: {Timecode.Seconds(r.ReencodedSeconds)} re-encoded with {r.Encoder}{cpu}.",
                _ => $"{mode}: audio only.",
            });
            if (!string.IsNullOrEmpty(r.FallbackReason)) lines.Add(char.ToUpper(r.FallbackReason[0]) + r.FallbackReason[1..] + ".");
            var v = r.Validation;
            string frames = v.FrameCount > 0 ? $"{v.FrameCount} frames, " : "";  // Matroska/WebM store no frame count
            string bitrate = v.VideoBitRate > 0 ? $", {v.VideoBitRate / 1e6:0.#} Mb/s" : "";
            lines.Add($"Checked: {TrimPlanner.CodecName(v.VideoCodec)} {v.Width}×{v.Height}, {frames}{v.FpsAvg:0.##} fps{bitrate}, {DescribeAudio(r)}.");
            if (!string.IsNullOrEmpty(r.AudioNote)) lines.Add(char.ToUpper(r.AudioNote[0]) + r.AudioNote[1..] + ".");
            lines.Add($"Range {Timecode.Precise(r.ActualStartUs)} → {Timecode.Precise(r.ActualEndUs)} in {r.ElapsedSeconds:0.0} s.");
            return lines;
        }
    }

    private static string DescribeAudio(TrimReport r)
    {
        var outs = r.AudioOutputs;
        if (outs.Length == 0) return "no audio";
        string mixer = outs.Any(o => o.Adjusted) ? " with the mixer's settings" : "";
        if (outs.Length == 1 && outs[0].Method == "mix") return $"{r.AudioTracks} audio tracks mixed into one{mixer} ({outs[0].Description})";
        if (outs.All(o => o.Method == "copy")) return outs.Length == 1 ? "audio copied" : $"{outs.Length} audio tracks kept separate, copied";
        string encoded = outs.First(o => o.Method != "copy").Description;
        int copied = outs.Count(o => o.Method == "copy");
        if (outs.Length == 1) return $"audio re-encoded{mixer} ({encoded})";
        return copied > 0
            ? $"{outs.Length} audio tracks kept separate, {copied} copied, {outs.Length - copied} re-encoded{mixer} ({encoded})"
            : $"{outs.Length} audio tracks kept separate, re-encoded{mixer} ({encoded})";
    }

    public void Open(SessionViewModel session)
    {
        _session = session;
        _result = null;
        _error = null;
        Progress = 0;
        string saved = settings.TrimFolder;
        bool available = saved.Length > 0 && Directory.Exists(saved);
        _folderFollowsClip = !available;
        _folderChosen = false;
        _outputFolder = available ? saved : session.Folder;
        FolderNote = saved.Length > 0 && !available ? $"{saved} is not available, so the clip's folder is used." : "";
        SuggestName();
        foreach (string name in new[] { nameof(Session), nameof(RangeText), nameof(HasResult), nameof(HasError), nameof(IsEditing), nameof(InUs), nameof(OutUs),
                                        nameof(OutputFolder), nameof(OutputFolderName), nameof(CanUseClipFolder), nameof(AudioTrackCount), nameof(CanArrangeAudio),
                                        nameof(ArrangementHint), nameof(HeightChoices), nameof(FpsChoices), nameof(CustomHeightLabel), nameof(CustomFpsLabel),
                                        nameof(CustomSpeedLabel), nameof(CustomAudioBitrateLabel), nameof(CustomSampleRateLabel), nameof(CustomChannelsLabel) })
            Raise(name);
        Replan();
        IsOpen = true;
    }

    public async Task RunAsync()
    {
        if (_session == null || _running) return;
        Replan();  // the mixer as it is now; the request below is a copy, so later changes cannot reach the job
        if (_plan.Blocker != null)
        {
            Error = _plan.Blocker;
            return;
        }
        string target = OutputPath;
        if (Path.GetExtension(target).Length == 0) target += Path.GetExtension(_session.Path);
        if (File.Exists(target))
        {
            Error = "A file with this name already exists. Choose another name.";
            return;
        }
        if (!Directory.Exists(_outputFolder))
        {
            Error = $"The folder {_outputFolder} is not available. Choose another folder.";
            return;
        }
        IsRunning = true;
        Error = null;
        _cts = new CancellationTokenSource();
        try
        {
            TrimOutput output = _plan.Output;
            using var job = TrimJob.Start(new TrimRequest(_session.Path, target, InUs, OutUs, _mode, settings.HardwareEncoding, output));
            var progress = new Progress<double>(p => Progress = p);
            var (state, error) = await job.WaitAsync(progress, _cts.Token);
            TrimReport? report = job.Report;
            if (state == JobState.Succeeded && report != null)
            {
                Result = report;
                RememberFolder();
                notify($"Saved {Path.GetFileName(report.Output)}");
                Log.Info("trim", $"ok mode={report.ModeUsed} video={report.VideoHandling}/{report.VideoMethod} encoder={report.Encoder} audio={report.AudioMethod} elapsed={report.ElapsedSeconds:0.00}s");
            }
            else if (state == JobState.Cancelled)
            {
                Error = "Cancelled. Nothing was written.";
            }
            else
            {
                string detail = report?.Error is { Length: > 0 } e ? e : MediaEngine.Describe(error);
                string issues = report?.Validation.Issues is { Length: > 0 } i ? " " + string.Join("; ", i) + "." : "";
                Error = $"The clip could not be saved: {detail}.{issues} The source file was not changed.";
                Log.Warn("trim", $"failed: {detail}{issues}");
            }
        }
        catch (EngineException ex)
        {
            Error = $"The clip could not be saved: {ex.Message}";
        }
        finally
        {
            IsRunning = false;
            _cts.Dispose();
            _cts = null;
        }
    }

    // A folder chosen in the sheet becomes the default for every clip once a trim to it succeeds,
    // until another one is confirmed the same way ("" = next to each source clip). Falling back to the
    // clip's folder because the remembered one is missing (an unplugged drive) forgets nothing.
    private void RememberFolder()
    {
        if (!_folderChosen) return;
        string folder = _folderFollowsClip ? "" : _outputFolder;
        if (string.Equals(settings.TrimFolder, folder, StringComparison.OrdinalIgnoreCase)) return;
        settings.TrimFolder = folder;
        saveSettings();
    }

    public void Cancel()
    {
        if (_running) _cts?.Cancel();
        else IsOpen = false;
    }

    /// <summary>Back to the settings after an error, keeping the chosen folder and name.</summary>
    public void Reset()
    {
        Result = null;
        Error = null;
        Replan();
    }
}
