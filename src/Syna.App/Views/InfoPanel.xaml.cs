using System.ComponentModel;
using System.Text;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Syna.Views;

/// <summary>Read-only facts about the active clip, from the probe and the live player; copyable as plain text.</summary>
public sealed partial class InfoPanel : UserControl
{
    private readonly StringBuilder _plain = new();

    public InfoPanel()
    {
        ViewModel = App.Current!.ViewModel!;
        InitializeComponent();
        ViewModel.PropertyChanged += OnViewModelChanged;
    }

    public MainViewModel ViewModel { get; }

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MainViewModel.InfoVisible) or nameof(MainViewModel.ActiveSession) or nameof(MainViewModel.Stage)
            && ViewModel.InfoVisible)
            Refresh();
    }

    private void Refresh()
    {
        Sections.Children.Clear();
        _plain.Clear();
        var session = ViewModel.ActiveSession;
        var info = session?.Info;
        EmptyText.Visibility = Ui.Show(info == null);
        CopyButton.IsEnabled = info != null;
        if (session == null || info == null) return;

        Section("File");
        Row("Name", session.FileName);
        Row("Folder", session.Folder);
        Row("Container", info.Container.Length > 0 ? info.Container : info.ContainerLong);
        Row("Size", $"{FormatBytes(info.SizeBytes)} ({info.SizeBytes:N0} bytes)");
        Row("Duration", Timecode.Precise(info.DurationUs));
        if (info.BitRate > 0) Row("Overall bitrate", FormatRate(info.BitRate));

        if (info.Video is { } v)
        {
            Section("Video");
            Row("Codec", Join(v.Codec.ToUpperInvariant(), v.Profile, v.Level > 0 ? "level " + FormatLevel(v) : ""));
            Row("Resolution", $"{v.Width}×{v.Height}" +(v.SarNum > 0 && v.SarNum != v.SarDen ? $", sample aspect {v.SarNum}:{v.SarDen}" : ""));
            Row("Frame rate", v.Vfr
                ? $"Variable, {v.FpsAvg:0.###} fps average, frame intervals {v.FrameIntervalMinMs:0.##}–{v.FrameIntervalMaxMs:0.##} ms"
                : $"{v.FpsAvg:0.###} fps, constant");
            if (v.FrameCount > 0) Row("Frames", v.FrameCount.ToString("N0"));
            Row("Pixel format", Join(v.PixelFormat, v.BitDepth > 0 ? $"{v.BitDepth}-bit" : "", v.Chroma));
            Row("Color", ColorText(v));
            if (v.Hdr.Length > 0) Row("HDR", v.Hdr);
            if (v.BitRate > 0) Row("Bitrate", FormatRate(v.BitRate));
            if (v.KeyframeIntervalS > 0) Row("Keyframe interval", $"{v.KeyframeIntervalS:0.##} s");
            Row("B-frames", v.HasBFrames > 0 ? "Yes" : "No");
            if (v.Rotation != 0) Row("Rotation", $"{v.Rotation:0}°");
        }

        for (int i = 0; i < info.Audio.Length; i++)
        {
            var a = info.Audio[i];
            string label = i < session.Tracks.Count ? session.Tracks[i].Label : a.Label;
            Section(label.Length > 0 ? $"Audio {i + 1} · {label}" : $"Audio {i + 1}");
            Row("Codec", Join(a.Codec.ToUpperInvariant(), a.Profile));
            Row("Channels", Join(a.Channels.ToString(), a.ChannelLayout));
            Row("Sample rate", $"{a.SampleRate / 1000.0:0.###} kHz");
            if (a.BitRate > 0) Row("Bitrate", FormatRate(a.BitRate));
            if (a.Title.Length > 0) Row("Stream title", a.Title);
            if (a.Language.Length > 0 && a.Language != "und") Row("Language", a.Language);
        }

        if (ViewModel.Playback.Diagnostics() is { } d)
        {
            Section("Playback");
            Row("Decoder", d.Decoder);
            Row("Renderer", d.Renderer);
            Row("GPU", d.Gpu);
            Row("Audio output", d.AudioDevice ? d.AudioFormat : "No audio device");
        }
    }

    private void Section(string title)
    {
        Sections.Children.Add(new TextBlock
        {
            Text = title.ToUpperInvariant(),
            Style = Resource<Style>("SectionLabel"),
            Margin = new Thickness(0, Sections.Children.Count == 0 ? 4 : 20, 0, 6),
        });
        if (_plain.Length > 0) _plain.AppendLine();
        _plain.AppendLine(title);
    }

    private void Row(string label, string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return;
        var grid = new Grid { ColumnSpacing = 12, Padding = new Thickness(0, 3, 0, 3) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(104) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(new TextBlock { Text = label, Style = Resource<Style>("CaptionText"), TextWrapping = TextWrapping.Wrap });
        var text = new TextBlock { Text = value, Style = Resource<Style>("BodyText"), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);
        Sections.Children.Add(grid);
        _plain.Append(label).Append(": ").AppendLine(value);
    }

    private static string Join(params string[] parts) => string.Join(", ", parts.Where(p => !string.IsNullOrWhiteSpace(p)));

    // "BT.709, limited range" when matrix, primaries and transfer agree; each named otherwise.
    private static string ColorText(VideoStreamInfo v)
    {
        static string Name(string s) => s switch
        {
            "bt709" => "BT.709",
            "bt470bg" => "BT.601 (625)",
            "smpte170m" => "BT.601 (525)",
            "bt2020nc" or "bt2020c" or "bt2020" => "BT.2020",
            "smpte2084" => "PQ",
            "arib-std-b67" => "HLG",
            "iec61966-2-1" => "sRGB",
            "" or "unknown" or "unspecified" => "",
            _ => s,
        };
        string range = v.ColorRange switch { "tv" => "limited range", "pc" => "full range", _ => "" };
        string m = Name(v.ColorSpace), p = Name(v.ColorPrimaries), t = Name(v.ColorTransfer);
        string standard = m == p && p == t ? m : Join(m.Length > 0 ? "matrix " + m : "", p.Length > 0 ? "primaries " + p : "", t.Length > 0 ? "transfer " + t : "");
        return Join(standard, range);
    }

    private static string FormatLevel(VideoStreamInfo v) => v.Codec switch
    {
        "h264" => $"{v.Level / 10.0:0.#}",
        "hevc" => $"{v.Level / 30.0:0.#}",
        _ => v.Level.ToString(),
    };

    private static string FormatBytes(long bytes) => bytes switch
    {
        < 1024 * 1024 => $"{bytes / 1024.0:0} KB",
        < 1024L * 1024 * 1024 => $"{bytes / (1024.0 * 1024):0.0} MB",
        _ => $"{bytes / (1024.0 * 1024 * 1024):0.00} GB",
    };

    private static string FormatRate(long bitsPerSecond) => bitsPerSecond >= 1_000_000
        ? $"{bitsPerSecond / 1_000_000.0:0.0} Mb/s"
        : $"{bitsPerSecond / 1000.0:0} kb/s";

    private void OnCopyClick(object sender, RoutedEventArgs e)
    {
        if (_plain.Length == 0) return;
        FileOperations.CopyText(_plain.ToString());
        ViewModel.Toast("Media info copied.");
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => ViewModel.InfoVisible = false;

    private static T Resource<T>(string key) => (T)Application.Current.Resources[key];
}
