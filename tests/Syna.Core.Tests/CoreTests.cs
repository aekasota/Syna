using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;
using Xunit;

namespace Syna.Core.Tests;

public sealed class TempDir : IDisposable
{
    public string Path { get; } = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "syna-core-tests", Guid.NewGuid().ToString("N"));
    public TempDir() => Directory.CreateDirectory(Path);
    public void Dispose()
    {
        try { Directory.Delete(Path, recursive: true); } catch (IOException) { }
    }
}

public class OutputNamingTests
{
    [Fact]
    public void Picks_suffix_then_numbers_and_skips_partials()
    {
        using var dir = new TempDir();
        string first = MediaFiles.NextAvailable(dir.Path, "clip.mp4", "_trimmed", ".mp4");
        Assert.Equal(Path.Combine(dir.Path, "clip_trimmed.mp4"), first);
        File.WriteAllText(first, "");
        File.WriteAllText(Path.Combine(dir.Path, "clip_trimmed_2.mp4.partial"), "");
        Assert.Equal(Path.Combine(dir.Path, "clip_trimmed_3.mp4"), MediaFiles.NextAvailable(dir.Path, "clip.mp4", "_trimmed", ".mp4"));
    }

    [Fact]
    public void Cache_key_changes_with_size_or_time()
    {
        var t = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        string a = MediaFiles.CacheKey(@"C:\v\a.mp4", 100, t);
        Assert.Equal(a, MediaFiles.CacheKey(@"c:\V\A.MP4", 100, t));
        Assert.NotEqual(a, MediaFiles.CacheKey(@"C:\v\a.mp4", 101, t));
        Assert.NotEqual(a, MediaFiles.CacheKey(@"C:\v\a.mp4", 100, t.AddSeconds(1)));
    }

    [Theory]
    [InlineData(0L, "0:00.000")]
    [InlineData(42_531_000L, "0:42.531")]
    [InlineData(3_727_000_000L, "1:02:07.000")]
    [InlineData(16_667L, "0:00.017")]
    public void Timecodes(long us, string expected) => Assert.Equal(expected, Timecode.Precise(us));
}

public class WaveformDataTests
{
    private static byte[] Build(params ushort[][] tracks)
    {
        var bytes = new List<byte>();
        bytes.AddRange("SYNW"u8.ToArray());
        void U32(uint v) { var b = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, v); bytes.AddRange(b); }
        U32(1);
        U32((uint)tracks.Length);
        U32(10);
        var d = new byte[8];
        BinaryPrimitives.WriteInt64LittleEndian(d, 1_000_000);
        bytes.AddRange(d);
        foreach (var t in tracks)
        {
            U32((uint)t.Length);
            foreach (ushort v in t)
            {
                var b = new byte[4];
                BinaryPrimitives.WriteUInt16LittleEndian(b, v);
                BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(2), (ushort)(v / 2));
                bytes.AddRange(b);
            }
        }
        return bytes.ToArray();
    }

    [Fact]
    public void Parses_and_reduces_to_columns_by_max()
    {
        var data = WaveformData.Parse(Build([0, 65535, 0, 0, 32768, 0, 0, 0, 0, 65535]))!;
        Assert.Single(data.Tracks);
        Span<float> peaks = stackalloc float[5], rms = stackalloc float[5];
        data.Tracks[0].Render(0, 1_000_000, peaks, rms);
        Assert.Equal(1f, peaks[0], 3);
        Assert.Equal(0f, peaks[1], 3);
        Assert.Equal(0.5f, peaks[2], 2);
        Assert.Equal(1f, peaks[4], 3);
        Assert.Equal(0.5f, rms[0], 2);
    }

    [Fact]
    public void Rejects_garbage() => Assert.Null(WaveformData.Parse("nope"u8));
}

public class DatabaseTests
{
    [Fact]
    public void Tracks_recents_favorites_positions_and_sessions()
    {
        using var dir = new TempDir();
        using (var db = new AppDatabase(Path.Combine(dir.Path, "state.db")))
        {
            db.MarkOpened(@"C:\a.mp4");
            Thread.Sleep(2);
            db.MarkOpened(@"C:\b.mp4");
            db.SavePosition(@"C:\a.mp4", 12_345);
            db.SetFavorite(@"C:\b.mp4", true);
            db.SaveMixer(@"C:\a.mp4", "{\"m\":1}", null);
            db.SaveSessions([@"C:\a.mp4", @"C:\b.mp4"], 1);
            db.AddWatchFolder(@"C:\Videos");
            db.PutSummary(@"C:\a.mp4", 10, 20, new MediaSummary(1920, 1080, 60, 42_000_000, "h264", 2));
        }
        using (var db = new AppDatabase(Path.Combine(dir.Path, "state.db")))
        {
            Assert.Equal([@"C:\b.mp4", @"C:\a.mp4"], db.RecentFiles(10).Select(f => f.Path).ToArray());
            Assert.Equal(12_345, db.GetFile(@"c:\A.mp4")!.LastPositionUs);
            Assert.Equal("{\"m\":1}", db.GetFile(@"C:\a.mp4")!.MixerJson);
            Assert.Single(db.Favorites());
            var (paths, active) = db.LoadSessions();
            Assert.Equal(2, paths.Count);
            Assert.Equal(1, active);
            Assert.Equal([@"C:\Videos"], db.WatchFolders().ToArray());
            Assert.Equal(1920, db.GetSummary(@"C:\a.mp4", 10, 20)!.Width);
            Assert.Null(db.GetSummary(@"C:\a.mp4", 11, 20));
            db.RenamePath(@"C:\a.mp4", @"C:\c.mp4");
            Assert.Equal(12_345, db.GetFile(@"C:\c.mp4")!.LastPositionUs);
        }
    }

}

