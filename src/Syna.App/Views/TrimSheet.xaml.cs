using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace Syna.Views;

public sealed partial class TrimSheet : UserControl
{
    public TrimSheet()
    {
        Trim = App.Current!.ViewModel!.Trim;
        InitializeComponent();
        Trim.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(TrimViewModel.IsOpen) && Trim.IsOpen)
                DispatcherQueue.TryEnqueue(() => SaveButton.Focus(FocusState.Programmatic));
        };
    }

    public TrimViewModel Trim { get; }

    private void OnSaveClick(object sender, RoutedEventArgs e) => _ = Trim.RunAsync();
    private void OnCancelRunClick(object sender, RoutedEventArgs e) => Trim.Cancel();
    private void OnBackClick(object sender, RoutedEventArgs e) => Trim.Reset();
    private void OnUseClipFolderClick(object sender, RoutedEventArgs e) => Trim.UseClipFolder();

    private void OnCloseClick(object sender, RoutedEventArgs e)
    {
        if (!Trim.IsRunning) Trim.IsOpen = false;
    }

    private async void OnChangeFolderClick(object sender, RoutedEventArgs e)
    {
        if (App.Current?.Window is not IShell shell) return;
        string? folder = await shell.PickFolderAsync();
        if (folder != null) Trim.ChooseFolder(folder);
    }

    private void OnCodecClick(object sender, RoutedEventArgs e) => ShowChoices(sender, Trim.CodecChoices, Trim.CustomCodec, v => Trim.CustomCodec = v);
    private void OnSpeedClick(object sender, RoutedEventArgs e) => ShowChoices(sender, TrimViewModel.SpeedChoices, Trim.CustomSpeed, v => Trim.CustomSpeed = v);
    private void OnHeightClick(object sender, RoutedEventArgs e) => ShowChoices(sender, Trim.HeightChoices, Trim.CustomHeight, v => Trim.CustomHeight = v);
    private void OnFpsClick(object sender, RoutedEventArgs e) => ShowChoices(sender, Trim.FpsChoices, Trim.CustomFps, v => Trim.CustomFps = v);
    private void OnAudioCodecClick(object sender, RoutedEventArgs e) => ShowChoices(sender, Trim.AudioCodecChoices, Trim.CustomAudioCodec, v => Trim.CustomAudioCodec = v);
    private void OnAudioBitrateClick(object sender, RoutedEventArgs e) => ShowChoices(sender, TrimViewModel.AudioBitrateChoices, Trim.CustomAudioBitrate, v => Trim.CustomAudioBitrate = v);
    private void OnSampleRateClick(object sender, RoutedEventArgs e) => ShowChoices(sender, TrimViewModel.SampleRateChoices, Trim.CustomSampleRate, v => Trim.CustomSampleRate = v);
    private void OnChannelsClick(object sender, RoutedEventArgs e) => ShowChoices(sender, TrimViewModel.ChannelChoices, Trim.CustomChannels, v => Trim.CustomChannels = v);

    private static void ShowChoices<T>(object anchor, IReadOnlyList<Choice<T>> choices, T current, Action<T> select)
    {
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedLeft };
        foreach (var choice in choices)
        {
            var item = new RadioMenuFlyoutItem
            {
                Text = choice.Label, GroupName = "choice", IsEnabled = choice.Enabled,
                IsChecked = EqualityComparer<T>.Default.Equals(choice.Value, current),
            };
            item.Click += (_, _) => select(choice.Value);
            menu.Items.Add(item);
        }
        menu.ShowAt((FrameworkElement)anchor);
    }

    private void OnRevealClick(object sender, RoutedEventArgs e)
    {
        if (Trim.Result is { } r) FileOperations.Reveal(r.Output);
    }

    private void OnOpenResultClick(object sender, RoutedEventArgs e)
    {
        if (Trim.Result is not { } r) return;
        Trim.IsOpen = false;
        App.Current?.ViewModel?.OpenPaths([r.Output]);
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape && !Trim.IsRunning)
        {
            Trim.IsOpen = false;
            e.Handled = true;
        }
        else if (e.Key == VirtualKey.Enter && Trim.IsEditing && FocusManager.GetFocusedElement(XamlRoot) is not TextBox)
        {
            _ = Trim.RunAsync();
            e.Handled = true;
        }
    }
}
