using System.Security.Cryptography;
using System.Text;

namespace Syna.Core.Services;

public static class MediaFiles
{
    /// <summary>Extensions the engine handles robustly; also the set registered for file associations.</summary>
    public static readonly string[] Extensions = [".mp4", ".m4v", ".mov", ".mkv", ".webm"];

    public static bool IsSupported(string path) =>
        Extensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);

    /// <summary>Cheap cache identity: path + size + last write time (no content hashing).</summary>
    public static string CacheKey(string path, long size, DateTime lastWriteUtc)
    {
        string identity = $"{path.ToUpperInvariant()}|{size}|{lastWriteUtc.Ticks}";
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity)), 0, 16).ToLowerInvariant();
    }

    public static string? CacheKey(string path)
    {
        try
        {
            var info = new FileInfo(path);
            return info.Exists ? CacheKey(path, info.Length, info.LastWriteTimeUtc) : null;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>
    /// Collision-safe output path: "clip_trimmed.mp4", then "clip_trimmed_2.mp4", "clip_trimmed_3.mp4"...
    /// Never returns an existing path.
    /// </summary>
    public static string NextAvailable(string folder, string sourceName, string suffix, string extension)
    {
        string stem = Path.GetFileNameWithoutExtension(sourceName) + suffix;
        string candidate = Path.Combine(folder, stem + extension);
        for (int i = 2; File.Exists(candidate) || File.Exists(candidate + ".partial"); i++)
            candidate = Path.Combine(folder, $"{stem}_{i}{extension}");
        return candidate;
    }
}

public static class Timecode
{
    /// <summary>"m:ss.fff" (or "h:mm:ss.fff" past an hour).</summary>
    public static string Precise(long us)
    {
        if (us < 0) us = 0;
        long ms = (us + 500) / 1000;
        long h = ms / 3_600_000, m = ms / 60_000 % 60, s = ms / 1000 % 60, f = ms % 1000;
        return h > 0 ? $"{h}:{m:00}:{s:00}.{f:000}" : $"{m}:{s:00}.{f:000}";
    }

    /// <summary>"0:42" / "1:02:07" for durations and list metadata.</summary>
    public static string Short(long us)
    {
        if (us < 0) us = 0;
        long total = us / 1_000_000;
        long h = total / 3600, m = total / 60 % 60, s = total % 60;
        return h > 0 ? $"{h}:{m:00}:{s:00}" : $"{m}:{s:00}";
    }

    public static string Seconds(double seconds) => seconds < 10 ? $"{seconds:0.000} s" : $"{seconds:0.0} s";
}
