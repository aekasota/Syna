using Microsoft.Data.Sqlite;
using Syna.Core.State;

namespace Syna.Services;

// %LOCALAPPDATA%\Syna\{Logs,Cache\{thumbnails,waveforms},state.db,settings.json}. SYNA_DATA_DIR
// names another folder instead: a separate profile for tests and screenshots.
public static class AppPaths
{
    private static readonly string LocalAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
    private static readonly string? DataDirOverride =
        Environment.GetEnvironmentVariable("SYNA_DATA_DIR") is { Length: > 0 } dir ? Path.GetFullPath(dir) : null;

    public static string Root { get; } = DataDirOverride ?? Path.Combine(LocalAppData, "Syna");
    public static string Logs => Path.Combine(Root, "Logs");
    public static string Cache => Path.Combine(Root, "Cache");
    public static string Thumbnails => Path.Combine(Cache, "thumbnails");
    public static string Waveforms => Path.Combine(Cache, "waveforms");
    public static string Database => Path.Combine(Root, LegacyImport.DatabaseFile);
    public static string Settings => Path.Combine(Root, LegacyImport.SettingsFile);

    // Data folder of the app's previous name (Hyak Clip Player).
    private static string LegacyRoot => Path.Combine(LocalAppData, "HyakClipPlayer");

    /// <summary>
    /// Runs <see cref="LegacyImport"/> once, into the default data folder only; returns a line for the
    /// log, or null when there was nothing to do.
    /// </summary>
    public static string? ImportLegacyData()
    {
        if (DataDirOverride != null) return null;
        try
        {
            return LegacyImport.Run(LegacyRoot, Root) ? $"imported settings and history from {LegacyRoot}" : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or SqliteException)
        {
            return $"could not import data from {LegacyRoot}: {ex.Message}";
        }
    }
}
