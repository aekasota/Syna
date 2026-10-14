namespace Syna.Core.Services;

public enum NotificationState { Visible, Dismissing, Removed }

/// <summary>A transient message (toast); its lifecycle belongs to <see cref="NotificationCenter"/>.</summary>
public sealed class Notification
{
    private readonly NotificationCenter _owner;
    private Action? _action;
    internal ITimer? Timer;
    internal int Generation;

    internal Notification(NotificationCenter owner, string message, string? actionLabel, Action? action, TimeSpan duration)
    {
        _owner = owner;
        Message = message;
        ActionLabel = actionLabel;
        _action = action;
        Duration = duration;
    }

    public string Message { get; }
    public string? ActionLabel { get; }
    public TimeSpan Duration { get; }
    public NotificationState State { get; internal set; }

    /// <summary>Runs the action at most once, then dismisses the notification.</summary>
    public void InvokeAction()
    {
        if (State != NotificationState.Visible) return;
        Action? action = _action;
        _action = null;
        action?.Invoke();
        Dismiss();
    }

    public void Dismiss() => _owner.Dismiss(this);
}

/// <summary>
/// Lifecycle of the transient notifications: shown, dismissed after their duration or by the user,
/// then removed once the fade-out has played. Each notification holds its current timer until it
/// fires or is replaced (a timer nothing references can be collected and never fire, leaving the
/// notification on screen), and every step runs through <c>post</c> on the UI thread. A timer
/// that fires after the notification moved on carries an old generation and is ignored, so a
/// timeout and a click arriving together resolve to a single dismissal.
/// </summary>
/// <remarks>Call <see cref="Show"/> and <see cref="Dismiss"/> on the thread <c>post</c> runs work on.</remarks>
public sealed class NotificationCenter(TimeProvider time, Action<Action> post)
{
    public static readonly TimeSpan DefaultDuration = TimeSpan.FromSeconds(3.5);
    public static readonly TimeSpan ActionDuration = TimeSpan.FromSeconds(6);
    public static readonly TimeSpan FadeOut = TimeSpan.FromMilliseconds(180);
    public const int MaxVisible = 3;

    private readonly List<Notification> _active = [];

    /// <summary>A notification to add to the screen.</summary>
    public event Action<Notification>? Shown;
    /// <summary>A notification starts fading out; it no longer responds to input.</summary>
    public event Action<Notification>? Dismissing;
    /// <summary>A notification's fade-out has finished: take it off the screen.</summary>
    public event Action<Notification>? Removed;

    /// <summary>Notifications on screen, including those fading out, oldest first.</summary>
    public IReadOnlyList<Notification> Active => _active;

    public Notification Show(string message, string? actionLabel = null, Action? action = null, TimeSpan? duration = null)
    {
        bool hasAction = actionLabel != null && action != null;
        var n = new Notification(this, message, hasAction ? actionLabel : null, hasAction ? action : null,
                                 duration ?? (hasAction ? ActionDuration : DefaultDuration));
        _active.Add(n);
        Shown?.Invoke(n);
        Schedule(n, n.Duration);
        // The oldest visible ones make room.
        var visible = _active.Where(x => x.State == NotificationState.Visible).ToList();
        foreach (var old in visible.Take(visible.Count - MaxVisible)) Dismiss(old);
        return n;
    }

    public void Dismiss(Notification n)
    {
        if (n.State != NotificationState.Visible) return;
        n.State = NotificationState.Dismissing;
        Dismissing?.Invoke(n);
        Schedule(n, FadeOut);
    }

    private void Schedule(Notification n, TimeSpan due)
    {
        n.Timer?.Dispose();
        int generation = ++n.Generation;
        n.Timer = time.CreateTimer(_ => post(() => Elapsed(n, generation)), null, due, Timeout.InfiniteTimeSpan);
    }

    private void Elapsed(Notification n, int generation)
    {
        if (generation != n.Generation) return;
        if (n.State == NotificationState.Visible) Dismiss(n);
        else if (n.State == NotificationState.Dismissing) Remove(n);
    }

    private void Remove(Notification n)
    {
        n.State = NotificationState.Removed;
        n.Timer?.Dispose();
        n.Timer = null;
        _active.Remove(n);
        Removed?.Invoke(n);
    }
}
