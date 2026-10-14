using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Syna.Controls;

public enum IconKind
{
    None, AppMark, Play, Pause, FrameBack, FrameForward, SkipBack, SkipForward, Loop, Volume, VolumeMute,
    MarkIn, MarkOut, Trim, Info, Settings, Folder, VideoFile, ChevronLeft, ChevronRight, ChevronUp, ChevronDown,
    ArrowUp, Search, Close, Star, StarFilled, Fullscreen, ExitFullscreen, PanelLeft, PanelRight, PanelBottom,
    Camera, Copy, External, Trash, Pencil, Eye, Clock, Check, Warning, Plus, Minus, Speed, Audio, Refresh, CloseAll,
}

/// <summary>
/// Product icon set: 20-unit grid, 1.5-unit strokes, round caps and joins. Play/pause are filled
/// so the primary transport control carries more weight than secondary actions.
/// </summary>
public sealed partial class Icon : ContentControl
{
    private static readonly Dictionary<IconKind, (string Data, bool Filled)> Glyphs = new()
    {
        // Two signals easing into one line; the lower one starts a unit later (the offset that sync resolves).
        [IconKind.AppMark] = ("M3.5 5 H6 C8.75 5 8.75 10 11.5 10 H16.5 M4.5 15 H6 C8.75 15 8.75 10 11.5 10", false),
        [IconKind.Play] = ("M6.5 4.6 C6.5 3.8 7.4 3.3 8.1 3.8 L15.6 9.2 C16.2 9.6 16.2 10.4 15.6 10.8 L8.1 16.2 C7.4 16.7 6.5 16.2 6.5 15.4 Z", true),
        [IconKind.Pause] = ("M5.5 4.5 A1 1 0 0 1 6.5 3.5 H8 A1 1 0 0 1 9 4.5 V15.5 A1 1 0 0 1 8 16.5 H6.5 A1 1 0 0 1 5.5 15.5 Z M11 4.5 A1 1 0 0 1 12 3.5 H13.5 A1 1 0 0 1 14.5 4.5 V15.5 A1 1 0 0 1 13.5 16.5 H12 A1 1 0 0 1 11 15.5 Z", true),
        [IconKind.FrameBack] = ("M5 5 V15 M15 5.5 L8.5 10 L15 14.5 Z", false),
        [IconKind.FrameForward] = ("M15 5 V15 M5 5.5 L11.5 10 L5 14.5 Z", false),
        [IconKind.SkipBack] = ("M10 5 L5 10 L10 15 M15.5 5 L10.5 10 L15.5 15", false),
        [IconKind.SkipForward] = ("M10 5 L15 10 L10 15 M4.5 5 L9.5 10 L4.5 15", false),
        [IconKind.Loop] = ("M3.5 9 A4.5 4.5 0 0 1 8 4.5 H15 M12.8 2.3 L15 4.5 L12.8 6.7 M16.5 11 A4.5 4.5 0 0 1 12 15.5 H5 M7.2 17.7 L5 15.5 L7.2 13.3", false),
        [IconKind.Volume] = ("M3.5 8 H6 L10 4.5 V15.5 L6 12 H3.5 Z M13 7.5 A3.5 3.5 0 0 1 13 12.5 M15.3 5.2 A6.5 6.5 0 0 1 15.3 14.8", false),
        [IconKind.VolumeMute] = ("M3.5 8 H6 L10 4.5 V15.5 L6 12 H3.5 Z M13 8 L17 12 M17 8 L13 12", false),
        [IconKind.MarkIn] = ("M9.5 3.5 H5.5 V16.5 H9.5 M12.5 10 H15.5", false),
        [IconKind.MarkOut] = ("M10.5 3.5 H14.5 V16.5 H10.5 M4.5 10 H7.5", false),
        [IconKind.Trim] = ("M6 7.5 A2.5 2.5 0 1 0 6 2.5 A2.5 2.5 0 1 0 6 7.5 M6 17.5 A2.5 2.5 0 1 0 6 12.5 A2.5 2.5 0 1 0 6 17.5 M8 6.5 L16.5 15 M8 13.5 L16.5 5", false),
        [IconKind.Info] = ("M10 17.5 A7.5 7.5 0 1 0 10 2.5 A7.5 7.5 0 1 0 10 17.5 M10 9 V14 M10 6.2 V6.3", false),
        [IconKind.Settings] = ("M3.5 6.5 H10 M14 6.5 H16.5 M12 4.5 A2 2 0 1 0 12 8.5 A2 2 0 1 0 12 4.5 M3.5 13.5 H5.5 M9.5 13.5 H16.5 M7.5 11.5 A2 2 0 1 0 7.5 15.5 A2 2 0 1 0 7.5 11.5", false),
        [IconKind.Folder] = ("M2.5 6 A1.5 1.5 0 0 1 4 4.5 H7.5 L9.5 6.5 H16 A1.5 1.5 0 0 1 17.5 8 V14.5 A1.5 1.5 0 0 1 16 16 H4 A1.5 1.5 0 0 1 2.5 14.5 Z", false),
        [IconKind.VideoFile] = ("M3.5 5.5 A1.5 1.5 0 0 1 5 4 H15 A1.5 1.5 0 0 1 16.5 5.5 V14.5 A1.5 1.5 0 0 1 15 16 H5 A1.5 1.5 0 0 1 3.5 14.5 Z M8.5 7.5 L12.5 10 L8.5 12.5 Z", false),
        [IconKind.ChevronLeft] = ("M12 5 L7 10 L12 15", false),
        [IconKind.ChevronRight] = ("M8 5 L13 10 L8 15", false),
        [IconKind.ChevronUp] = ("M5 12 L10 7 L15 12", false),
        [IconKind.ChevronDown] = ("M5 8 L10 13 L15 8", false),
        [IconKind.ArrowUp] = ("M10 16 V4.5 M5.5 9 L10 4.5 L14.5 9", false),
        [IconKind.Search] = ("M9 15 A6 6 0 1 0 9 3 A6 6 0 1 0 9 15 M13.3 13.3 L17 17", false),
        [IconKind.Close] = ("M5.5 5.5 L14.5 14.5 M14.5 5.5 L5.5 14.5", false),
        [IconKind.Star] = ("M10 2.8 L12.2 7.3 L17.1 8 L13.5 11.4 L14.4 16.3 L10 14 L5.6 16.3 L6.5 11.4 L2.9 8 L7.8 7.3 Z", false),
        [IconKind.StarFilled] = ("M10 2.8 L12.2 7.3 L17.1 8 L13.5 11.4 L14.4 16.3 L10 14 L5.6 16.3 L6.5 11.4 L2.9 8 L7.8 7.3 Z", true),
        [IconKind.Fullscreen] = ("M3.5 7.5 V3.5 H7.5 M12.5 3.5 H16.5 V7.5 M16.5 12.5 V16.5 H12.5 M7.5 16.5 H3.5 V12.5", false),
        [IconKind.ExitFullscreen] = ("M7.5 3.5 V7.5 H3.5 M16.5 7.5 H12.5 V3.5 M12.5 16.5 V12.5 H16.5 M3.5 12.5 H7.5 V16.5", false),
        [IconKind.PanelLeft] = ("M3.5 5 A1.5 1.5 0 0 1 5 3.5 H15 A1.5 1.5 0 0 1 16.5 5 V15 A1.5 1.5 0 0 1 15 16.5 H5 A1.5 1.5 0 0 1 3.5 15 Z M8 3.5 V16.5", false),
        [IconKind.PanelRight] = ("M3.5 5 A1.5 1.5 0 0 1 5 3.5 H15 A1.5 1.5 0 0 1 16.5 5 V15 A1.5 1.5 0 0 1 15 16.5 H5 A1.5 1.5 0 0 1 3.5 15 Z M12 3.5 V16.5", false),
        [IconKind.PanelBottom] = ("M3.5 5 A1.5 1.5 0 0 1 5 3.5 H15 A1.5 1.5 0 0 1 16.5 5 V15 A1.5 1.5 0 0 1 15 16.5 H5 A1.5 1.5 0 0 1 3.5 15 Z M3.5 12 H16.5", false),
        [IconKind.Camera] = ("M3 7 A1.5 1.5 0 0 1 4.5 5.5 H6.5 L8 3.5 H12 L13.5 5.5 H15.5 A1.5 1.5 0 0 1 17 7 V14.5 A1.5 1.5 0 0 1 15.5 16 H4.5 A1.5 1.5 0 0 1 3 14.5 Z M10 13.5 A3 3 0 1 0 10 7.5 A3 3 0 1 0 10 13.5", false),
        [IconKind.Copy] = ("M7.5 7.5 A1 1 0 0 1 8.5 6.5 H15.5 A1 1 0 0 1 16.5 7.5 V15.5 A1 1 0 0 1 15.5 16.5 H8.5 A1 1 0 0 1 7.5 15.5 Z M4.5 13.5 A1 1 0 0 1 3.5 12.5 V4.5 A1 1 0 0 1 4.5 3.5 H12.5 A1 1 0 0 1 13.5 4.5", false),
        [IconKind.External] = ("M11 3.5 H16.5 V9 M16.5 3.5 L9.5 10.5 M14 12 V15.5 A1 1 0 0 1 13 16.5 H4.5 A1 1 0 0 1 3.5 15.5 V7 A1 1 0 0 1 4.5 6 H8", false),
        [IconKind.Trash] = ("M4 6 H16 M8 6 V4.5 A1 1 0 0 1 9 3.5 H11 A1 1 0 0 1 12 4.5 V6 M5.5 6 L6.2 15.6 A1 1 0 0 0 7.2 16.5 H12.8 A1 1 0 0 0 13.8 15.6 L14.5 6 M8.5 9 V13.5 M11.5 9 V13.5", false),
        [IconKind.Pencil] = ("M12.5 4.5 L15.5 7.5 L7.5 15.5 L4 16 L4.5 12.5 Z M11 6 L14 9", false),
        [IconKind.Eye] = ("M2.5 10 C4.5 6 7 4.5 10 4.5 C13 4.5 15.5 6 17.5 10 C15.5 14 13 15.5 10 15.5 C7 15.5 4.5 14 2.5 10 Z M10 12.5 A2.5 2.5 0 1 0 10 7.5 A2.5 2.5 0 1 0 10 12.5", false),
        [IconKind.Clock] = ("M10 17.5 A7.5 7.5 0 1 0 10 2.5 A7.5 7.5 0 1 0 10 17.5 M10 6 V10 L12.8 11.8", false),
        [IconKind.Check] = ("M4.5 10.5 L8.5 14.5 L15.5 6", false),
        [IconKind.Warning] = ("M10 3.5 L17.5 16.5 H2.5 Z M10 8.5 V11.5 M10 14 V14.1", false),
        [IconKind.Plus] = ("M10 4.5 V15.5 M4.5 10 H15.5", false),
        [IconKind.Minus] = ("M4.5 10 H15.5", false),
        [IconKind.Speed] = ("M3.5 14 A6.5 6.5 0 1 1 16.5 14 M10 13.5 L13 8.5", false),
        [IconKind.Audio] = ("M3.5 8.5 V11.5 M6.5 6 V14 M9.5 3.5 V16.5 M12.5 6.5 V13.5 M15.5 8.5 V11.5", false),
        [IconKind.Refresh] = ("M16 10 A6 6 0 1 1 13.8 5.4 M14.5 2.5 V5.8 H11.2", false),
        // A stack of clips, the front one crossed out.
        [IconKind.CloseAll] = ("M7 3.5 H15.5 A1 1 0 0 1 16.5 4.5 V13 M3.5 7.5 A1 1 0 0 1 4.5 6.5 H12.5 A1 1 0 0 1 13.5 7.5 V15.5 A1 1 0 0 1 12.5 16.5 H4.5 A1 1 0 0 1 3.5 15.5 Z M6.5 9.5 L10.5 13.5 M10.5 9.5 L6.5 13.5", false),
    };

