using Microsoft.Data.Sqlite;

namespace Syna.Core.State;

public sealed record FileRecord(string Path, long LastOpenedUnixMs, long LastPositionUs, bool Favorite, string? MixerJson, string? LabelsJson);

public sealed record MediaSummary(int Width, int Height, double Fps, long DurationUs, string VideoCodec, int AudioTracks);

/// <summary>
/// Small relational state: recents, favorites, playback positions, per-file mixer state, sessions,
/// watch folders and the explorer metadata cache. Large artifacts (thumbnails, waveforms) stay on disk.
/// </summary>
public sealed class AppDatabase : IDisposable
{
    private const int SchemaVersion = 1;
    private readonly SqliteConnection _db;
    private readonly Lock _gate = new();

    public AppDatabase(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        _db = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = path, Pooling = false }.ToString());
        _db.Open();
        Execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
        Migrate();
    }

    private void Migrate()
    {
        long version = Scalar<long>("PRAGMA user_version");
        if (version < 1)
        {
            Execute("""
                CREATE TABLE IF NOT EXISTS files (
                    path TEXT PRIMARY KEY COLLATE NOCASE,
                    last_opened INTEGER NOT NULL DEFAULT 0,
                    last_position_us INTEGER NOT NULL DEFAULT 0,
                    favorite INTEGER NOT NULL DEFAULT 0,
                    mixer TEXT,
                    labels TEXT
                );
                CREATE INDEX IF NOT EXISTS files_last_opened ON files(last_opened DESC);
                CREATE TABLE IF NOT EXISTS watch_folders (path TEXT PRIMARY KEY COLLATE NOCASE, added INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS sessions (ordinal INTEGER PRIMARY KEY, path TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS media_index (
                    path TEXT PRIMARY KEY COLLATE NOCASE,
                    size INTEGER NOT NULL, mtime INTEGER NOT NULL,
                    width INTEGER, height INTEGER, fps REAL, duration_us INTEGER, codec TEXT, audio_tracks INTEGER
                );
                """);
        }
        if (version < SchemaVersion) Execute($"PRAGMA user_version = {SchemaVersion}");
    }

    public FileRecord? GetFile(string path)
    {
        lock (_gate)
        {
            using var cmd = Command("SELECT path, last_opened, last_position_us, favorite, mixer, labels FROM files WHERE path = $p", ("$p", path));
            using var r = cmd.ExecuteReader();
            return r.Read() ? ReadFile(r) : null;
        }
    }

    public void MarkOpened(string path)
    {
        Execute("""
            INSERT INTO files(path, last_opened) VALUES($p, $t)
            ON CONFLICT(path) DO UPDATE SET last_opened = excluded.last_opened
            """, ("$p", path), ("$t", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));
    }

    public void SavePosition(string path, long positionUs)
    {
        Execute("""
            INSERT INTO files(path, last_position_us) VALUES($p, $v)
            ON CONFLICT(path) DO UPDATE SET last_position_us = excluded.last_position_us
            """, ("$p", path), ("$v", positionUs));
    }

    public void SetFavorite(string path, bool favorite)
    {
        Execute("""
            INSERT INTO files(path, favorite) VALUES($p, $v)
            ON CONFLICT(path) DO UPDATE SET favorite = excluded.favorite
            """, ("$p", path), ("$v", favorite ? 1 : 0));
    }

    public void SaveMixer(string path, string? mixerJson, string? labelsJson)
    {
        Execute("""
            INSERT INTO files(path, mixer, labels) VALUES($p, $m, $l)
            ON CONFLICT(path) DO UPDATE SET mixer = excluded.mixer, labels = excluded.labels
            """, ("$p", path), ("$m", (object?)mixerJson ?? DBNull.Value), ("$l", (object?)labelsJson ?? DBNull.Value));
    }

    public void RenamePath(string oldPath, string newPath)
    {
        Execute("UPDATE OR REPLACE files SET path = $n WHERE path = $o", ("$o", oldPath), ("$n", newPath));
        Execute("UPDATE OR REPLACE media_index SET path = $n WHERE path = $o", ("$o", oldPath), ("$n", newPath));
        Execute("UPDATE sessions SET path = $n WHERE path = $o", ("$o", oldPath), ("$n", newPath));
    }

    public void Forget(string path)
    {
        Execute("DELETE FROM files WHERE path = $p", ("$p", path));
        Execute("DELETE FROM media_index WHERE path = $p", ("$p", path));
    }

    public IReadOnlyList<FileRecord> RecentFiles(int limit) =>
        QueryFiles("SELECT path, last_opened, last_position_us, favorite, mixer, labels FROM files WHERE last_opened > 0 ORDER BY last_opened DESC LIMIT $n", limit);

    public IReadOnlyList<FileRecord> Favorites() =>
        QueryFiles("SELECT path, last_opened, last_position_us, favorite, mixer, labels FROM files WHERE favorite = 1 ORDER BY last_opened DESC LIMIT $n", 10000);

    public IReadOnlyList<string> WatchFolders() => QueryStrings("SELECT path FROM watch_folders ORDER BY added LIMIT $n", 1000);

    public void AddWatchFolder(string path) =>
        Execute("INSERT OR IGNORE INTO watch_folders(path, added) VALUES($p, $t)", ("$p", path), ("$t", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));

    public void RemoveWatchFolder(string path) => Execute("DELETE FROM watch_folders WHERE path = $p", ("$p", path));

    public void SaveSessions(IReadOnlyList<string> paths, int activeIndex)
    {
        lock (_gate)
        {
            using var tx = _db.BeginTransaction();
            ExecuteLocked("DELETE FROM sessions");
            for (int i = 0; i < paths.Count; i++)
                ExecuteLocked("INSERT INTO sessions(ordinal, path, active) VALUES($o, $p, $a)", ("$o", i), ("$p", paths[i]), ("$a", i == activeIndex ? 1 : 0));
            tx.Commit();
        }
    }

    public (IReadOnlyList<string> Paths, int ActiveIndex) LoadSessions()
    {
        lock (_gate)
        {
            using var cmd = Command("SELECT path, active FROM sessions ORDER BY ordinal");
            using var r = cmd.ExecuteReader();
            var paths = new List<string>();
            int active = -1;
            while (r.Read())
            {
                if (r.GetInt64(1) == 1) active = paths.Count;
                paths.Add(r.GetString(0));
            }
            return (paths, active);
        }
    }

    public MediaSummary? GetSummary(string path, long size, long mtime)
    {
        lock (_gate)
        {
            using var cmd = Command("SELECT width, height, fps, duration_us, codec, audio_tracks FROM media_index WHERE path = $p AND size = $s AND mtime = $m",
                ("$p", path), ("$s", size), ("$m", mtime));
            using var r = cmd.ExecuteReader();
            if (!r.Read()) return null;
            return new MediaSummary(r.GetInt32(0), r.GetInt32(1), r.GetDouble(2), r.GetInt64(3), r.IsDBNull(4) ? "" : r.GetString(4), r.GetInt32(5));
        }
    }

    public void PutSummary(string path, long size, long mtime, MediaSummary s) =>
        Execute("""
            INSERT INTO media_index(path, size, mtime, width, height, fps, duration_us, codec, audio_tracks)
            VALUES($p, $s, $m, $w, $h, $f, $d, $c, $a)
            ON CONFLICT(path) DO UPDATE SET size = excluded.size, mtime = excluded.mtime, width = excluded.width, height = excluded.height,
                fps = excluded.fps, duration_us = excluded.duration_us, codec = excluded.codec, audio_tracks = excluded.audio_tracks
            """, ("$p", path), ("$s", size), ("$m", mtime), ("$w", s.Width), ("$h", s.Height), ("$f", s.Fps), ("$d", s.DurationUs), ("$c", s.VideoCodec), ("$a", s.AudioTracks));

    private IReadOnlyList<FileRecord> QueryFiles(string sql, int limit)
    {
        lock (_gate)
        {
            using var cmd = Command(sql, ("$n", limit));
            using var r = cmd.ExecuteReader();
            var list = new List<FileRecord>();
            while (r.Read()) list.Add(ReadFile(r));
            return list;
        }
    }

    private IReadOnlyList<string> QueryStrings(string sql, int limit)
    {
        lock (_gate)
        {
            using var cmd = Command(sql, ("$n", limit));
            using var r = cmd.ExecuteReader();
            var list = new List<string>();
            while (r.Read()) list.Add(r.GetString(0));
            return list;
        }
    }

    private static FileRecord ReadFile(SqliteDataReader r) =>
        new(r.GetString(0), r.GetInt64(1), r.GetInt64(2), r.GetInt64(3) != 0, r.IsDBNull(4) ? null : r.GetString(4), r.IsDBNull(5) ? null : r.GetString(5));

    private T Scalar<T>(string sql)
    {
        lock (_gate)
        {
            using var cmd = Command(sql);
            return (T)Convert.ChangeType(cmd.ExecuteScalar()!, typeof(T));
        }
    }

    private void Execute(string sql, params (string Name, object Value)[] args)
    {
        lock (_gate) ExecuteLocked(sql, args);
    }

    private void ExecuteLocked(string sql, params (string Name, object Value)[] args)
    {
        using var cmd = Command(sql, args);
        cmd.ExecuteNonQuery();
    }

    private SqliteCommand Command(string sql, params (string Name, object Value)[] args)
    {
        var cmd = _db.CreateCommand();
        cmd.CommandText = sql;
        foreach (var (name, value) in args) cmd.Parameters.AddWithValue(name, value);
        return cmd;
    }

    /// <summary>
    /// Copies a database through SQLite's backup API, which reads a consistent snapshot even while
    /// another process has the source open.
    /// </summary>
    public static void Copy(string source, string destination)
    {
        using var from = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = source, Mode = SqliteOpenMode.ReadOnly, Pooling = false }.ToString());
        using var to = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = destination, Pooling = false }.ToString());
        from.Open();
        to.Open();
        from.BackupDatabase(to);
    }

    public void Dispose()
    {
        lock (_gate) _db.Dispose();
    }
}
