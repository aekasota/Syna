using System.ComponentModel;
using Syna.Controls;
using Syna.Core.Services;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace Syna.Views;

public sealed partial class EditorStrip : UserControl
{
    private readonly Dictionary<AudioTrackViewModel, WaveformView> _lanes = [];
    private SessionViewModel? _session;
    private bool _laneScrub;

    public EditorStrip()
    {
        ViewModel = App.Current!.ViewModel!;
        InitializeComponent();
        Timeline.SeekRequested += (us, exact) => ViewModel.SeekTo(us, exact);
        Timeline.ScrubbingChanged += s => ViewModel.IsScrubbing = s;
        Timeline.RangeChanged += (a, b) => ViewModel.SetRange(a, b);
        Timeline.ViewChanged += (_, _) => UpdateLaneViews();
        ViewModel.PropertyChanged += OnViewModelChanged;
        LaneOverlay.SizeChanged += (_, _) => PlaceLanePlayhead();
        PointerEntered += (_, _) => IsPointerOver = true;
        PointerExited += (_, _) => IsPointerOver = false;
        PointerCanceled += (_, _) => IsPointerOver = false;
        Bind(ViewModel.ActiveSession);
    }

    public MainViewModel ViewModel { get; }

    /// <summary>Keeps fullscreen chrome visible while the pointer rests on the strip.</summary>
    public bool IsPointerOver { get; private set; }

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(MainViewModel.ActiveSession):
                Bind(ViewModel.ActiveSession);
                break;
            case nameof(MainViewModel.DurationUs):
                SyncMedia();
                break;
        }
    }

    private void Bind(SessionViewModel? session)
    {
        if (_session != null) _session.PropertyChanged -= OnSessionChanged;
        _session = session;
        if (session != null) session.PropertyChanged += OnSessionChanged;
        Lanes.ItemsSource = session?.Tracks;
        SyncMedia();
        SyncRange();
        SyncWaveforms();
    }

    private void OnSessionChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(SessionViewModel.Info):
            case nameof(SessionViewModel.DurationUs):
                SyncMedia();
                break;
            case nameof(SessionViewModel.InUs):
            case nameof(SessionViewModel.OutUs):
                SyncRange();
                break;
            case nameof(SessionViewModel.Waveform):
                SyncWaveforms();
                break;
        }
    }

    private void SyncMedia()
    {
        long duration = ViewModel.DurationUs > 0 ? ViewModel.DurationUs : _session?.DurationUs ?? 0;
        Timeline.SetMedia(duration, _session?.Keyframes ?? []);
        UpdateLaneViews();
    }

    private void SyncRange()
    {
        long a = _session?.InUs ?? -1, b = _session?.OutUs ?? -1;
        Timeline.SetRange(a, b);
        InText.Text = a >= 0 ? Timecode.Precise(a) : "IN";
        OutText.Text = b >= 0 ? Timecode.Precise(b) : "OUT";
        ClearRangeButton.Visibility = a >= 0 || b >= 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void SyncWaveforms()
    {
        foreach (var (track, view) in _lanes) ApplyLane(track, view);
    }

    private void ApplyLane(AudioTrackViewModel track, WaveformView view)
    {
        var wf = _session?.Waveform;
        var data = wf != null && track.Index < wf.Tracks.Count ? wf.Tracks[track.Index] : null;
        view.SetTrack(data, ThemeColors.Track(track.Index));
        view.SetView(Timeline.ViewStartUs, Timeline.ViewEndUs);
    }

    private void UpdateLaneViews()
    {
        foreach (var view in _lanes.Values) view.SetView(Timeline.ViewStartUs, Timeline.ViewEndUs);
        PlaceLanePlayhead();
    }

    public void OnThemeChanged() => SyncWaveforms();

    /// <summary>Per UI frame while playing (and on the idle tick).</summary>
    public void OnTick()
    {
        if (!ViewModel.IsScrubbing) Timeline.SetPosition(ViewModel.PositionUs);
        PlaceLanePlayhead();
    }

    private void PlaceLanePlayhead()
    {
        double w = LaneOverlay.ActualWidth;
        long span = Timeline.ViewEndUs - Timeline.ViewStartUs;
        if (w <= 0 || span <= 0)
        {
            LanePlayhead.Visibility = Visibility.Collapsed;
            return;
        }
        double x = (Timeline.PositionUs - Timeline.ViewStartUs) * w / span;
        LanePlayhead.Visibility = x >= 0 && x <= w ? Visibility.Visible : Visibility.Collapsed;
        LanePlayhead.Height = LaneOverlay.ActualHeight;
        Canvas.SetLeft(LanePlayhead, Math.Round(x));
    }

    /// <summary>Fullscreen: a translucent bar with timeline and transport only.</summary>
    public void SetOverlayMode(bool on)
    {
        Lanes.Visibility = on || !ViewModel.AudioPanelVisible ? Visibility.Collapsed : Visibility.Visible;
        LaneOverlay.Visibility = Lanes.Visibility;
        VisualStateManager.GoToState(this, on ? "Overlay" : "Docked", false);
        RequestedTheme = on ? ElementTheme.Dark : ElementTheme.Default;
    }

    private void OnWaveformLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is WaveformView view && view.DataContext is AudioTrackViewModel track)
        {
            _lanes[track] = view;
            ApplyLane(track, view);
        }
    }

    private void OnWaveformUnloaded(object sender, RoutedEventArgs e)
    {
        if (sender is WaveformView view && view.DataContext is AudioTrackViewModel track && _lanes.TryGetValue(track, out var v) && v == view)
            _lanes.Remove(track);
    }

    private long LaneUs(UIElement lane, PointerRoutedEventArgs e)
    {
        double x = e.GetCurrentPoint(lane).Position.X;
        double w = ((FrameworkElement)lane).ActualWidth;
        return Timeline.ViewStartUs + (long)((Timeline.ViewEndUs - Timeline.ViewStartUs) * Math.Clamp(x / Math.Max(1, w), 0, 1));
    }

    private void OnLanePointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!e.GetCurrentPoint((UIElement)sender).Properties.IsLeftButtonPressed || ViewModel.DurationUs <= 0) return;
        _laneScrub = true;
        ((UIElement)sender).CapturePointer(e.Pointer);
        ViewModel.IsScrubbing = true;
        long us = LaneUs((UIElement)sender, e);
        Timeline.SetPosition(us);
        ViewModel.SeekTo(us, false);
        e.Handled = true;
    }

    private void OnLanePointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_laneScrub) return;
        long us = LaneUs((UIElement)sender, e);
        Timeline.SetPosition(us);
        ViewModel.SeekTo(us, false);
    }

    private void OnLanePointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (!_laneScrub) return;
        _laneScrub = false;
        ((UIElement)sender).ReleasePointerCapture(e.Pointer);
        ViewModel.SeekTo(LaneUs((UIElement)sender, e), true);
        ViewModel.IsScrubbing = false;
    }

    private void OnLabelDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: AudioTrackViewModel track }) _ = RenameTrackAsync(track);
    }

    private async Task RenameTrackAsync(AudioTrackViewModel track)
    {
        if (App.Current?.Window is not IShell shell) return;
        string? name = await shell.PromptAsync("Rename track", track.Label, "Rename");
        if (name != null) track.Label = name;
    }

    private void OnGainDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: AudioTrackViewModel track }) ResetVolume(track);
    }

    // Back to the track's default volume (Settings > Audio), which new clips also start at.
    private void ResetVolume(AudioTrackViewModel track) => track.Gain = ViewModel.Settings.DefaultGain(track.Index);

    private void OnLaneContextRequested(UIElement sender, ContextRequestedEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: AudioTrackViewModel track }) return;
        var menu = new MenuFlyout();
        void Add(string text, Action action, string? accel = null)
        {
            var item = new MenuFlyoutItem { Text = text, KeyboardAcceleratorTextOverride = accel ?? "" };
            item.Click += (_, _) => action();
            menu.Items.Add(item);
        }
        Add(track.Muted ? "Unmute" : "Mute", () => track.Muted = !track.Muted, $"Alt+{track.Index + 1}");
        Add(track.Soloed ? "Unsolo" : "Solo", () => track.Soloed = !track.Soloed, $"Alt+Shift+{track.Index + 1}");
        Add("Reset volume", () => ResetVolume(track));
        Add("Rename track…", () => _ = RenameTrackAsync(track));
        menu.Items.Add(new MenuFlyoutSeparator());
        Add("Delay by 10 ms", () => track.OffsetMs += 10);
        Add("Advance by 10 ms", () => track.OffsetMs -= 10);
        Add("Delay by 50 ms", () => track.OffsetMs += 50);
        Add("Advance by 50 ms", () => track.OffsetMs -= 50);
        if (track.HasOffset) Add($"Reset offset ({track.OffsetText})", () => track.OffsetMs = 0);
        if (e.TryGetPosition(sender, out var point)) menu.ShowAt(sender, point);
        else menu.ShowAt((FrameworkElement)sender);
        e.Handled = true;
    }

    private void OnPlayClick(object sender, RoutedEventArgs e) => ViewModel.TogglePlay();
    private void OnFrameBackClick(object sender, RoutedEventArgs e) => ViewModel.StepFrame(-1);
    private void OnFrameForwardClick(object sender, RoutedEventArgs e) => ViewModel.StepFrame(1);
    private void OnSkipBackClick(object sender, RoutedEventArgs e) => ViewModel.Skip(-ViewModel.Settings.ShortSkipMs);
    private void OnSkipForwardClick(object sender, RoutedEventArgs e) => ViewModel.Skip(ViewModel.Settings.ShortSkipMs);
    private void OnInClick(object sender, RoutedEventArgs e) => ViewModel.SetIn();
    private void OnOutClick(object sender, RoutedEventArgs e) => ViewModel.SetOut();
    private void OnClearRangeClick(object sender, RoutedEventArgs e) => ViewModel.ClearRange();
    private void OnSaveFrameClick(object sender, RoutedEventArgs e) => _ = ViewModel.SaveFrameAsync(false);
    private void OnTrimClick(object sender, RoutedEventArgs e) => ViewModel.OpenTrim();
    private void OnFullscreenClick(object sender, RoutedEventArgs e) => ViewModel.ToggleFullscreen();

    private void OnRateMenuOpening(object? sender, object e)
    {
        RateMenu.Items.Clear();
        foreach (double r in new[] { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 })
        {
            var item = new RadioMenuFlyoutItem { Text = $"{r:0.##}×", IsChecked = Math.Abs(ViewModel.Rate - r) < 0.001, GroupName = "rate" };
            double rate = r;
            item.Click += (_, _) => ViewModel.SetRate(rate);
            RateMenu.Items.Add(item);
        }
    }
}
