using System.ComponentModel;
using System.Diagnostics;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.Windows.Storage.Pickers;
using Windows.System;

namespace Syna.Views;

public sealed partial class SettingsView : UserControl
{
    private static readonly int[] ShortSkips = [500, 1000, 2000, 3000];
    private static readonly int[] LongSkips = [5000, 10000, 15000, 30000];

    private readonly (RadioButton Nav, FrameworkElement Section)[] _sections;
    private bool _loading;
    private bool _systemQueried;
    private RadioButton? _navTarget;

    public SettingsView()
    {
        ViewModel = App.Current!.ViewModel!;
        InitializeComponent();
        _sections =
        [
            (NavPlayback, PlaybackSection), (NavAudio, AudioSection), (NavPerformance, PerformanceSection), (NavFiles, FilesSection),
            (NavInterface, InterfaceSection), (NavSystem, SystemSection), (NavAbout, AboutSection),
        ];
        ViewModel.PropertyChanged += OnViewModelChanged;
    }

    public MainViewModel ViewModel { get; }
    private AppSettings Settings => ViewModel.Settings;

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName != nameof(MainViewModel.SettingsOpen)) return;
        if (ViewModel.SettingsOpen)
        {
            Load();
            Scrim.Visibility = Visibility.Visible;
            Scroller.ChangeView(null, 0, null, disableAnimation: true);
            NavPlayback.IsChecked = true;
            DispatcherQueue.TryEnqueue(() => NavPlayback.Focus(FocusState.Programmatic));
            if (!_systemQueried) _ = LoadSystemInfoAsync();
            _ = UpdateCacheSizeAsync();
        }
        else
        {
            Scrim.Visibility = Visibility.Collapsed;
        }
    }

    private void Load()
    {
        _loading = true;
        try
        {
            AutoplaySwitch.IsOn = Settings.Autoplay;
            RememberSwitch.IsOn = Settings.RememberPosition;
            RestoreSwitch.IsOn = Settings.RestoreSessions;
            ShortSkipText.Text = SkipText(Settings.ShortSkipMs);
            LongSkipText.Text = SkipText(Settings.LongSkipMs);
            Track1Slider.Value = Settings.DefaultGain(0) * 100;
            Track2Slider.Value = Settings.DefaultGain(1) * 100;
            UpdateTrackTexts();
            UpdateOffsetText();
            LimiterSwitch.IsOn = Settings.Limiter;
            HwDecodeSwitch.IsOn = Settings.HardwareDecoding;
            HwEncodeSwitch.IsOn = Settings.HardwareEncoding;
            ThumbsSwitch.IsOn = Settings.Thumbnails;
            WavesSwitch.IsOn = Settings.Waveforms;
            DiagSwitch.IsOn = ViewModel.DiagnosticsVisible;
            UpdateTrimFolder();
            SuffixBox.Text = Settings.TrimSuffix;
            (Settings.TrimMode switch { TrimMode.Quick => ModeQuick, TrimMode.Exact => ModeExact, _ => ModeSmart }).IsChecked = true;
            RebuildWatchList();
            UpdateEditor();
            (Settings.Theme switch { ThemePreference.Dark => ThemeDark, ThemePreference.Light => ThemeLight, _ => ThemeSystem }).IsChecked = true;
            UpdateAssociations();
        }
        finally
        {
            _loading = false;
        }
    }

    private void OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not ToggleSwitch s) return;
        bool on = s.IsOn;
        if (s == AutoplaySwitch) Settings.Autoplay = on;
        else if (s == RememberSwitch) Settings.RememberPosition = on;
        else if (s == RestoreSwitch) Settings.RestoreSessions = on;
        else if (s == LimiterSwitch) { Settings.Limiter = on; ViewModel.ApplyAudioSettings(); }
        else if (s == HwDecodeSwitch) Settings.HardwareDecoding = on;
        else if (s == HwEncodeSwitch) Settings.HardwareEncoding = on;
        else if (s == ThumbsSwitch) Settings.Thumbnails = on;
        else if (s == WavesSwitch) Settings.Waveforms = on;
        else if (s == DiagSwitch) { ViewModel.DiagnosticsVisible = on; return; }
        ViewModel.SaveSettingsSoon();
    }

    private void OnShortSkipClick(object sender, RoutedEventArgs e) =>
        ShowSkipChoices(ShortSkipButton, ShortSkipText, ShortSkips, Settings.ShortSkipMs, ms => Settings.ShortSkipMs = ms);

    private void OnLongSkipClick(object sender, RoutedEventArgs e) =>
        ShowSkipChoices(LongSkipButton, LongSkipText, LongSkips, Settings.LongSkipMs, ms => Settings.LongSkipMs = ms);

    private void ShowSkipChoices(Button anchor, TextBlock label, int[] values, int current, Action<int> apply)
    {
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        foreach (int ms in values)
        {
            var item = new RadioMenuFlyoutItem { Text = SkipText(ms), IsChecked = ms == current, GroupName = anchor.Name };
            item.Click += (_, _) =>
            {
                apply(ms);
                label.Text = SkipText(ms);
                ViewModel.SaveSettingsSoon();
            };
            menu.Items.Add(item);
        }
        menu.ShowAt(anchor);
    }

    private static string SkipText(int ms) => $"{ms / 1000.0:0.#} s";

    private void OnTrackDefaultChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading || Track1Text == null || Track2Text == null) return;
        int index = ReferenceEquals(sender, Track1Slider) ? 0 : 1;
        if (Settings.DefaultTrackGains.Length <= index)
            Settings.DefaultTrackGains = [.. Settings.DefaultTrackGains, .. Enumerable.Repeat(1.0, index + 1 - Settings.DefaultTrackGains.Length)];
        Settings.DefaultTrackGains[index] = e.NewValue / 100;
        UpdateTrackTexts();
        ViewModel.SaveSettingsSoon();
    }

    private void UpdateTrackTexts()
    {
        Track1Text.Text = $"{Math.Round(Track1Slider.Value)}%";
        Track2Text.Text = $"{Math.Round(Track2Slider.Value)}%";
    }

    private void OnOffsetDown(object sender, RoutedEventArgs e) => SetOffset(Settings.AvOffsetMs - 10);
    private void OnOffsetUp(object sender, RoutedEventArgs e) => SetOffset(Settings.AvOffsetMs + 10);

    private void SetOffset(int ms)
    {
        Settings.AvOffsetMs = Math.Clamp(ms, -500, 500);
        UpdateOffsetText();
        ViewModel.ApplyAudioSettings();
        ViewModel.SaveSettingsSoon();
    }

    private void UpdateOffsetText() => OffsetText.Text = $"{Settings.AvOffsetMs:+0;-0;0} ms";

    private async Task LoadSystemInfoAsync()
    {
        _systemQueried = true;
        try
        {
            var info = await Task.Run(() => MediaEngine.QuerySystem());
            string decoders = info.HardwareDecoders.Length > 0 ? string.Join(", ", info.HardwareDecoders) : "none, video decodes on the CPU";
            string encoders = info.HardwareEncoders.Length > 0 ? string.Join(", ", info.HardwareEncoders) : "none, trims re-encode on the CPU";
            GpuText.Text = $"{info.Adapter}\nHardware decoding: {decoders}\nHardware encoding: {encoders}";
            UpdateAbout(info);
        }
        catch (EngineException ex)
        {
            _systemQueried = false;
            GpuText.Text = "Hardware capabilities could not be read. " + ex.Message;
            UpdateAbout(null);
        }
    }

    private async void OnTrimFolderClick(object sender, RoutedEventArgs e)
    {
        if (App.Current?.Window is not IShell shell) return;
        string? folder = await shell.PickFolderAsync();
        if (folder == null) return;
        Settings.TrimFolder = folder;
        UpdateTrimFolder();
        ViewModel.SaveSettingsSoon();
    }

    private void OnTrimFolderReset(object sender, RoutedEventArgs e)
    {
        Settings.TrimFolder = "";
        UpdateTrimFolder();
        ViewModel.SaveSettingsSoon();
    }

    private void UpdateTrimFolder()
    {
        bool custom = Settings.TrimFolder.Length > 0;
        TrimFolderText.Text = (custom ? Settings.TrimFolder : "Next to the source clip") + ". Saving a trim to another folder changes this.";
        TrimFolderReset.Visibility = Ui.Show(custom);
    }

    private void OnSuffixChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        string value = SuffixBox.Text.Trim();
        if (value.Length == 0 || value.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) return;
        Settings.TrimSuffix = value;
        ViewModel.SaveSettingsSoon();
    }

    private void OnModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (ReferenceEquals(sender, ModeQuick)) ViewModel.Trim.IsQuick = true;
        else if (ReferenceEquals(sender, ModeExact)) ViewModel.Trim.IsExact = true;
        else ViewModel.Trim.IsSmart = true;
        ViewModel.SaveSettingsSoon();
    }

    private async void OnAddWatchClick(object sender, RoutedEventArgs e)
    {
        if (App.Current?.Window is not IShell shell) return;
        string? folder = await shell.PickFolderAsync();
        if (folder == null) return;
        ViewModel.Db.AddWatchFolder(folder);
        ViewModel.StartWatching();
        RebuildWatchList();
    }

    private void RebuildWatchList()
    {
        WatchList.Children.Clear();
        var folders = ViewModel.Db.WatchFolders();
        if (folders.Count == 0)
        {
            WatchList.Children.Add(new TextBlock { Text = "No folders.", Style = Resource<Style>("CaptionText") });
            return;
        }
        foreach (string folder in folders)
        {
            var row = new Grid { ColumnSpacing = 8 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var text = new TextBlock { Text = folder, Style = Resource<Style>("SmallText"), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            ToolTipService.SetToolTip(text, folder);
            var remove = new Button { Style = Resource<Style>("IconButton"), Content = new Controls.Icon { Kind = Controls.IconKind.Close, Width = 12, Height = 12 } };
            AutomationProperties.SetName(remove, "Stop watching " + folder);
            ToolTipService.SetToolTip(remove, "Stop watching");
            remove.Click += (_, _) =>
            {
                ViewModel.Db.RemoveWatchFolder(folder);
                ViewModel.StartWatching();
                RebuildWatchList();
            };
            Grid.SetColumn(remove, 1);
            row.Children.Add(text);
            row.Children.Add(remove);
            WatchList.Children.Add(row);
        }
    }

    private async void OnEditorClick(object sender, RoutedEventArgs e)
    {
        if (App.Current?.Window is not { } window) return;
        var picker = new FileOpenPicker(window.AppWindow.Id) { CommitButtonText = "Choose" };
        picker.FileTypeFilter.Add(".exe");
        var result = await picker.PickSingleFileAsync();
        if (result == null) return;
        Settings.ExternalEditor = result.Path;
        UpdateEditor();
        ViewModel.SaveSettingsSoon();
    }

    private void OnEditorClear(object sender, RoutedEventArgs e)
    {
        Settings.ExternalEditor = "";
        UpdateEditor();
        ViewModel.SaveSettingsSoon();
    }

    private void UpdateEditor()
    {
        bool set = Settings.ExternalEditor.Length > 0;
        EditorText.Text = set ? Settings.ExternalEditor : "Not set. Used by Open in external editor.";
        EditorClear.Visibility = Ui.Show(set);
    }

    private void OnThemeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var theme = ReferenceEquals(sender, ThemeDark) ? ThemePreference.Dark : ReferenceEquals(sender, ThemeLight) ? ThemePreference.Light : ThemePreference.System;
        Settings.Theme = theme;
        (App.Current?.Window as IShell)?.ApplyTheme(theme);
        ViewModel.SaveSettingsSoon();
    }

    private void UpdateAssociations()
    {
        bool registered = ShellIntegration.AreAssociationsRegistered();
        AssocText.Text = registered
            ? $"Listed in Open with for {string.Join(", ", MediaFiles.Extensions)}."
            : "Not registered. Registering adds this app to Open with; your default app does not change.";
        AssocButton.Content = registered ? "Remove" : "Register";
    }

    private void OnAssocClick(object sender, RoutedEventArgs e)
    {
        try
        {
            if (ShellIntegration.AreAssociationsRegistered()) ShellIntegration.UnregisterAssociations();
            else ShellIntegration.RegisterAssociations();
        }
        catch (Exception ex)
        {
            Log.Warn("settings", "file type registration failed: " + ex.Message);
            ViewModel.Toast("File type registration could not be changed.");
        }
        UpdateAssociations();
    }

    private async void OnDefaultAppsClick(object sender, RoutedEventArgs e) => await ShellIntegration.OpenDefaultAppsSettingsAsync();

    private void OnLogsClick(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(AppPaths.Logs);
        Process.Start(new ProcessStartInfo(AppPaths.Logs) { UseShellExecute = true });
    }

    private async Task UpdateCacheSizeAsync()
    {
        long bytes = await Task.Run(() => DirectorySize(AppPaths.Cache));
        CacheText.Text = $"Thumbnails, waveforms and metadata use {FormatSize(bytes)}. Clearing never touches your clips.";
    }

    private async void OnClearCacheClick(object sender, RoutedEventArgs e)
    {
        int kept = await Task.Run(() =>
        {
            int inUse = 0;
            if (!Directory.Exists(AppPaths.Cache)) return 0;
            foreach (string file in Directory.EnumerateFiles(AppPaths.Cache, "*", SearchOption.AllDirectories))
            {
                try { File.Delete(file); }
                catch (IOException) { inUse++; }
                catch (UnauthorizedAccessException) { inUse++; }
            }
            return inUse;
        });
        await UpdateCacheSizeAsync();
        ViewModel.Toast(kept == 0 ? "Cache cleared." : $"Cache cleared. {kept} files in use were kept.");
    }

    private static long DirectorySize(string directory)
    {
        if (!Directory.Exists(directory)) return 0;
        long total = 0;
        foreach (var file in new DirectoryInfo(directory).EnumerateFiles("*", SearchOption.AllDirectories))
        {
            try { total += file.Length; }
            catch (IOException) { }
        }
        return total;
    }

    private static string FormatSize(long bytes) => bytes switch
    {
        < 1024 * 1024 => $"{bytes / 1024.0:0} KB",
        < 1024L * 1024 * 1024 => $"{bytes / (1024.0 * 1024):0.0} MB",
        _ => $"{bytes / (1024.0 * 1024 * 1024):0.00} GB",
    };

    private void UpdateAbout(SystemInfo? info)
    {
        var version = typeof(App).Assembly.GetName().Version;
        string ffmpeg = info == null ? "FFmpeg (version unavailable)" : $"FFmpeg {info.FfmpegVersion}, {info.FfmpegLicense}";
        AboutText.Text =
            $"Syna {version?.ToString(3)}\n" +
            $"Media engine API {MediaEngine.ApiVersion}\n" +
            $"{ffmpeg}, dynamically linked from the app folder\n" +
            $".NET {Environment.Version}\n" +
            $"Data and cache: {AppPaths.Root}";
    }

    private void OnNavClick(object sender, RoutedEventArgs e)
    {
        foreach (var (nav, section) in _sections)
        {
            if (!ReferenceEquals(nav, sender)) continue;
            _navTarget = nav;
            section.StartBringIntoView(new BringIntoViewOptions { VerticalAlignmentRatio = 0, AnimationDesired = true });
            return;
        }
    }

    /// <summary>Highlights the section at the top of the viewport while the user scrolls.</summary>
    private void OnScrollerViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
    {
        if (_navTarget != null)
        {
            if (!e.IsIntermediate) _navTarget = null;
            return;
        }
        if (Scroller.Content is not UIElement content) return;
        double offset = Scroller.VerticalOffset;
        RadioButton active = _sections[0].Nav;
        if (offset >= Scroller.ScrollableHeight - 1 && offset > 0) active = _sections[^1].Nav;
        else
        {
            foreach (var (nav, section) in _sections)
                if (section.TransformToVisual(content).TransformPoint(default).Y <= offset + 48) active = nav;
        }
        active.IsChecked = true;
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => ViewModel.SettingsOpen = false;

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Escape) return;
        ViewModel.SettingsOpen = false;
        e.Handled = true;
    }

    private void OnScrimSizeChanged(object sender, SizeChangedEventArgs e)
    {
        Panel.MaxWidth = Math.Max(0, e.NewSize.Width - 48);
        Panel.MaxHeight = Math.Max(0, e.NewSize.Height - 48);
    }

    private static T Resource<T>(string key) => (T)Application.Current.Resources[key];
}
