using System.Text.Json;
using System.Text.Json.Serialization;
using Syna.Core.Engine;

namespace Syna.Core.State;

public enum ThemePreference { System, Dark, Light }

public sealed class AppSettings
{
    // Playback
    public bool RememberPosition { get; set; } = true;
    public bool Autoplay { get; set; } = true;
    public int ShortSkipMs { get; set; } = 1000;
    public int LongSkipMs { get; set; } = 5000;
    public bool RestoreSessions { get; set; } = true;

    // Audio
    public double[] DefaultTrackGains { get; set; } = [1, 1, 1, 1, 1, 1, 1, 1];
    /// <summary>A track's volume in a clip without a saved mixer, and what Reset volume returns it to.</summary>
    public double DefaultGain(int track) => DefaultTrackGains.ElementAtOrDefault(track) is double g && g > 0 ? g : 1.0;
    /// <summary>The listening volume: applied to playback only, never to a trim.</summary>
    public double MasterGain { get; set; } = 1.0;
    public int AvOffsetMs { get; set; }
    public bool Limiter { get; set; } = true;

    // Performance
    public bool HardwareDecoding { get; set; } = true;
    public bool HardwareEncoding { get; set; } = true;
    public bool Thumbnails { get; set; } = true;
    public bool Waveforms { get; set; } = true;

    // Files
    /// <summary>Where trims are saved: the folder last confirmed in the trim sheet; empty = next to the source clip.</summary>
    public string TrimFolder { get; set; } = "";
    public string TrimSuffix { get; set; } = "_trimmed";
    public TrimMode TrimMode { get; set; } = TrimMode.Smart;
    public TrimProfile TrimProfile { get; set; } = TrimProfile.Original;
    public TrimAudio TrimAudio { get; set; } = TrimAudio.Mix;
    public TrimCustomSettings TrimCustom { get; set; } = new();
    public string ExternalEditor { get; set; } = "";
    public string LastFolder { get; set; } = "";

    // Interface
    public ThemePreference Theme { get; set; } = ThemePreference.System;
    public bool ExplorerVisible { get; set; } = true;
    public bool RailVisible { get; set; } = true;
    public bool AudioPanelVisible { get; set; } = true;
    public bool ShowDiagnostics { get; set; }
    public int[] WindowBounds { get; set; } = [];
    public bool WindowMaximized { get; set; }
}

[JsonSourceGenerationOptions(WriteIndented = true, UseStringEnumConverter = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(AppSettings))]
internal sealed partial class SettingsJson : JsonSerializerContext;

/// <summary>Loads and atomically saves settings.json; unreadable files fall back to defaults.</summary>
public sealed class SettingsStore(string path)
{
    public AppSettings Load()
    {
        try
        {
            if (File.Exists(path))
                return JsonSerializer.Deserialize(File.ReadAllText(path), SettingsJson.Default.AppSettings) ?? new AppSettings();
        }
        catch (JsonException)
        {
        }
        catch (IOException)
        {
        }
        return new AppSettings();
    }

    public void Save(AppSettings settings)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temp = path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(settings, SettingsJson.Default.AppSettings));
        File.Move(temp, path, overwrite: true);
    }
}
