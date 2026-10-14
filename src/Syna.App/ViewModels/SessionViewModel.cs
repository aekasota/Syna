using System.Collections.ObjectModel;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;
using Microsoft.UI.Xaml.Media;

namespace Syna.ViewModels;

public sealed class AudioTrackViewModel : ObservableObject
{
    private readonly Action<AudioTrackViewModel> _changed;
    private string _label;
    private double _gain = 1.0;
    private bool _muted;
    private bool _soloed;
    private int _offsetMs;
    private double _level;
    private Brush _color;

    public AudioTrackViewModel(int index, string label, bool suggestion, string detail, Brush color, Action<AudioTrackViewModel> changed)
    {
        Index = index;
        _label = label;
        LabelIsSuggestion = suggestion;
        Detail = detail;
        _color = color;
        _changed = changed;
    }

    public int Index { get; }
    public string Detail { get; }
    public bool LabelIsSuggestion { get; private set; }

    public Brush Color
    {
        get => _color;
        private set => Set(ref _color, value);
    }

    public void SetColor(Brush brush) => Color = brush;

    public string Label
    {
        get => _label;
        set
        {
            value = value.Trim();
            if (value.Length == 0 || !Set(ref _label, value)) return;
            LabelIsSuggestion = false;
            Raise(nameof(LabelIsSuggestion));
            _changed(this);
        }
    }

    /// <summary>Linear gain, 0..2 (0 to +6 dB).</summary>
    public double Gain
    {
        get => _gain;
        set
        {
            value = Math.Clamp(value, 0, 2);
            if (Set(ref _gain, value))
            {
                Raise(nameof(GainText));
                _changed(this);
            }
        }
    }

    public string GainText => $"{Math.Round(_gain * 100)}%";

    public bool Muted
    {
        get => _muted;
        set { if (Set(ref _muted, value)) _changed(this); }
    }

    public bool Soloed
    {
        get => _soloed;
        set { if (Set(ref _soloed, value)) _changed(this); }
    }

    public int OffsetMs
    {
        get => _offsetMs;
        set
        {
            value = Math.Clamp(value, -5000, 5000);
            if (Set(ref _offsetMs, value))
            {
                Raise(nameof(OffsetText));
                Raise(nameof(HasOffset));
                _changed(this);
            }
        }
    }

    public bool HasOffset => _offsetMs != 0;
    public string OffsetText => _offsetMs == 0 ? "" : $"{(_offsetMs > 0 ? "+" : "")}{_offsetMs} ms";

    /// <summary>Meter position 0..1 on a −60..0 dBFS scale, with peak-hold style decay.</summary>
    public double Level
    {
        get => _level;
        private set => Set(ref _level, value);
    }

    public void PushPeak(float peak, double seconds)
    {
        double target = peak <= 1e-6f ? 0 : Math.Clamp((20 * Math.Log10(peak) + 60) / 60, 0, 1);
        Level = target >= _level ? target : Math.Max(target, _level - seconds * 0.45);
    }

    public void ResetLevel() => Level = 0;

    public void Load(double gain, bool muted, bool soloed, int offsetMs, string? label)
    {
        _gain = Math.Clamp(gain, 0, 2);
        _muted = muted;
        _soloed = soloed;
        _offsetMs = offsetMs;
        if (!string.IsNullOrWhiteSpace(label))
        {
            _label = label;
            LabelIsSuggestion = false;
        }
        Raise(nameof(Gain));
        Raise(nameof(GainText));
        Raise(nameof(Muted));
        Raise(nameof(Soloed));
        Raise(nameof(OffsetMs));
        Raise(nameof(OffsetText));
        Raise(nameof(HasOffset));
        Raise(nameof(Label));
        Raise(nameof(LabelIsSuggestion));
    }
}

public sealed class SessionViewModel(string path) : ObservableObject
{
    private string _path = path;
    private MediaInfo? _info;
    private ImageSource? _thumbnail;
    private bool _isActive;
    private bool _isMissing;
    private bool _isFavorite;
    private string? _error;
    private long _inUs = -1;
    private long _outUs = -1;
    private WaveformData? _waveform;
    private long _durationUs;
    private volatile bool _closed;

