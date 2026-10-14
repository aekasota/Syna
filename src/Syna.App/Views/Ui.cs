using Syna.Controls;
using Microsoft.UI.Xaml;

namespace Syna.Views;

/// <summary>Small pure functions for x:Bind function bindings.</summary>
public static class Ui
{
    public static Visibility Show(bool value) => value ? Visibility.Visible : Visibility.Collapsed;
    public static Visibility Hide(bool value) => value ? Visibility.Collapsed : Visibility.Visible;
    public static Visibility ShowText(string? value) => string.IsNullOrEmpty(value) ? Visibility.Collapsed : Visibility.Visible;
    public static bool Not(bool value) => !value;
    public static IconKind PlayIcon(bool playing) => playing ? IconKind.Pause : IconKind.Play;
    public static IconKind VolumeIcon(bool muted, double volume) => muted || volume <= 0 ? IconKind.VolumeMute : IconKind.Volume;
    public static GridLength Width(bool visible, double width) => new(visible ? width : 0);
    public static double Opacity(bool dim) => dim ? 0.45 : 1.0;
}