    private readonly Path _path = new()
    {
        StrokeStartLineCap = PenLineCap.Round,
        StrokeEndLineCap = PenLineCap.Round,
        StrokeLineJoin = PenLineJoin.Round,
        Width = 20,
        Height = 20,
    };

    public Icon()
    {
        IsTabStop = false;
        Width = Height = 16;
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        Content = new Viewbox { Stretch = Stretch.Uniform, Child = new Grid { Width = 20, Height = 20, Children = { _path } } };
        RegisterPropertyChangedCallback(WidthProperty, (_, _) => Apply());
        RegisterPropertyChangedCallback(ForegroundProperty, (_, _) => ApplyBrush());
        // The inherited Foreground can settle after the theme change reaches this element (a binding
        // to it lagged one theme behind), so read it again once the change has completed.
        ActualThemeChanged += (_, _) => DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, ApplyBrush);
    }

    public IconKind Kind
    {
        get => (IconKind)GetValue(KindProperty);
        set => SetValue(KindProperty, value);
    }

    public static readonly DependencyProperty KindProperty =
        DependencyProperty.Register(nameof(Kind), typeof(IconKind), typeof(Icon), new PropertyMetadata(IconKind.None, (d, _) => ((Icon)d).Apply()));

    private void Apply()
    {
        _path.ClearValue(Shape.FillProperty);
        _path.ClearValue(Shape.StrokeProperty);
        if (!Glyphs.TryGetValue(Kind, out var glyph))
        {
            _path.Data = null;
            return;
        }
        _path.Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), glyph.Data);
        // Keep the optical stroke weight near 1.25 px at small sizes.
        double scale = (double.IsNaN(Width) ? 16 : Width) / 20.0;
        _path.StrokeThickness = scale >= 1 ? 1.5 : Math.Min(1.9, 1.25 / scale);
        ApplyBrush();
    }

    private void ApplyBrush()
    {
        if (Glyphs.TryGetValue(Kind, out var glyph)) _path.SetValue(glyph.Filled ? Shape.FillProperty : Shape.StrokeProperty, Foreground);
    }
}