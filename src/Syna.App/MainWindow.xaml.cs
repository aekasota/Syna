using System.ComponentModel;
using System.Runtime.InteropServices;
using Syna.Controls;
using Syna.Core.Services;
using Syna.Core.State;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.Windows.Storage.Pickers;
using Windows.ApplicationModel.DataTransfer;
using Windows.Graphics;
using DispatcherQueuePriority = Microsoft.UI.Dispatching.DispatcherQueuePriority;
using DispatcherQueueTimer = Microsoft.UI.Dispatching.DispatcherQueueTimer;
using Windows.System;
using Windows.UI.Core;

namespace Syna;

public sealed partial class MainWindow : Window, IShell
{
    private readonly DispatcherQueueTimer _idleTimer;
    private readonly DispatcherQueueTimer _loadingTimer;
    private readonly DispatcherQueueTimer _chromeTimer;
    private readonly DispatcherQueueTimer _tapTimer;
    private readonly NotificationCenter _notifications;
    private readonly Dictionary<Notification, Border> _toastViews = [];
    private bool _renderHooked;
    private long _lastTickTicks;
    private bool _fullscreen;
    private bool _cursorHidden;
    private (bool Explorer, bool Rail, bool Audio) _beforeFullscreen;

    public MainWindow(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleDragArea);
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        AppWindow.Title = "Syna";
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico"));
        RestoreBounds();

        _idleTimer = DispatcherQueue.CreateTimer();
        _idleTimer.Interval = TimeSpan.FromMilliseconds(80);
        _idleTimer.Tick += (_, _) => OnTick();
        _loadingTimer = DispatcherQueue.CreateTimer();
        _loadingTimer.Interval = TimeSpan.FromMilliseconds(350);
        _loadingTimer.IsRepeating = false;
        _loadingTimer.Tick += (_, _) => LoadingBar.Visibility = ViewModel.IsLoading ? Visibility.Visible : Visibility.Collapsed;
        _chromeTimer = DispatcherQueue.CreateTimer();
        _chromeTimer.Interval = TimeSpan.FromMilliseconds(2400);
        _chromeTimer.IsRepeating = false;
        _chromeTimer.Tick += (_, _) => HideFullscreenChrome();
        _tapTimer = DispatcherQueue.CreateTimer();
        _tapTimer.Interval = TimeSpan.FromMilliseconds(230);
        _tapTimer.IsRepeating = false;
        _tapTimer.Tick += (_, _) => ViewModel.TogglePlay();
        _notifications = new NotificationCenter(TimeProvider.System, work => DispatcherQueue.TryEnqueue(() => work()));
        _notifications.Shown += AddToastView;
        _notifications.Dismissing += FadeToastView;
        _notifications.Removed += RemoveToastView;

