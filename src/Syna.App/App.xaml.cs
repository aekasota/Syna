using Syna.Core.Engine;
using Syna.Services;
using Syna.ViewModels;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;

namespace Syna;

public partial class App : Application
{
    private static readonly object RedirectGate = new();
    // Redirected activations that arrived before OnLaunched could take them; null from then on.
    private static List<(ExtendedActivationKind Kind, IReadOnlyList<string> Paths)>? _earlyRedirects = [];

    private readonly AppActivationArguments _launchArgs;
    private readonly List<string> _pending = [];
    private bool _shellReady;
    private DispatcherQueue? _ui;

    public App(AppActivationArguments launchArgs)
    {
        _launchArgs = launchArgs;
        Current = this;
        InitializeComponent();
        UnhandledException += (_, e) => Log.Error("app", $"unhandled: {e.Exception}");
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Log.Error("app", $"unhandled (domain): {e.ExceptionObject}");
        TaskScheduler.UnobservedTaskException += (_, e) => Log.Error("app", $"unobserved task: {e.Exception}");
    }

    public static new App? Current { get; private set; }

    public MainWindow? Window { get; private set; }
    public MainViewModel? ViewModel { get; private set; }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            MediaEngine.Initialize(AppPaths.Logs, verbose: Environment.GetEnvironmentVariable("SYNA_VERBOSE") == "1");
        }
        catch (Exception ex)
        {
            StartupFailure.Report("Syna could not load its media engine (Syna.Native.dll and the FFmpeg libraries next to Syna.exe).", ex);
            return;
        }
        _pending.AddRange(ActivationParser.GetPaths(_launchArgs));
        try
        {
            ViewModel = new MainViewModel(DispatcherQueue.GetForCurrentThread());
            Window = new MainWindow(ViewModel);
            Window.Activate();
        }
        catch (Exception ex)
        {
            StartupFailure.Report("Syna could not open its main window.", ex);
            return;
        }
        _ui = DispatcherQueue.GetForCurrentThread();
        List<(ExtendedActivationKind Kind, IReadOnlyList<string> Paths)> early;
        lock (RedirectGate)
        {
            early = _earlyRedirects!;
            _earlyRedirects = null;
        }
        foreach (var (kind, paths) in early) Receive(kind, paths);
    }

    /// <summary>The video surface exists: restore the previous sessions, then open what Windows passed in.</summary>
    internal void OnShellReady()
    {
        if (_shellReady || ViewModel == null) return;
        _shellReady = true;
        if (_pending.Count == 0) ViewModel.RestoreSessions();
        else
        {
            ViewModel.RestoreSessionsWithoutActivating();
            ViewModel.OpenPaths(_pending);
        }
        _pending.Clear();
        _ = ViewModel.Explorer.NavigateAsync(StartFolder());
    }

    private string StartFolder()
    {
        string last = ViewModel!.Settings.LastFolder;
        if (last.Length > 0 && Directory.Exists(last)) return last;
        string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        return Directory.Exists(videos) ? videos : "";
    }

    // Raised on a background thread when another launch was redirected to this instance (a clip opened
    // from Explorer, the Start menu entry). It can come while this instance is still starting, before
    // the App or its window exists: several clips opened together start one process each. Those are
    // kept until OnLaunched takes them.
    internal static void OnRedirectedActivation(AppActivationArguments args)
    {
        ExtendedActivationKind kind = args.Kind;
        IReadOnlyList<string> paths = ActivationParser.GetPaths(args);
        lock (RedirectGate)
        {
            if (_earlyRedirects != null)
            {
                _earlyRedirects.Add((kind, paths));
                return;
            }
        }
        App app = Current!;
        app._ui!.TryEnqueue(() => app.Receive(kind, paths));
    }

    private void Receive(ExtendedActivationKind kind, IReadOnlyList<string> paths)
    {
        Log.Info("app", $"redirected activation kind={kind} files={paths.Count}");
        Window!.BringToFront();
        if (_shellReady) Window.OpenPaths(paths);
        else _pending.AddRange(paths);
    }
}
