using System.Collections.Concurrent;
using Syna.Core.Services;
using Xunit;

namespace Syna.Core.Tests;

/// <summary>A clock that only moves when told to; due timers fire synchronously, in order.</summary>
public sealed class ManualTimeProvider : TimeProvider
{
    private readonly List<ManualTimer> _timers = [];
    private TimeSpan _now;

    public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
    {
        var t = new ManualTimer(this, callback, state, _now + dueTime);
        _timers.Add(t);
        return t;
    }

    public void Advance(TimeSpan by)
    {
        TimeSpan target = _now + by;
        for (;;)
        {
            ManualTimer? next = _timers.Where(t => t.Due <= target).OrderBy(t => t.Due).FirstOrDefault();
            if (next == null) break;
            _timers.Remove(next);
            _now = next.Due;
            next.Fire();
        }
        _now = target;
    }

    private sealed class ManualTimer(ManualTimeProvider owner, TimerCallback callback, object? state, TimeSpan due) : ITimer
    {
        public TimeSpan Due { get; private set; } = due;
        public void Fire() => callback(state);
        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            Due = owner._now + dueTime;
            if (!owner._timers.Contains(this)) owner._timers.Add(this);
            return true;
        }
        public void Dispose() => owner._timers.Remove(this);
        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}

public class NotificationTests
{
    private readonly ManualTimeProvider _clock = new();
    private readonly Queue<Action> _ui = new();
    private readonly List<string> _events = [];

    private NotificationCenter Center(bool queued = false)
    {
        var center = new NotificationCenter(_clock, queued ? _ui.Enqueue : a => a());
        center.Shown += n => _events.Add("shown " + n.Message);
        center.Dismissing += n => _events.Add("dismissing " + n.Message);
        center.Removed += n => _events.Add("removed " + n.Message);
        return center;
    }

    private void RunUi()
    {
        while (_ui.TryDequeue(out var work)) work();
    }

    private static readonly TimeSpan Ms = TimeSpan.FromMilliseconds(1);

    [Fact]
    public void Dismisses_itself_after_its_duration_and_is_removed_after_the_fade()
    {
        var center = Center();
        var n = center.Show("Saved clip.mp4");
        _clock.Advance(3499 * Ms);
        Assert.Equal(NotificationState.Visible, n.State);
        _clock.Advance(1 * Ms);
        Assert.Equal(NotificationState.Dismissing, n.State);
        _clock.Advance(NotificationCenter.FadeOut);
        Assert.Equal(NotificationState.Removed, n.State);
        Assert.Empty(center.Active);
        Assert.Equal(new[] { "shown Saved clip.mp4", "dismissing Saved clip.mp4", "removed Saved clip.mp4" }, _events);
    }

    [Fact]
    public void Notifications_with_an_action_stay_longer()
    {
        var center = Center();
        var n = center.Show("New clip", "Open", () => { });
        _clock.Advance(TimeSpan.FromSeconds(5.9));
        Assert.Equal(NotificationState.Visible, n.State);
        _clock.Advance(TimeSpan.FromSeconds(0.1) + NotificationCenter.FadeOut);
        Assert.Equal(NotificationState.Removed, n.State);
    }

    [Fact]
    public void Closing_by_hand_cancels_the_automatic_dismissal()
    {
        var center = Center();
        var n = center.Show("Saved clip.mp4");
        _clock.Advance(TimeSpan.FromSeconds(1));
        n.Dismiss();
        Assert.Equal(NotificationState.Dismissing, n.State);
        _clock.Advance(NotificationCenter.FadeOut);
        Assert.Equal(NotificationState.Removed, n.State);
        _clock.Advance(TimeSpan.FromSeconds(10));
        Assert.Equal(1, _events.Count(e => e.StartsWith("dismissing")));
        Assert.Equal(1, _events.Count(e => e.StartsWith("removed")));
    }

    [Fact]
    public void Closing_by_hand_just_before_the_timeout_dismisses_once()
    {
        var center = Center();
        var n = center.Show("Saved clip.mp4");
        _clock.Advance(3499 * Ms);
        n.Dismiss();
        _clock.Advance(TimeSpan.FromSeconds(1));
        Assert.Equal(NotificationState.Removed, n.State);
        Assert.Equal(new[] { "shown Saved clip.mp4", "dismissing Saved clip.mp4", "removed Saved clip.mp4" }, _events);
    }

