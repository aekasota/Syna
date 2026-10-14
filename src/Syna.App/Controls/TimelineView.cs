using Syna.Core.Services;
using Syna.Services;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Path = Microsoft.UI.Xaml.Shapes.Path;
using Windows.Foundation;

namespace Syna.Controls;

/// <summary>
/// Time ruler with keyframe ticks, the IN/OUT selection, the playhead and scrubbing. Drawn with a
/// fixed, small set of shapes (one Path for all ticks), so its cost does not grow with clip length.
/// </summary>
public sealed partial class TimelineView : Grid
{
    private const double HandleHitWidth = 10;
    private readonly Canvas _canvas = new() { IsHitTestVisible = false };
    private readonly Rectangle _track = new() { Height = 4, RadiusX = 2, RadiusY = 2 };
    private readonly Rectangle _played = new() { Height = 4, RadiusX = 2, RadiusY = 2 };
    private readonly Rectangle _range = new();
    private readonly Rectangle _inHandle = new() { Width = 2 };
    private readonly Rectangle _outHandle = new() { Width = 2 };
    private readonly Path _ticks = new() { StrokeThickness = 1 };
    private readonly Rectangle _playhead = new() { Width = 2 };
    private readonly Polygon _playheadCap = new();
    private readonly Rectangle _hover = new() { Width = 1, Opacity = 0 };
    private readonly Border _hoverLabel = new() { Padding = new Thickness(6, 2, 6, 2), CornerRadius = new CornerRadius(4), Opacity = 0 };
    private readonly TextBlock _hoverText = new() { FontSize = 11 };
    private readonly List<TextBlock> _labels = [];
    private enum Drag { None, Scrub, In, Out }
    private Drag _drag;
    private long _lastScrubUs = -1;

    public event Action<long, bool>? SeekRequested;
    public event Action<long, long>? RangeChanged;
    public event Action<bool>? ScrubbingChanged;
    public event Action<long, long>? ViewChanged;

    public TimelineView()
    {
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Children.Add(_canvas);
        _canvas.Children.Add(_track);
        _canvas.Children.Add(_played);
        _canvas.Children.Add(_range);
        _canvas.Children.Add(_ticks);
        _canvas.Children.Add(_inHandle);
        _canvas.Children.Add(_outHandle);
        _canvas.Children.Add(_hover);
        _canvas.Children.Add(_playhead);
        _canvas.Children.Add(_playheadCap);
        _hoverLabel.Child = _hoverText;
        _canvas.Children.Add(_hoverLabel);
        _playheadCap.Points = [new Point(-5, 0), new Point(5, 0), new Point(0, 6)];
        SizeChanged += (_, _) => Redraw();
        ActualThemeChanged += (_, _) => ApplyBrushes();
        Loaded += (_, _) => ApplyBrushes();
        PointerPressed += OnPressed;
        PointerMoved += OnMoved;
        PointerReleased += OnReleased;
        PointerCaptureLost += (_, _) => EndDrag(commit: true);
        PointerExited += (_, _) => { _hover.Opacity = 0; _hoverLabel.Opacity = 0; };
        PointerWheelChanged += OnWheel;
    }

    public long DurationUs { get; private set; }
    public long PositionUs { get; private set; }
    public long InUs { get; private set; } = -1;
    public long OutUs { get; private set; } = -1;
    public long ViewStartUs { get; private set; }
    public long ViewEndUs { get; private set; }
    private long[] _keyframes = [];

    public void SetMedia(long durationUs, long[] keyframes)
    {
        bool changed = durationUs != DurationUs;
        DurationUs = durationUs;
        _keyframes = keyframes;
        if (changed || ViewEndUs <= ViewStartUs) SetView(0, durationUs);
        else Redraw();
    }

    public void SetPosition(long us)
    {
        PositionUs = us;
        PlaceRunningParts();
    }

    public void SetRange(long inUs, long outUs)
    {
        InUs = inUs;
        OutUs = outUs;
        Redraw();
    }

    public void SetView(long start, long end)
    {
        if (DurationUs <= 0)
        {
            ViewStartUs = 0;
            ViewEndUs = 1;
        }
        else
        {
            long min = Math.Min(DurationUs, 200_000);  // never zoom past ~0.2 s across the full width
            long span = Math.Clamp(end - start, min, DurationUs);
            start = Math.Clamp(start, 0, DurationUs - span);
            ViewStartUs = start;
            ViewEndUs = start + span;
        }
        Redraw();
        ViewChanged?.Invoke(ViewStartUs, ViewEndUs);
    }