public class LegacyImportTests
{
    private static string Hash(string path) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(path)));

    [Fact]
    public void Imports_settings_and_history_without_writing_to_the_old_folder()
    {
        using var dir = new TempDir();
        string old = Path.Combine(dir.Path, "old"), fresh = Path.Combine(dir.Path, "new");
        using (var db = new AppDatabase(Path.Combine(old, "state.db")))
        {
            db.MarkOpened(@"C:\a.mp4");
            db.SavePosition(@"C:\a.mp4", 7_000);
        }
        File.WriteAllText(Path.Combine(old, "settings.json"), "{\"masterGain\":0.5}");
        var before = Directory.GetFiles(old).ToDictionary(f => Path.GetFileName(f)!, Hash);

        Assert.True(LegacyImport.Run(old, fresh));
        Assert.Equal(before, Directory.GetFiles(old).ToDictionary(f => Path.GetFileName(f)!, Hash));
        Assert.Equal("{\"masterGain\":0.5}", File.ReadAllText(Path.Combine(fresh, "settings.json")));
        using (var db = new AppDatabase(Path.Combine(fresh, "state.db")))
            Assert.Equal(7_000, db.GetFile(@"C:\a.mp4")!.LastPositionUs);

        Assert.False(LegacyImport.Run(old, fresh));  // only ever once
    }

    [Fact]
    public void Reads_history_through_the_log_of_a_running_previous_version()
    {
        // The previous version may still be open, with recent writes only in its write-ahead log.
        using var dir = new TempDir();
        string old = Path.Combine(dir.Path, "old"), fresh = Path.Combine(dir.Path, "new");
        using var open = new AppDatabase(Path.Combine(old, "state.db"));
        open.MarkOpened(@"C:\a.mp4");
        open.SavePosition(@"C:\a.mp4", 7_000);
        open.SaveSessions([@"C:\a.mp4"], 0);
        Assert.True(File.Exists(Path.Combine(old, "state.db-wal")));

        Assert.True(LegacyImport.Run(old, fresh));
        using var db = new AppDatabase(Path.Combine(fresh, "state.db"));
        Assert.Equal(7_000, db.GetFile(@"C:\a.mp4")!.LastPositionUs);
        Assert.Single(db.LoadSessions().Paths);
    }

    [Fact]
    public void Does_nothing_without_a_previous_folder()
    {
        using var dir = new TempDir();
        Assert.False(LegacyImport.Run(Path.Combine(dir.Path, "missing"), Path.Combine(dir.Path, "new")));
        Assert.False(Directory.Exists(Path.Combine(dir.Path, "new")));
    }
}

public class SettingsTests
{
    [Fact]
    public void Round_trips_and_survives_corruption()
    {
        using var dir = new TempDir();
        string path = Path.Combine(dir.Path, "settings.json");
        var store = new SettingsStore(path);
        var s = store.Load();
        s.TrimMode = TrimMode.Exact;
        s.Theme = ThemePreference.Light;
        s.DefaultTrackGains[1] = 0.5;
        store.Save(s);
        var loaded = store.Load();
        Assert.Equal(TrimMode.Exact, loaded.TrimMode);
        Assert.Equal(ThemePreference.Light, loaded.Theme);
        Assert.Equal(0.5, loaded.DefaultTrackGains[1]);
        File.WriteAllText(path, "{ not json");
        Assert.Equal(TrimMode.Smart, store.Load().TrimMode);
    }
}

public class SourceTextTests
{
    // UTF-8 text read through Windows-1252 and saved again: a lead byte (C2, C3 or E2: Latin-1 signs,
    // punctuation, arrows) followed by a continuation byte, both shown as Windows-1252 characters.
    // That turned an ellipsis into U+00E2 U+20AC U+00A6 and a multiplication sign into U+00C3 U+2014
    // in the track and speed menus once. Built from the bytes, so this file holds none of it.
    private static readonly System.Text.RegularExpressions.Regex Damage = BuildDamage();

    private static System.Text.RegularExpressions.Regex BuildDamage()
    {
        System.Text.Encoding.RegisterProvider(System.Text.CodePagesEncodingProvider.Instance);
        var cp1252 = System.Text.Encoding.GetEncoding(1252);
        string lead = cp1252.GetString([0xC2, 0xC3, 0xE2]);
        string continuation = cp1252.GetString(Enumerable.Range(0x80, 0x40).Select(b => (byte)b).ToArray());
        return new($"[{lead}][{continuation}]|{(char)0xFFFD}");
    }

