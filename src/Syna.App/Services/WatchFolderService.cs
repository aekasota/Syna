using System.Collections.Concurrent;
using Syna.Core.Services;

namespace Syna.Services;

/// <summary>
/// Notifies when a new clip appears in a watched folder (including subfolders, where ShadowPlay puts
/// per-game recordings). Uses file-system notifications; the only polling is a short size-stability
/// check on a file that was just announced, so a clip is reported once it is completely written.
/// </summary>
public sealed class WatchFolderService : IDisposable
{
    private readonly Dictionary<string, FileSystemWatcher> _watchers = new(StringComparer.OrdinalIgnoreCase);
    private readonly ConcurrentDictionary<string, byte> _settling = new(StringComparer.OrdinalIgnoreCase);
    private readonly CancellationTokenSource _stop = new();

    public event Action<string>? ClipReady;

    public void SetFolders(IEnumerable<string> folders)
    {
        var wanted = new HashSet<string>(folders.Where(Directory.Exists), StringComparer.OrdinalIgnoreCase);
        foreach (string gone in _watchers.Keys.Where(k => !wanted.Contains(k)).ToList())
        {
            _watchers[gone].Dispose();
            _watchers.Remove(gone);
        }
        foreach (string folder in wanted.Where(f => !_watchers.ContainsKey(f)))
        {
            try
            {
                var w = new FileSystemWatcher(folder)
                {
                    IncludeSubdirectories = true,
                    NotifyFilter = NotifyFilters.FileName | NotifyFilters.Size,
                    InternalBufferSize = 64 * 1024,
                };
                w.Created += (_, e) => Consider(e.FullPath);
                w.Renamed += (_, e) => Consider(e.FullPath);
                w.EnableRaisingEvents = true;
                _watchers[folder] = w;
                Log.Info("watch", $"watching {folder}");
            }
            catch (Exception ex) when (ex is IOException or ArgumentException or UnauthorizedAccessException)
            {
                Log.Warn("watch", $"cannot watch {folder}: {ex.Message}");
            }
        }
    }

    private void Consider(string path)
    {
        if (!MediaFiles.IsSupported(path) || !_settling.TryAdd(path, 0)) return;
        _ = Task.Run(async () =>
        {
            try
            {
                long last = -1;
                for (int i = 0; i < 120 && !_stop.IsCancellationRequested; i++)
                {
                    await Task.Delay(1000, _stop.Token);
                    var info = new FileInfo(path);
                    if (!info.Exists) return;
                    if (info.Length > 0 && info.Length == last && CanRead(path))
                    {
                        ClipReady?.Invoke(path);
                        return;
                    }
                    last = info.Length;
                }
            }
            catch (OperationCanceledException)
            {
            }
            finally
            {
                _settling.TryRemove(path, out _);
            }
        });
    }

    private static bool CanRead(string path)
    {
        try
        {
            using var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    public void Dispose()
    {
        _stop.Cancel();
        foreach (var w in _watchers.Values) w.Dispose();
        _watchers.Clear();
    }
}