    private double X(long us) => ActualWidth <= 0 || ViewEndUs <= ViewStartUs ? 0 : (us - ViewStartUs) * ActualWidth / (ViewEndUs - ViewStartUs);
    private long Us(double x) => ViewStartUs + (long)((ViewEndUs - ViewStartUs) * Math.Clamp(x / Math.Max(1, ActualWidth), 0, 1));

    private void ApplyBrushes()
    {
        ThemeColors.Theme = ActualTheme;
        static Brush B(string key) => ThemeColors.Brush(key);
        _track.Fill = B("MeterTrackBrush");
        _played.Fill = B("BorderStrongBrush");
        _range.Fill = B("AccentSubtleBrush");
        _inHandle.Fill = _outHandle.Fill = B("AccentBrush");
        _ticks.Stroke = B("KeyframeTickBrush");
        _playhead.Fill = _playheadCap.Fill = B("PlayheadBrush");
        _hover.Fill = B("TextTertiaryBrush");
        _hoverLabel.Background = B("Surface3Brush");
        _hoverText.Foreground = B("TextPrimaryBrush");
        foreach (var l in _labels) l.Foreground = B("TextTertiaryBrush");
    }

    private void Redraw()
    {
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        double trackY = h - 12;
        Canvas.SetTop(_track, trackY);
        _track.Width = w;
        Canvas.SetTop(_played, trackY);

        // Ruler labels at a step that keeps them >= 90 px apart.
        double usPerPx = (ViewEndUs - ViewStartUs) / w;
        double[] steps = [0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600];
        double step = steps.FirstOrDefault(s => s * 1e6 / usPerPx >= 90, 7200);
        long stepUs = (long)(step * 1e6);
        long first = (ViewStartUs + stepUs - 1) / stepUs * stepUs;
        int n = 0;
        for (long t = first; t <= ViewEndUs && n < 64; t += stepUs, n++)
        {
            if (n >= _labels.Count)
            {
                var tb = new TextBlock { FontSize = 11, Foreground = ThemeColors.Brush("TextTertiaryBrush") };
                tb.SetValue(Typography.NumeralAlignmentProperty, FontNumeralAlignment.Tabular);
                _labels.Add(tb);
                _canvas.Children.Insert(0, tb);
            }
            var label = _labels[n];
            label.Visibility = Visibility.Visible;
            label.Text = step < 1 ? Timecode.Precise(t)[..^2] : Timecode.Short(t);
            // Labels sit right of their tick; the last one flips left so it is never clipped.
            label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            double left = X(t) + 4;
            if (left + label.DesiredSize.Width > w) left = X(t) - 4 - label.DesiredSize.Width;
            Canvas.SetLeft(label, left);
            Canvas.SetTop(label, 2);
        }
        for (int i = n; i < _labels.Count; i++) _labels[i].Visibility = Visibility.Collapsed;

        // Keyframe ticks, only when they are far enough apart to read.
        var geometry = new GeometryGroup();
        var visible = _keyframes.Where(k => k >= ViewStartUs && k <= ViewEndUs).ToArray();
        if (visible.Length > 0 && w / visible.Length >= 3)
        {
            foreach (long k in visible)
            {
                double x = Math.Round(X(k)) + 0.5;
                geometry.Children.Add(new LineGeometry { StartPoint = new Point(x, trackY - 7), EndPoint = new Point(x, trackY - 2) });
            }
        }
        _ticks.Data = geometry;

        // Selection.
        bool hasRange = InUs >= 0 || OutUs >= 0;
        long a = Math.Max(0, InUs), b = OutUs >= 0 ? OutUs : DurationUs;
        _range.Visibility = _inHandle.Visibility = _outHandle.Visibility = hasRange && b > a ? Visibility.Visible : Visibility.Collapsed;
        if (hasRange && b > a)
        {
            double xa = Math.Clamp(X(a), -2, w + 2), xb = Math.Clamp(X(b), -2, w + 2);
            Canvas.SetLeft(_range, xa);
            Canvas.SetTop(_range, 18);
            _range.Width = Math.Max(0, xb - xa);
            _range.Height = h - 18;
            Canvas.SetLeft(_inHandle, xa - 1);
            Canvas.SetTop(_inHandle, 18);
            _inHandle.Height = h - 18;
            _inHandle.Visibility = InUs >= 0 ? Visibility.Visible : Visibility.Collapsed;
            Canvas.SetLeft(_outHandle, xb - 1);
            Canvas.SetTop(_outHandle, 18);
            _outHandle.Height = h - 18;
            _outHandle.Visibility = OutUs >= 0 ? Visibility.Visible : Visibility.Collapsed;
        }
        PlaceRunningParts();
    }

