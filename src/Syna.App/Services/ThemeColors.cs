using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Syna.Services;

/// <summary>
/// Theme-aware token lookup for code that draws pixels (waveforms, meters) or creates brushes, where
/// {ThemeResource} cannot follow a runtime theme switch.
/// </summary>
public static class ThemeColors
{
    public static ElementTheme Theme { get; set; } = ElementTheme.Dark;

    public static Color Get(string key)
    {
        string themeKey = Theme == ElementTheme.Light ? "Light" : "Dark";
        if (Find(Application.Current.Resources, themeKey, key) is Color c) return c;
        return Microsoft.UI.Colors.Magenta;
    }

    public static Color Track(int index) => Get($"Track{index % 4 + 1}Color");

    /// <summary>A brush for a token such as "Surface2Brush", resolved for <see cref="Theme"/>.</summary>
    public static SolidColorBrush Brush(string brushKey) => new(Get(brushKey.Replace("Brush", "Color")));

    private static Color? Find(ResourceDictionary dict, string theme, string key)
    {
        if (dict.ThemeDictionaries.TryGetValue(theme, out object? td) && td is ResourceDictionary themed && themed.TryGetValue(key, out object? v) && v is Color color)
            return color;
        foreach (var merged in dict.MergedDictionaries)
            if (Find(merged, theme, key) is Color c) return c;
        return null;
    }
}
