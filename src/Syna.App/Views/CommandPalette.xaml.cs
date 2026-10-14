using System.ComponentModel;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace Syna.Views;

public sealed partial class CommandPalette : UserControl
{
    public CommandPalette()
    {
        ViewModel = App.Current!.ViewModel!;
        InitializeComponent();
        ViewModel.PropertyChanged += OnViewModelChanged;
    }

    public MainViewModel ViewModel { get; }

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName != nameof(MainViewModel.PaletteOpen)) return;
        if (ViewModel.PaletteOpen)
        {
            Scrim.Visibility = Visibility.Visible;
            Query.Text = "";
            Refresh();
            Query.Focus(FocusState.Programmatic);
        }
        else
        {
            Scrim.Visibility = Visibility.Collapsed;
        }
    }

    /// <summary>Substring matches rank first (earlier is better), then in-order character matches.</summary>
    private static int Score(string title, string query)
    {
        if (query.Length == 0) return 1;
        int at = title.IndexOf(query, StringComparison.OrdinalIgnoreCase);
        if (at >= 0) return 1000 - at * 10 - (title.Length - query.Length);
        int ti = 0, gaps = 0;
        foreach (char q in query)
        {
            int next = title.IndexOf(q.ToString(), ti, StringComparison.OrdinalIgnoreCase);
            if (next < 0) return -1;
            gaps += next - ti;
            ti = next + 1;
        }
        return 500 - gaps;
    }

    private void Refresh()
    {
        string q = Query.Text.Trim();
        var results = ViewModel.Commands
            .Where(c => c.IsEnabled)
            .Select(c => (Command: c, Score: Math.Max(Score(c.Title, q), Score(c.Group + " " + c.Title, q) - 50)))
            .Where(x => x.Score >= 0)
            .OrderByDescending(x => q.Length == 0 ? 0 : x.Score)
            .Select(x => new PaletteItem(x.Command))
            .ToList();
        Results.ItemsSource = results;
        NoResults.Visibility = results.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (results.Count > 0) Results.SelectedIndex = 0;
    }

    private void OnQueryChanged(object sender, TextChangedEventArgs e) => Refresh();

    private void OnQueryKeyDown(object sender, KeyRoutedEventArgs e)
    {
        int count = (Results.ItemsSource as IList<PaletteItem>)?.Count ?? 0;
        switch (e.Key)
        {
            case VirtualKey.Down when count > 0:
                Results.SelectedIndex = (Results.SelectedIndex + 1) % count;
                Results.ScrollIntoView(Results.SelectedItem);
                e.Handled = true;
                break;
            case VirtualKey.Up when count > 0:
                Results.SelectedIndex = (Results.SelectedIndex - 1 + count) % count;
                Results.ScrollIntoView(Results.SelectedItem);
                e.Handled = true;
                break;
            case VirtualKey.Enter:
                if (Results.SelectedItem is PaletteItem item) Run(item);
                e.Handled = true;
                break;
            case VirtualKey.Escape:
                ViewModel.PaletteOpen = false;
                e.Handled = true;
                break;
        }
    }

    private void OnItemClick(object sender, ItemClickEventArgs e)
    {
        if (e.ClickedItem is PaletteItem item) Run(item);
    }

    private void Run(PaletteItem item)
    {
        ViewModel.PaletteOpen = false;
        DispatcherQueue.TryEnqueue(() =>
        {
            try
            {
                item.Command.Execute();
            }
            catch (Exception ex)
            {
                Log.Error("palette", $"{item.Title} failed: {ex}");
                ViewModel.Toast($"{item.Title} failed. Details are in the log.");
            }
        });
    }

    private void OnScrimPressed(object sender, PointerRoutedEventArgs e) => ViewModel.PaletteOpen = false;
    private void OnPanelPressed(object sender, PointerRoutedEventArgs e) => e.Handled = true;
}

public sealed class PaletteItem(AppCommand command)
{
    public AppCommand Command { get; } = command;
    public string Title => Command.Title;
    public string Group => Command.Group;
    public string Shortcut => Command.Shortcut;
    public Controls.IconKind Icon => Command.Icon;
    public bool HasShortcut => Command.Shortcut.Length > 0;
    public override string ToString() => Command.Title;
}
