namespace Syna.Core.State;

/// <summary>
/// First run under a new data folder: carries settings and history (recents, positions, mixer
/// state, sessions, metadata cache) over from the previous one. Caches are rebuilt on demand.
/// </summary>
public static class LegacyImport
{
    public const string SettingsFile = "settings.json";
    public const string DatabaseFile = "state.db";

    /// <summary>
    /// Copies settings and history from <paramref name="from"/> into <paramref name="to"/>, which
    /// must not exist yet. Returns false when there is nothing to do. The source folder is never
    /// written: a cleanly closed database is copied as a file, and one with a write-ahead log (the
    /// previous version is running, or did not close) is read through SQLite's backup API, whose
    /// snapshot includes the log. On failure nothing is left half-imported.
    /// </summary>
    public static bool Run(string from, string to)
    {
        if (Directory.Exists(to) || !Directory.Exists(from)) return false;
        Directory.CreateDirectory(to);
        try
        {
            string settings = Path.Combine(from, SettingsFile);
            if (File.Exists(settings)) File.Copy(settings, Path.Combine(to, SettingsFile));
            string database = Path.Combine(from, DatabaseFile);
            if (File.Exists(database))
            {
                if (File.Exists(database + "-wal")) AppDatabase.Copy(database, Path.Combine(to, DatabaseFile));
                else File.Copy(database, Path.Combine(to, DatabaseFile));
            }
            return true;
        }
        catch
        {
            try { Directory.Delete(to, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            throw;
        }
    }
}
