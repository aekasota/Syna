using System.Collections.Specialized;
using System.ComponentModel;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace Syna.Views;

public sealed partial class SessionRail : UserControl
{
    private bool _syncing;

    public SessionRail()
    {
        ViewModel = App.Current!.ViewModel!;
        InitializeComponent();
        ViewModel.Sessions.CollectionChanged += OnSessionsChanged;
        ViewModel.PropertyChanged += OnViewModelChanged;
        OnSessionsChanged(null, null);
    }

    public MainViewModel ViewModel { get; }

    private void OnSessionsChanged(object? sender, NotifyCollectionChangedEventArgs? e)
    {
        int n = ViewModel.Sessions.Count;
        CountText.Text = n > 0 ? n.ToString() : "";
        EmptyHint.Visibility = n == 0 ? Visibility.Visible : Visibility.Collapsed;
        CloseAllButton.Visibility = n > 0 ? Visibility.Visible : Visibility.Collapsed;
        SyncSelection();
    }

    private void OnCloseAllClick(object sender, RoutedEventArgs e) => _ = ViewModel.CloseAllSessionsAsync();

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.ActiveSession)) SyncSelection();
    }

    private void SyncSelection()
    {
        _syncing = true;
        List.SelectedItem = ViewModel.ActiveSession;
        if (ViewModel.ActiveSession != null) List.ScrollIntoView(ViewModel.ActiveSession);
        _syncing = false;
    }

    private void OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing) return;
        if (List.SelectedItem is SessionViewModel s && s != ViewModel.ActiveSession) ViewModel.Activate(s);
    }

    private static Button? CloseButtonOf(object sender) =>
        sender is Grid g && g.Children.Count > 0 && g.Children[0] is Grid thumb ? thumb.Children.OfType<Button>().FirstOrDefault() : null;

    private void OnItemPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if (CloseButtonOf(sender) is { } b) b.Opacity = 1;
    }

    private void OnItemPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (CloseButtonOf(sender) is { } b) b.Opacity = 0;
    }

    private void OnItemPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (e.GetCurrentPoint((UIElement)sender).Properties.IsMiddleButtonPressed && sender is FrameworkElement { DataContext: SessionViewModel s })
        {
            ViewModel.CloseSession(s);
            e.Handled = true;
        }
    }

    private void OnCloseClick(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: SessionViewModel s }) ViewModel.CloseSession(s);
    }

    private void OnItemContextRequested(UIElement sender, ContextRequestedEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: SessionViewModel s }) return;
        var menu = new MenuFlyout();
        void Add(string text, Action action, string accel = "", bool enabled = true)
        {
            var item = new MenuFlyoutItem { Text = text, KeyboardAcceleratorTextOverride = accel, IsEnabled = enabled };
            item.Click += (_, _) => action();
            menu.Items.Add(item);
        }
        Add("Play", () => { ViewModel.Activate(s); ViewModel.Play(); }, enabled: !s.IsMissing);
        Add("Trim…", () => { ViewModel.Activate(s); ViewModel.OpenTrim(); }, enabled: !s.IsMissing);
        menu.Items.Add(new MenuFlyoutSeparator());
        Add("Reveal in Explorer", () => FileOperations.Reveal(s.Path));
        Add("Copy path", () => FileOperations.CopyText(s.Path));
        Add(s.IsFavorite ? "Remove star" : "Star", () => ViewModel.ToggleFavorite(s.Path), "Ctrl+D");
        Add("Open in external editor", () => ViewModel.OpenInExternalEditor(s.Path), enabled: !s.IsMissing);
        menu.Items.Add(new MenuFlyoutSeparator());
        int index = ViewModel.Sessions.IndexOf(s);
        Add("Move up", () => ViewModel.MoveSession(s, -1), enabled: index > 0);
        Add("Move down", () => ViewModel.MoveSession(s, 1), enabled: index < ViewModel.Sessions.Count - 1);
        Add("Close", () => ViewModel.CloseSession(s), "Ctrl+W");
        Add("Close others", () => ViewModel.CloseOthers(s), enabled: ViewModel.Sessions.Count > 1);
        Add("Close all", () => _ = ViewModel.CloseAllSessionsAsync(), "Ctrl+Shift+W");
        if (e.TryGetPosition(sender, out var point)) menu.ShowAt(sender, point);
        else menu.ShowAt((FrameworkElement)sender);
        e.Handled = true;
    }
}