    /// <summary>Removed from the rail: background work queued for it is skipped and its results dropped.</summary>
    public bool IsClosed
    {
        get => _closed;
        set => _closed = value;
    }

    public string Path
    {
        get => _path;
        set
        {
            if (Set(ref _path, value))
            {
                Raise(nameof(FileName));
                Raise(nameof(Folder));
            }
        }
    }

    public string FileName => System.IO.Path.GetFileName(_path);
    public override string ToString() => FileName;  // accessible name of the rail item
    public string Folder => System.IO.Path.GetDirectoryName(_path) ?? "";

    public long PositionUs { get; set; }
    public bool WasPlaying { get; set; }
    public ObservableCollection<AudioTrackViewModel> Tracks { get; } = [];

    /// <summary>A copy of this clip's mixer as it stands: what is saved with the file and what a trim exports.</summary>
    public MixerState Mixer => new([.. Tracks.Select(t => t.Gain)], [.. Tracks.Select(t => t.Muted)], [.. Tracks.Select(t => t.Soloed)], [.. Tracks.Select(t => t.OffsetMs)]);

    public MediaInfo? Info
    {
        get => _info;
        set
        {
            if (!Set(ref _info, value)) return;
            if (value != null && value.DurationUs > 0) DurationUs = value.DurationUs;
            Raise(nameof(Summary));
            Raise(nameof(Keyframes));
        }
    }

    public long DurationUs
    {
        get => _durationUs;
        set
        {
            if (Set(ref _durationUs, value)) Raise(nameof(DurationText));
        }
    }

    public string DurationText => _durationUs > 0 ? Timecode.Short(_durationUs) : "";

    public string Summary
    {
        get
        {
            var v = _info?.Video;
            if (v == null) return _info != null ? $"Audio · {Timecode.Short(_info.DurationUs)}" : "";
            string fps = v.FpsAvg > 0 ? $" · {v.FpsAvg:0.##} fps" : "";
            return $"{v.Width}×{v.Height}{fps} · {Timecode.Short(_info!.DurationUs)}";
        }
    }

    public long[] Keyframes => _info?.Video?.KeyframesUs ?? [];

    public ImageSource? Thumbnail
    {
        get => _thumbnail;
        set => Set(ref _thumbnail, value);
    }

    public bool IsActive
    {
        get => _isActive;
        set => Set(ref _isActive, value);
    }

    public bool IsMissing
    {
        get => _isMissing;
        set => Set(ref _isMissing, value);
    }

    public bool IsFavorite
    {
        get => _isFavorite;
        set => Set(ref _isFavorite, value);
    }

    public string? Error
    {
        get => _error;
        set
        {
            if (Set(ref _error, value)) Raise(nameof(HasError));
        }
    }

    public bool HasError => _error != null;

    public long InUs
    {
        get => _inUs;
        set
        {
            if (Set(ref _inUs, value)) RaiseRange();
        }
    }

    public long OutUs
    {
        get => _outUs;
        set
        {
            if (Set(ref _outUs, value)) RaiseRange();
        }
    }

    public bool HasIn => _inUs >= 0;
    public bool HasOut => _outUs >= 0;

    /// <summary>The selection used for trimming and A/B looping; open ends default to the clip edges.</summary>
    public (long In, long Out) Range => (Math.Max(0, _inUs), _outUs >= 0 ? _outUs : _durationUs);

    public bool HasRange => (_inUs >= 0 || _outUs >= 0) && Range.Out > Range.In;

    private void RaiseRange()
    {
        Raise(nameof(HasIn));
        Raise(nameof(HasOut));
        Raise(nameof(HasRange));
        Raise(nameof(Range));
    }

    public WaveformData? Waveform
    {
        get => _waveform;
        set => Set(ref _waveform, value);
    }
}
