using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;

namespace Syna.Views;

public sealed partial class ExplorerPanel : UserControl
{
    private bool _settingMode;

    public ExplorerPanel()
    {
        ViewModel = App.Current!.ViewModel!;
        InitializeComponent();
        ViewModel.Explorer.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ExplorerViewModel.Mode)) SyncMode();
        };
        SyncMode();
        PreviewKeyDown += OnPreviewKeyDown;
    }

    public MainViewModel ViewModel { get; }

    private void SyncMode()
    {
        _settingMode = true;
        (ViewModel.Explorer.Mode switch
        {
            ExplorerMode.Recent => RecentMode,
            ExplorerMode.Starred => StarredMode,
            _ => FolderMode,
        }).IsChecked = true;
        _settingMode = false;
    }

    private void OnModeChecked(object sender, RoutedEventArgs e)
    {
        if (_settingMode) return;
        if (ReferenceEquals(sender, RecentMode)) _ = ViewModel.Explorer.ShowListAsync(ExplorerMode.Recent);
        else if (ReferenceEquals(sender, StarredMode)) _ = ViewModel.Explorer.ShowListAsync(ExplorerMode.Starred);
        else _ = ViewModel.Explorer.NavigateAsync(ViewModel.Explorer.CurrentFolder, record: false);
    }

    private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        bool alt = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        if (!alt) return;
        switch (e.Key)
        {
            case VirtualKey.Left: ViewModel.Explorer.GoBack(); e.Handled = true; break;
            case VirtualKey.Right: ViewModel.Explorer.GoForward(); e.Handled = true; break;
            case VirtualKey.Up: ViewModel.Explorer.GoUp(); e.Handled = true; break;
        }
    }

    private void OnBackClick(object sender, RoutedEventArgs e) => ViewModel.Explorer.GoBack();
    private void OnForwardClick(object sender, RoutedEventArgs e) => ViewModel.Explorer.GoForward();
    private void OnUpClick(object sender, RoutedEventArgs e) => ViewModel.Explorer.GoUp();
    private void OnOpenFolderClick(object sender, RoutedEventArgs e) => _ = ViewModel.PickAndOpenFolderAsync();

    private IReadOnlyList<ExplorerItem> Selection() => Items.SelectedItems.OfType<ExplorerItem>().ToList();

    private void Open(IReadOnlyList<ExplorerItem> items)
    {
        if (items.Count == 1 && items[0].IsFolder)
        {
            _ = ViewModel.Explorer.NavigateAsync(items[0].Path);
            return;
        }
        var media = items.Where(i => i.IsMedia).Select(i => i.Path).ToList();
        foreach (var i in items.Where(i => i.IsNew)) i.IsNew = false;
        if (media.Count > 0) ViewModel.OpenPaths(media);
    }

    private void OnItemsDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if ((e.OriginalSource as FrameworkElement)?.DataContext is ExplorerItem item)
        {
            Open([item]);
            e.Handled = true;
        }
    }

    private void OnItemsKeyDown(object sender, KeyRoutedEventArgs e)
    {
        var selection = Selection();
        switch (e.Key)
        {
            case VirtualKey.Enter when selection.Count > 0:
                Open(selection);
                e.Handled = true;
                break;
            case VirtualKey.Delete when selection.Count == 1 && selection[0].IsMedia:
                _ = ViewModel.RecycleAsync(selection[0].Path);
                e.Handled = true;
                break;
            case VirtualKey.F2 when selection.Count == 1 && selection[0].IsMedia:
                _ = ViewModel.RenameAsync(selection[0].Path);
                e.Handled = true;
                break;
            case VirtualKey.Back:
                ViewModel.Explorer.GoUp();
                e.Handled = true;
                break;
        }
    }

    private void OnDragItemsStarting(object sender, DragItemsStartingEventArgs e)
    {
        var paths = e.Items.OfType<ExplorerItem>().Where(i => i.IsMedia).Select(i => i.Path).ToList();
        if (paths.Count == 0)
        {
            e.Cancel = true;
            return;
        }
        e.Data.SetData(ExplorerDrag.Format, string.Join('\n', paths));
        e.Data.RequestedOperation = DataPackageOperation.Copy;
    }

    private void OnItemContextRequested(UIElement sender, ContextRequestedEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ExplorerItem item }) return;
        if (!Items.SelectedItems.Contains(item))
        {
            Items.SelectedItems.Clear();
            Items.SelectedItems.Add(item);
        }
        var selection = Selection();
        var menu = new MenuFlyout();
        void Add(string text, Action action, string accel = "", bool enabled = true)
        {
            var mi = new MenuFlyoutItem { Text = text, KeyboardAcceleratorTextOverride = accel, IsEnabled = enabled };
            mi.Click += (_, _) => action();
            menu.Items.Add(mi);
        }
        if (item.IsFolder)
        {
            Add("Open", () => _ = ViewModel.Explorer.NavigateAsync(item.Path), "Enter");
            Add("Reveal in Explorer", () => FileOperations.Reveal(item.Path));
            Add("Copy path", () => FileOperations.CopyText(item.Path));
            bool watched = ViewModel.Db.WatchFolders().Any(w => string.Equals(w, item.Path, StringComparison.OrdinalIgnoreCase));
            Add(watched ? "Stop watching for new clips" : "Watch for new clips", () =>
            {
                if (watched) ViewModel.Db.RemoveWatchFolder(item.Path);
                else ViewModel.Db.AddWatchFolder(item.Path);
                ViewModel.StartWatching();
            });
        }
        else
        {
            Add(selection.Count > 1 ? $"Open {selection.Count} clips" : "Open", () => Open(selection), "Enter");
            Add("Play", () => { Open([item]); ViewModel.Play(); });
            Add("Trim…", () => { Open([item]); ViewModel.OpenTrim(); });
            menu.Items.Add(new MenuFlyoutSeparator());
            Add("Reveal in Explorer", () => FileOperations.Reveal(item.Path));
            Add("Copy path", () => FileOperations.CopyText(item.Path));
            Add(item.IsFavorite ? "Remove star" : "Star", () => ViewModel.ToggleFavorite(item.Path));
            Add("Open in external editor", () => ViewModel.OpenInExternalEditor(item.Path));
            menu.Items.Add(new MenuFlyoutSeparator());
            Add("Rename…", () => _ = ViewModel.RenameAsync(item.Path), "F2", selection.Count == 1);
            Add("Move to Recycle Bin", () => _ = ViewModel.RecycleAsync(item.Path), "Delete", selection.Count == 1);
        }
        if (e.TryGetPosition(sender, out var point)) menu.ShowAt(sender, point);
        else menu.ShowAt((FrameworkElement)sender);
        e.Handled = true;
    }
}