    [Fact]
    public void A_timeout_and_a_click_arriving_together_resolve_to_one_dismissal()
    {
        // The timer fires (its work is queued for the UI thread), then the click runs first.
        var center = Center(queued: true);
        var n = center.Show("Saved clip.mp4");
        _clock.Advance(NotificationCenter.DefaultDuration);
        Assert.Single(_ui);
        n.Dismiss();
        RunUi();
        Assert.Equal(NotificationState.Dismissing, n.State);
        _clock.Advance(NotificationCenter.FadeOut);
        RunUi();
        Assert.Equal(NotificationState.Removed, n.State);
        Assert.Equal(new[] { "shown Saved clip.mp4", "dismissing Saved clip.mp4", "removed Saved clip.mp4" }, _events);

        // The other order: the timeout is handled, then the click finds it already going.
        _events.Clear();
        var m = center.Show("Second");
        _clock.Advance(NotificationCenter.DefaultDuration);
        RunUi();
        m.Dismiss();
        _clock.Advance(NotificationCenter.FadeOut);
        RunUi();
        Assert.Equal(new[] { "shown Second", "dismissing Second", "removed Second" }, _events);
        Assert.Empty(center.Active);
    }

    [Fact]
    public void Many_notifications_keep_three_on_screen_and_all_go_away()
    {
        var center = Center();
        var shown = Enumerable.Range(1, 10).Select(i => center.Show($"Saved clip_{i}.mp4")).ToList();
        Assert.Equal(NotificationCenter.MaxVisible, center.Active.Count(n => n.State == NotificationState.Visible));
        Assert.All(shown.TakeLast(3), n => Assert.Equal(NotificationState.Visible, n.State));
        _clock.Advance(NotificationCenter.FadeOut);
        Assert.Equal(3, center.Active.Count);
        _clock.Advance(NotificationCenter.DefaultDuration + NotificationCenter.FadeOut);
        Assert.Empty(center.Active);
        Assert.Equal(10, _events.Count(e => e.StartsWith("removed")));
        Assert.Equal(10, _events.Count(e => e.StartsWith("dismissing")));
    }

    [Fact]
    public void A_new_notification_while_another_fades_out_does_not_disturb_it()
    {
        var center = Center();
        var first = center.Show("First");
        first.Dismiss();
        _clock.Advance(NotificationCenter.FadeOut / 2);
        var second = center.Show("Second");
        _clock.Advance(NotificationCenter.FadeOut / 2);
        Assert.Equal(NotificationState.Removed, first.State);
        Assert.Equal(NotificationState.Visible, second.State);
        Assert.Equal(new[] { second }, center.Active);
    }

    [Fact]
    public void Repeated_cycles_do_not_accumulate()
    {
        var center = Center();
        for (int i = 0; i < 50; i++)
        {
            var n = center.Show($"Saved {i}");
            if (i % 2 == 0) n.Dismiss();
            _clock.Advance(TimeSpan.FromSeconds(4));
        }
        Assert.Empty(center.Active);
        Assert.Equal(50, _events.Count(e => e.StartsWith("removed")));
    }

    [Fact]
    public void The_action_runs_once_and_dismisses()
    {
        var center = Center();
        int runs = 0;
        var n = center.Show("Saved frame", "Reveal", () => runs++);
        n.InvokeAction();
        n.InvokeAction();
        Assert.Equal(1, runs);
        Assert.Equal(NotificationState.Dismissing, n.State);
        n.Dismiss();
        Assert.Equal(1, _events.Count(e => e.StartsWith("dismissing")));
    }

    [Fact]
    public void Real_timers_still_fire_after_a_garbage_collection()
    {
        // The original failure: timers that nothing referenced were collected and never fired.
        var ui = new ConcurrentQueue<Action>();
        var center = new NotificationCenter(TimeProvider.System, ui.Enqueue);
        var n = center.Show("Saved clip.mp4", duration: TimeSpan.FromMilliseconds(50));
        for (int i = 0; i < 3; i++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (n.State != NotificationState.Removed && DateTime.UtcNow < deadline)
        {
            while (ui.TryDequeue(out var work)) work();
            Thread.Sleep(5);
            GC.Collect();
        }
        Assert.Equal(NotificationState.Removed, n.State);
        Assert.Empty(center.Active);
    }
}