    private void PlaceRunningParts()
    {
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0) return;
        double x = X(PositionUs);
        bool inView = x >= -1 && x <= w + 1;
        _playhead.Visibility = _playheadCap.Visibility = inView ? Visibility.Visible : Visibility.Collapsed;
        Canvas.SetLeft(_playhead, Math.Round(x) - 1);
        Canvas.SetTop(_playhead, 18);
        _playhead.Height = h - 18;
        Canvas.SetLeft(_playheadCap, Math.Round(x));
        Canvas.SetTop(_playheadCap, 18);
        _played.Width = Math.Clamp(x, 0, w);
    }

    private Drag HitTest(double x)
    {
        if (OutUs >= 0 && Math.Abs(x - X(OutUs)) <= HandleHitWidth / 2) return Drag.Out;
        if (InUs >= 0 && Math.Abs(x - X(InUs)) <= HandleHitWidth / 2) return Drag.In;
        return Drag.Scrub;
    }

    private void OnPressed(object sender, PointerRoutedEventArgs e)
    {
        if (DurationUs <= 0) return;
        var p = e.GetCurrentPoint(this);
        if (!p.Properties.IsLeftButtonPressed) return;
        _drag = HitTest(p.Position.X);
        CapturePointer(e.Pointer);
        if (_drag == Drag.Scrub)
        {
            ScrubbingChanged?.Invoke(true);
            Scrub(p.Position.X, exact: false);
        }
        e.Handled = true;
    }

    private void OnMoved(object sender, PointerRoutedEventArgs e)
    {
        double x = e.GetCurrentPoint(this).Position.X;
        UpdateHover(x);
        ProtectedCursor = InputSystemCursor.Create(_drag is Drag.In or Drag.Out || HitTest(x) is Drag.In or Drag.Out ? InputSystemCursorShape.SizeWestEast : InputSystemCursorShape.Arrow);
        switch (_drag)
        {
            case Drag.Scrub:
                Scrub(x, exact: false);
                break;
            case Drag.In:
                InUs = Math.Min(Us(x), OutUs >= 0 ? OutUs - 1000 : DurationUs);
                Redraw();
                break;
            case Drag.Out:
                OutUs = Math.Max(Us(x), InUs >= 0 ? InUs + 1000 : 0);
                Redraw();
                break;
        }
    }

    private void OnReleased(object sender, PointerRoutedEventArgs e)
    {
        EndDrag(commit: true);
        ReleasePointerCapture(e.Pointer);
    }

    private void EndDrag(bool commit)
    {
        var drag = _drag;
        _drag = Drag.None;
        if (!commit) return;
        if (drag == Drag.Scrub)
        {
            if (_lastScrubUs >= 0) SeekRequested?.Invoke(_lastScrubUs, true);
            ScrubbingChanged?.Invoke(false);
            _lastScrubUs = -1;
        }
        else if (drag is Drag.In or Drag.Out)
        {
            RangeChanged?.Invoke(InUs, OutUs);
        }
    }

    private void Scrub(double x, bool exact)
    {
        long us = Us(x);
        if (us == _lastScrubUs) return;
        _lastScrubUs = us;
        PositionUs = us;
        PlaceRunningParts();
        SeekRequested?.Invoke(us, exact);
    }

    private void UpdateHover(double x)
    {
        if (DurationUs <= 0) return;
        _hover.Opacity = 0.6;
        _hoverLabel.Opacity = 1;
        Canvas.SetLeft(_hover, Math.Round(x));
        Canvas.SetTop(_hover, 18);
        _hover.Height = Math.Max(0, ActualHeight - 18);
        _hoverText.Text = Timecode.Precise(Us(x));
        _hoverLabel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        double lw = _hoverLabel.DesiredSize.Width;
        Canvas.SetLeft(_hoverLabel, Math.Clamp(x - lw / 2, 0, Math.Max(0, ActualWidth - lw)));
        Canvas.SetTop(_hoverLabel, 0);
    }

    private void OnWheel(object sender, PointerRoutedEventArgs e)
    {
        if (DurationUs <= 0) return;
        var p = e.GetCurrentPoint(this);
        int delta = p.Properties.MouseWheelDelta;
        long span = ViewEndUs - ViewStartUs;
        bool ctrl = (e.KeyModifiers & Windows.System.VirtualKeyModifiers.Control) != 0;
        if (ctrl)
        {
            long anchor = Us(p.Position.X);
            double factor = delta > 0 ? 0.8 : 1.25;
            long newSpan = (long)(span * factor);
            double frac = (anchor - ViewStartUs) / (double)Math.Max(1, span);
            long start = anchor - (long)(newSpan * frac);
            SetView(start, start + newSpan);
        }
        else if (span < DurationUs)
        {
            long shift = (long)(span * 0.15) * (delta > 0 ? -1 : 1);
            SetView(ViewStartUs + shift, ViewEndUs + shift);
        }
        e.Handled = true;
    }
}