        VideoPanel.Loaded += OnVideoPanelLoaded;
        VideoPanel.SizeChanged += (_, _) => ResizeVideo();
        VideoPanel.CompositionScaleChanged += (_, _) => ResizeVideo();
        Root.AddHandler(UIElement.PreviewKeyDownEvent, new KeyEventHandler(OnPreviewKeyDown), true);
        Root.ActualThemeChanged += (_, _) => OnThemeChanged();
        ViewModel.PropertyChanged += OnViewModelChanged;
        ViewModel.AttachShell(this);
        ApplyTheme(ViewModel.Settings.Theme);
        Closed += OnClosed;
        _idleTimer.Start();
        PopulateRecent();
    }

    public MainViewModel ViewModel { get; }

    private void OnVideoPanelLoaded(object sender, RoutedEventArgs e)
    {
        try
        {
            ViewModel.Playback.Attach(VideoPanel);
        }
        catch (Exception ex)
        {
            Log.Error("app", $"video surface init failed: {ex}");
            Toast("The graphics device could not be initialized. Video playback is unavailable.");
            return;
        }
        ResizeVideo();
        OnThemeChanged();
        ViewModel.ApplyAudioSettings();
        ViewModel.StartWatching();
        App.Current?.OnShellReady();
        ViewModel.WarmUpEncoders();
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        Log.Info("app", "main window closed");
        SaveBounds();
        UnhookRendering();
        _idleTimer.Stop();
        ShowCursor(true);
        ViewModel.Dispose();
    }

    private void RestoreBounds()
    {
        int[] b = ViewModel.Settings.WindowBounds;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        if (b.Length == 4 && b[2] >= 800 && b[3] >= 500 && b[0] < area.X + area.Width - 100 && b[1] < area.Y + area.Height - 100)
        {
            AppWindow.MoveAndResize(new RectInt32(b[0], b[1], b[2], b[3]));
        }
        else
        {
            int w = Math.Min(1600, area.Width - 80), h = Math.Min(960, area.Height - 80);
            AppWindow.MoveAndResize(new RectInt32(area.X + (area.Width - w) / 2, area.Y + (area.Height - h) / 2, w, h));
        }
        if (ViewModel.Settings.WindowMaximized && AppWindow.Presenter is OverlappedPresenter p) p.Maximize();
    }

    private void SaveBounds()
    {
        if (_fullscreen || AppWindow.Presenter is not OverlappedPresenter p) return;
        ViewModel.Settings.WindowMaximized = p.State == OverlappedPresenterState.Maximized;
        if (p.State == OverlappedPresenterState.Restored)
            ViewModel.Settings.WindowBounds = [AppWindow.Position.X, AppWindow.Position.Y, AppWindow.Size.Width, AppWindow.Size.Height];
    }

    public void BringToFront()
    {
        if (AppWindow.Presenter is OverlappedPresenter p && p.State == OverlappedPresenterState.Minimized) p.Restore();
        Activate();
        SetForegroundWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
    }

    public void OpenPaths(IReadOnlyList<string> paths)
    {
        if (paths.Count > 0) ViewModel.OpenPaths(paths);
    }

    private void PopulateRecent()
    {
        RecentList.Items.Clear();
        foreach (var r in ViewModel.Db.RecentFiles(12).Where(r => File.Exists(r.Path)).Take(5))
        {
            string path = r.Path;
            var button = new Button
            {
                Style = (Style)Application.Current.Resources["QuietButton"],
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Left,
                Padding = new Thickness(10, 6, 10, 6),
                Content = new TextBlock { Text = Path.GetFileName(path), Style = (Style)Application.Current.Resources["SmallText"] },
            };
            ToolTipService.SetToolTip(button, path);
            button.Click += (_, _) => ViewModel.OpenPaths([path]);
            RecentList.Items.Add(button);
        }
    }

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(MainViewModel.IsPlaying):
                if (ViewModel.IsPlaying) HookRendering();
                else UnhookRendering();
                break;
            case nameof(MainViewModel.Stage):
                LoadingBar.Visibility = Visibility.Collapsed;
                if (ViewModel.IsLoading) _loadingTimer.Start();
                if (ViewModel.IsEmpty) PopulateRecent();
                break;
            case nameof(MainViewModel.PaletteOpen) when !ViewModel.PaletteOpen:
            case nameof(MainViewModel.SettingsOpen) when !ViewModel.SettingsOpen:
                Root.Focus(FocusState.Programmatic);
                break;
        }
    }

    private void HookRendering()
    {
        if (_renderHooked) return;
        _renderHooked = true;
        _idleTimer.Stop();
        CompositionTarget.Rendering += OnRendering;
    }

    private void UnhookRendering()
    {
        if (!_renderHooked) return;
        _renderHooked = false;
        CompositionTarget.Rendering -= OnRendering;
        _idleTimer.Start();
    }

    private void OnRendering(object? sender, object e) => OnTick();

    private void OnTick()
    {
        long now = Environment.TickCount64;
        double seconds = _lastTickTicks == 0 ? 0.016 : Math.Min(0.25, (now - _lastTickTicks) / 1000.0);
        _lastTickTicks = now;
        ViewModel.Tick(seconds);
        Editor.OnTick();
    }

    private void ResizeVideo()
    {
        float sx = VideoPanel.CompositionScaleX, sy = VideoPanel.CompositionScaleY;
        int w = (int)Math.Round(VideoPanel.ActualWidth * sx), h = (int)Math.Round(VideoPanel.ActualHeight * sy);
        if (w > 0 && h > 0) ViewModel.Playback.Resize(w, h, sx, sy);
    }

    private void OnStageTapped(object sender, TappedRoutedEventArgs e)
    {
        if (ViewModel.Stage == StageState.Video) _tapTimer.Start();
    }

    private void OnStageDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        _tapTimer.Stop();
        if (ViewModel.HasSession) SetFullscreen(!_fullscreen);
    }

    private static bool Down(VirtualKey key) => InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        bool ctrl = Down(VirtualKey.Control), shift = Down(VirtualKey.Shift), alt = Down(VirtualKey.Menu);
        var key = e.Key;
        var vm = ViewModel;

        if (ctrl && !shift && key == VirtualKey.K) { vm.PaletteOpen = !vm.PaletteOpen; e.Handled = true; return; }
        if (vm.PaletteOpen || vm.SettingsOpen || vm.Trim.IsOpen || Dialogs.IsOpen) return;  // overlays handle their own keys
        if (key == VirtualKey.Escape)
        {
            if (_fullscreen) SetFullscreen(false);
            else if (vm.InfoVisible) vm.InfoVisible = false;
            e.Handled = true;
            return;
        }

        var focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        if (focused is TextBox or PasswordBox or AutoSuggestBox or RichEditBox) return;
        bool inList = focused is ListViewItem or ListView;
        bool inSlider = focused is Slider;

        bool handled = true;
        switch (key)
        {
            case VirtualKey.Space when !ctrl: vm.TogglePlay(); break;
            case VirtualKey.Left when !inSlider: if (ctrl) vm.Skip(-vm.Settings.LongSkipMs); else if (shift) vm.Skip(-vm.Settings.ShortSkipMs); else vm.StepFrame(-1); break;
            case VirtualKey.Right when !inSlider: if (ctrl) vm.Skip(vm.Settings.LongSkipMs); else if (shift) vm.Skip(vm.Settings.ShortSkipMs); else vm.StepFrame(1); break;
            case VirtualKey.Up when !inList && !inSlider: vm.MasterVolume += 0.05; break;
            case VirtualKey.Down when !inList && !inSlider: vm.MasterVolume -= 0.05; break;
            case VirtualKey.Home: vm.SeekTo(0, true); break;
            case VirtualKey.End: vm.SeekTo(vm.DurationUs - vm.FrameDurationUs, true); break;
            case VirtualKey.I when !ctrl: if (shift) vm.GoToIn(); else vm.SetIn(); break;
            case VirtualKey.O when ctrl && shift: _ = vm.PickAndOpenFolderAsync(); break;
            case VirtualKey.O when ctrl: _ = vm.PickAndOpenFilesAsync(); break;
            case VirtualKey.O: if (shift) vm.GoToOut(); else vm.SetOut(); break;
            case VirtualKey.M when !ctrl: vm.ToggleMasterMute(); break;
            case VirtualKey.F when !ctrl: SetFullscreen(!_fullscreen); break;
            case VirtualKey.F11: SetFullscreen(!_fullscreen); break;
            case VirtualKey.J when !ctrl: vm.Jkl('J'); break;
            case VirtualKey.K when !ctrl: vm.Jkl('K'); break;
            case VirtualKey.L when ctrl && shift: vm.RailVisible = !vm.RailVisible; break;
            case VirtualKey.L when !ctrl: vm.Jkl('L'); break;
            case VirtualKey.R when !ctrl: vm.ToggleLoop(); break;
            case (VirtualKey)219: vm.StepRate(-1); break;  // [
            case (VirtualKey)221: vm.StepRate(1); break;   // ]
            case VirtualKey.Back: vm.SetRate(1.0); break;
            case VirtualKey.T when ctrl: vm.OpenTrim(); break;
            case VirtualKey.S when ctrl && shift: _ = vm.SaveFrameAsync(false); break;
            case VirtualKey.C when ctrl && shift: _ = vm.SaveFrameAsync(true); break;
            case VirtualKey.C when ctrl && alt: vm.CopyActivePath(); break;
            case VirtualKey.E when ctrl && shift: vm.ExplorerVisible = !vm.ExplorerVisible; break;
            case VirtualKey.E when ctrl: vm.RevealActive(); break;
            case VirtualKey.D when ctrl && shift: vm.DiagnosticsVisible = !vm.DiagnosticsVisible; break;
            case VirtualKey.D when ctrl: if (vm.ActiveSession != null) vm.ToggleFavorite(vm.ActiveSession.Path); break;
            case VirtualKey.A when ctrl && shift: vm.AudioPanelVisible = !vm.AudioPanelVisible; break;
            case VirtualKey.X when ctrl && shift: vm.ClearRange(); break;
            case VirtualKey.W when ctrl && shift: _ = vm.CloseAllSessionsAsync(); break;
            case VirtualKey.W when ctrl: if (vm.ActiveSession != null) vm.CloseSession(vm.ActiveSession); break;
            case VirtualKey.Tab when ctrl: vm.CycleSession(shift ? -1 : 1); break;
            case VirtualKey.I when ctrl: if (vm.HasSession) vm.InfoVisible = !vm.InfoVisible; break;
            case (VirtualKey)188 when ctrl: vm.SettingsOpen = true; break;  // ,
            case >= VirtualKey.Number1 and <= VirtualKey.Number8 when alt:
                int track = key - VirtualKey.Number1;
                if (shift) vm.ToggleTrackSolo(track); else vm.ToggleTrackMute(track);
                break;
            default: handled = false; break;
        }
        if (handled)
        {
            e.Handled = true;
            if (_fullscreen) ShowFullscreenChrome();
        }
    }

    private void OnDragOver(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems) && !e.DataView.Contains(ViewModels.ExplorerDrag.Format)) return;
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = "Open";
        e.DragUIOverride.IsGlyphVisible = false;
        DropOverlay.Visibility = Visibility.Visible;
    }

    private void OnDragLeave(object sender, DragEventArgs e) => DropOverlay.Visibility = Visibility.Collapsed;

    private async void OnDrop(object sender, DragEventArgs e)
    {
        DropOverlay.Visibility = Visibility.Collapsed;
        var paths = new List<string>();
        if (e.DataView.Contains(ViewModels.ExplorerDrag.Format))
        {
            if (await e.DataView.GetDataAsync(ViewModels.ExplorerDrag.Format) is string text)
                paths.AddRange(text.Split('\n', StringSplitOptions.RemoveEmptyEntries));
        }
        else if (e.DataView.Contains(StandardDataFormats.StorageItems))
        {
            var items = await e.DataView.GetStorageItemsAsync();
            paths.AddRange(items.Select(i => i.Path).Where(p => !string.IsNullOrEmpty(p)));
        }
        if (paths.Count > 0) ViewModel.OpenPaths(paths);
    }

    public bool IsFullscreen => _fullscreen;

    public void SetFullscreen(bool on)
    {
        if (on == _fullscreen) return;
        _fullscreen = on;
        if (on)
        {
            _beforeFullscreen = (ViewModel.ExplorerVisible, ViewModel.RailVisible, ViewModel.AudioPanelVisible);
            AppWindow.SetPresenter(AppWindowPresenterKind.FullScreen);
            TitleBar.Visibility = Visibility.Collapsed;
            ViewModel.ExplorerVisible = false;
            ViewModel.RailVisible = false;
            ViewModel.InfoVisible = false;
            Grid.SetRow(Editor, 0);
            Editor.VerticalAlignment = VerticalAlignment.Bottom;
            Editor.SetOverlayMode(true);
            ShowFullscreenChrome();
        }
        else
        {
            AppWindow.SetPresenter(AppWindowPresenterKind.Overlapped);
            TitleBar.Visibility = Visibility.Visible;
            (ViewModel.ExplorerVisible, ViewModel.RailVisible, ViewModel.AudioPanelVisible) = _beforeFullscreen;
            Grid.SetRow(Editor, 1);
            Editor.VerticalAlignment = VerticalAlignment.Stretch;
            Editor.SetOverlayMode(false);
            Editor.Opacity = 1;
            _chromeTimer.Stop();
            ShowCursor(true);
        }
    }

    private void OnRootPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (_fullscreen) ShowFullscreenChrome();
    }

    private void ShowFullscreenChrome()
    {
        ShowCursor(true);
        Fade(Editor, 1, 120);
        _chromeTimer.Stop();
        _chromeTimer.Start();
    }

    private void HideFullscreenChrome()
    {
        if (!_fullscreen || Editor.IsPointerOver) return;
        Fade(Editor, 0, 200);
        ShowCursor(false);
    }

    private void ShowCursor(bool show)
    {
        if (show == !_cursorHidden) return;
        _cursorHidden = !show;
        Win32ShowCursor(show);
    }

    private static void Fade(UIElement element, double to, int ms)
    {
        var anim = new DoubleAnimation { To = to, Duration = TimeSpan.FromMilliseconds(ms), EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } };
        Storyboard.SetTarget(anim, element);
        Storyboard.SetTargetProperty(anim, "Opacity");
        var sb = new Storyboard();
        sb.Children.Add(anim);
        sb.Begin();
    }

    public async Task<IReadOnlyList<string>> PickFilesAsync()
    {
        var picker = new FileOpenPicker(AppWindow.Id) { SuggestedStartLocation = PickerLocationId.VideosLibrary, CommitButtonText = "Open" };
        foreach (string ext in MediaFiles.Extensions) picker.FileTypeFilter.Add(ext);
        var results = await picker.PickMultipleFilesAsync();
        return results?.Select(r => r.Path).ToList() ?? [];
    }

    public async Task<string?> PickFolderAsync()
    {
        var picker = new FolderPicker(AppWindow.Id) { SuggestedStartLocation = PickerLocationId.VideosLibrary };
        var result = await picker.PickSingleFolderAsync();
        return result?.Path;
    }

    public Task<bool> ConfirmAsync(string title, string message, string confirm) => Dialogs.ConfirmAsync(title, message, confirm);

    public Task<string?> PromptAsync(string title, string initial, string confirm) => Dialogs.PromptAsync(title, initial, confirm);

    public void Toast(string message, string? actionLabel = null, Action? action = null)
    {
        if (!DispatcherQueue.HasThreadAccess)
        {
            DispatcherQueue.TryEnqueue(() => Toast(message, actionLabel, action));
            return;
        }
        _notifications.Show(message, actionLabel, action);
    }

    // The view of one notification; NotificationCenter decides when it fades and goes.
    private void AddToastView(Notification n)
    {
        var text = new TextBlock
        {
            Text = n.Message, Style = (Style)Application.Current.Resources["BodyText"], VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 520, TextWrapping = TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        row.Children.Add(text);
        if (n.ActionLabel != null)
        {
            var action = new Button
            {
                Content = n.ActionLabel, Style = (Style)Application.Current.Resources["QuietButton"], Foreground = ThemeColors.Brush("AccentStrongTextBrush"),
                MinHeight = 26, Margin = new Thickness(10, 0, 0, 0),
            };
            action.Click += (_, _) => n.InvokeAction();
            row.Children.Add(action);
        }
        var close = new Button
        {
            Style = (Style)Application.Current.Resources["IconButton"], Width = 26, Height = 26, Margin = new Thickness(n.ActionLabel == null ? 10 : 0, 0, 0, 0),
            Content = new Icon { Kind = IconKind.Close, Width = 12, Height = 12 },
        };
        AutomationProperties.SetName(close, "Dismiss notification");
        ToolTipService.SetToolTip(close, "Dismiss");
        close.Click += (_, _) => n.Dismiss();
        row.Children.Add(close);
        var toast = new Border
        {
            Child = row,
            Background = ThemeColors.Brush("Surface2Brush"),
            BorderBrush = ThemeColors.Brush("BorderStrongBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(14, 5, 5, 5),
            Opacity = 0,
            Translation = new System.Numerics.Vector3(0, 8, 0),
        };
        toast.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(160) };
        toast.OpacityTransition = new ScalarTransition { Duration = NotificationCenter.FadeOut - TimeSpan.FromMilliseconds(20) };
        _toastViews[n] = toast;
        ToastHost.Children.Add(toast);
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            if (n.State != NotificationState.Visible) return;
            toast.Opacity = 1;
            toast.Translation = default;
        });
    }

    private void FadeToastView(Notification n)
    {
        if (!_toastViews.TryGetValue(n, out var toast)) return;
        toast.IsHitTestVisible = false;
        toast.Opacity = 0;
    }

    private void RemoveToastView(Notification n)
    {
        if (_toastViews.Remove(n, out var toast)) ToastHost.Children.Remove(toast);
    }

    public void ApplyTheme(ThemePreference theme)
    {
        Root.RequestedTheme = theme switch
        {
            ThemePreference.Dark => ElementTheme.Dark,
            ThemePreference.Light => ElementTheme.Light,
            _ => ElementTheme.Default,
        };
        OnThemeChanged();
    }

    private void OnThemeChanged()
    {
        ThemeColors.Theme = Root.ActualTheme;
        var tb = AppWindow.TitleBar;
        tb.ButtonBackgroundColor = Colors.Transparent;
        tb.ButtonInactiveBackgroundColor = Colors.Transparent;
        tb.ButtonForegroundColor = ThemeColors.Get("TextPrimaryColor");
        tb.ButtonInactiveForegroundColor = ThemeColors.Get("TextTertiaryColor");
        tb.ButtonHoverBackgroundColor = ThemeColors.Get("HoverOverlayColor");
        tb.ButtonHoverForegroundColor = ThemeColors.Get("TextPrimaryColor");
        tb.ButtonPressedBackgroundColor = ThemeColors.Get("PressedOverlayColor");
        tb.ButtonPressedForegroundColor = ThemeColors.Get("TextPrimaryColor");
        ViewModel.Playback.SetBackground(ThemeColors.Get("StageColor"));
        ViewModel.RefreshThemeColors();
        Editor.OnThemeChanged();
    }

    private void OnPaletteClick(object sender, RoutedEventArgs e) => ViewModel.PaletteOpen = true;
    private void OnSettingsClick(object sender, RoutedEventArgs e) => ViewModel.SettingsOpen = true;
    private void OnOpenFileClick(object sender, RoutedEventArgs e) => _ = ViewModel.PickAndOpenFilesAsync();
    private void OnOpenFolderClick(object sender, RoutedEventArgs e) => _ = ViewModel.PickAndOpenFolderAsync();
    private void OnRevealClick(object sender, RoutedEventArgs e) => ViewModel.RevealActive();

    private void OnCloseClipClick(object sender, RoutedEventArgs e)
    {
        if (ViewModel.ActiveSession != null) ViewModel.CloseSession(ViewModel.ActiveSession);
    }

    [DllImport("user32.dll", EntryPoint = "ShowCursor")]
    private static extern int Win32ShowCursor([MarshalAs(UnmanagedType.Bool)] bool show);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(nint hwnd);
}
