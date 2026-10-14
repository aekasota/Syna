using System.Collections.Concurrent;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Core.State;

namespace Syna.Services;

/// <summary>
/// One low-priority worker thread fed by a LIFO stack: the most recently requested item (usually what
/// just scrolled into view) is processed first. Never competes with playback threads for CPU.
/// </summary>
public sealed class LowPriorityWorker : IDisposable
{
    private readonly ConcurrentStack<Action> _work = new();
    private readonly SemaphoreSlim _signal = new(0);
    private readonly CancellationTokenSource _stop = new();
    private readonly Thread _thread;

    public LowPriorityWorker(string name)
    {
        _thread = new Thread(Run) { IsBackground = true, Priority = ThreadPriority.BelowNormal, Name = name };
        _thread.Start();
    }

    public void Post(Action work)
    {
        _work.Push(work);
        _signal.Release();
    }

    private void Run()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                _signal.Wait(_stop.Token);
                if (_work.TryPop(out var work))
                {
                    try { work(); }
                    catch (Exception ex) { Log.Warn("worker", $"{_thread.Name}: {ex.Message}"); }
                }
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    public void Dispose()
    {
        _stop.Cancel();
        _thread.Join(2000);
    }
}

/// <summary>JPEG thumbnails cached by path + size + mtime.</summary>
public sealed class ThumbnailService(LowPriorityWorker worker)
{
    private readonly ConcurrentDictionary<string, Task<string?>> _pending = new(StringComparer.OrdinalIgnoreCase);

    public Task<string?> GetAsync(string path)
    {
        string? key = MediaFiles.CacheKey(path);
        if (key == null) return Task.FromResult<string?>(null);
        string file = Path.Combine(AppPaths.Thumbnails, key + ".jpg");
        if (File.Exists(file)) return Task.FromResult<string?>(file);
        return _pending.GetOrAdd(key, k =>
        {
            var tcs = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
            worker.Post(() =>
            {
                try
                {
                    Directory.CreateDirectory(AppPaths.Thumbnails);
                    MediaEngine.MakeThumbnail(path, file, 384);
                    tcs.TrySetResult(file);
                }
                catch (EngineException ex)
                {
                    Log.Warn("thumbnails", $"{Path.GetFileName(path)}: {ex.Message}");
                    tcs.TrySetResult(null);
                }
                finally
                {
                    _pending.TryRemove(key, out _);
                }
            });
            return tcs.Task;
        });
    }
}

/// <summary>Waveform envelopes, generated once per file version by a background engine job.</summary>
public sealed class WaveformService : IDisposable
{
    private readonly SemaphoreSlim _gate = new(1);
    private readonly CancellationTokenSource _shutdown = new();
    public const int BucketsPerSecond = 200;

    public async Task<WaveformData?> GetAsync(string path, CancellationToken token)
    {
        string? key = MediaFiles.CacheKey(path);
        if (key == null) return null;
        string file = Path.Combine(AppPaths.Waveforms, $"{key}.{BucketsPerSecond}.synw");
        if (File.Exists(file)) return await Task.Run(() => WaveformData.Load(file), token);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(token, _shutdown.Token);
        await _gate.WaitAsync(linked.Token);
        try
        {
            if (!File.Exists(file))
            {
                Directory.CreateDirectory(AppPaths.Waveforms);
                using var job = WaveformJob.Start(path, file, BucketsPerSecond);
                var (state, error) = await job.WaitAsync(null, linked.Token);
                if (state != JobState.Succeeded)
                {
                    if (state == JobState.Failed) Log.Warn("waveform", $"{Path.GetFileName(path)}: {error}");
                    return null;
                }
            }
            return await Task.Run(() => WaveformData.Load(file), token);
        }
        finally
        {
            _gate.Release();
        }
    }

    public void Dispose() => _shutdown.Cancel();
}

/// <summary>Explorer metadata ("1920×1080 · 60 fps · 0:42") from a header-only probe, cached in SQLite.</summary>
public sealed class MediaIndex(LowPriorityWorker worker, AppDatabase db)
{
    public void Request(string path, Action<MediaSummary> onReady)
    {
        worker.Post(() =>
        {
            var info = new FileInfo(path);
            if (!info.Exists) return;
            long mtime = info.LastWriteTimeUtc.Ticks;
            MediaSummary? summary = db.GetSummary(path, info.Length, mtime);
            if (summary == null)
            {
                if (!MediaEngine.TryProbe(path, full: false, out MediaInfo? m, out _) || m == null) return;
                summary = new MediaSummary(m.Video?.Width ?? 0, m.Video?.Height ?? 0, m.Video?.FpsAvg ?? 0, m.DurationUs, m.Video?.Codec ?? "", m.Audio.Length);
                db.PutSummary(path, info.Length, mtime, summary);
            }
            onReady(summary);
        });
    }
}