    private static string RepositoryRoot([CallerFilePath] string here = "") => Path.GetFullPath(Path.Combine(Path.GetDirectoryName(here)!, "..", ".."));

    [Fact]
    public void Project_text_carries_no_encoding_damage()
    {
        string root = RepositoryRoot();
        string[] types = [".cs", ".xaml", ".cpp", ".h", ".hlsl", ".md", ".ps1", ".json", ".txt", ".csproj", ".wxs", ".wixproj", ".yml"];
        var files = Directory.EnumerateFiles(root).Where(f => types.Contains(Path.GetExtension(f)) || Path.GetFileName(f) == "LICENSE").ToList();
        foreach (string dir in new[] { "src", "tests", "docs", "scripts", "installer", "licenses", ".github" }.Select(d => Path.Combine(root, d)).Where(Directory.Exists))
            files.AddRange(Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Where(f => types.Contains(Path.GetExtension(f))));
        files.RemoveAll(f => Path.GetRelativePath(root, f).Split(Path.DirectorySeparatorChar).Any(p => p is "bin" or "obj" or "generated"));
        var hits = new List<string>();
        foreach (string file in files)
            foreach (var (line, n) in File.ReadLines(file).Select((l, k) => (l, k + 1)))
                if (Damage.Match(line) is { Success: true } m)
                    hits.Add($"{Path.GetRelativePath(root, file)}:{n}: {string.Join(" ", m.Value.Select(c => $"U+{(int)c:X4}"))}");
        Assert.True(files.Count > 100, $"only {files.Count} files scanned");
        Assert.Empty(hits);
        Assert.Matches(Damage, "Rename track" + (char)0xE2 + (char)0x20AC + (char)0xA6);  // the original defect is caught
        Assert.Matches(Damage, "1.5" + (char)0xC3 + (char)0x2014);
        Assert.DoesNotMatch(Damage, "Rename track… · 1.5× · −6 dB · café · Syna");
    }
}

public class InteropTests
{
    private static string Fixture(string name, [CallerFilePath] string here = "") =>
        Path.Combine(Path.GetDirectoryName(here)!, "..", "fixtures", "generated", name);

    [Fact]
    public void Struct_layouts_match_the_c_abi()
    {
        Assert.Equal(192, Unsafe.SizeOf<PlayerState>());
        Assert.Equal(24, Unsafe.SizeOf<EngineEvent>());
        Assert.Equal(56, Unsafe.SizeOf<ProcessStats>());
        // syna_trim_request: two pointers and two int64, then 2 + 9 video + 5 audio int32 fields, then
        // the mixer's track array and count.
        Assert.Equal(112, System.Runtime.InteropServices.Marshal.SizeOf<Native.TrimRequestNative>());
        Assert.Equal(40, System.Runtime.InteropServices.Marshal.OffsetOf<Native.TrimRequestNative>(nameof(Native.TrimRequestNative.Video)).ToInt32());
        Assert.Equal(76, System.Runtime.InteropServices.Marshal.OffsetOf<Native.TrimRequestNative>(nameof(Native.TrimRequestNative.Audio)).ToInt32());
        Assert.Equal(92, System.Runtime.InteropServices.Marshal.OffsetOf<Native.TrimRequestNative>(nameof(Native.TrimRequestNative.AudioChannels)).ToInt32());
        Assert.Equal(96, System.Runtime.InteropServices.Marshal.OffsetOf<Native.TrimRequestNative>(nameof(Native.TrimRequestNative.Tracks)).ToInt32());
        Assert.Equal(104, System.Runtime.InteropServices.Marshal.OffsetOf<Native.TrimRequestNative>(nameof(Native.TrimRequestNative.TrackCount)).ToInt32());
        Assert.Equal(16, Unsafe.SizeOf<Native.TrimTrackNative>());
        Assert.Equal(8, System.Runtime.InteropServices.Marshal.OffsetOf<Native.TrimTrackNative>(nameof(Native.TrimTrackNative.OffsetUs)).ToInt32());
    }

    [Fact]
    public void Probe_round_trips_through_json()
    {
        string file = Fixture("A_h264_1080p60_2trk.mp4");
        Assert.SkipUnless(File.Exists(file), "fixtures not generated");
        MediaEngine.Initialize(null, false);
        MediaInfo info = MediaEngine.Probe(file, full: true);
        Assert.Equal("MP4", info.Container);
        Assert.Equal(1920, info.Video!.Width);
        Assert.Equal(20, info.Video.KeyframesUs.Length);
        Assert.Equal(["System sounds", "Microphone"], info.Audio.Select(a => a.Label).ToArray());
    }

    [Fact]
    public void Missing_files_raise_typed_errors()
    {
        MediaEngine.Initialize(null, false);
        var ex = Assert.Throws<EngineException>(() => MediaEngine.Probe(@"C:\definitely\missing.mp4", false));
        Assert.Equal(EngineError.NotFound, ex.Error);
    }
}
