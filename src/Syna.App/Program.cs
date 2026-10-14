using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Syna.Core.Engine;
using Syna.Core.Services;
using Syna.Services;
using Microsoft.Windows.AppLifecycle;
using Microsoft.Windows.ApplicationModel.Resources;

namespace Syna;

// Single-instance entry point: a second launch (e.g. double-clicking another clip in Explorer)
// forwards its activation to the running instance and exits.
public static partial class Program
{
    private const string InstanceKey = "Syna.Main";

    // Run by the installer (installer/Package.wxs) after installing and before removing the files.
    private const string RegisterSwitch = "--register-file-types";
    private const string UnregisterSwitch = "--unregister-file-types";
    // Used by scripts/package.ps1: can this folder start? Exit code 0 when it can.
    private const string SelfCheckSwitch = "--self-check";

    [STAThread]
    private static int Main()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        string[] cli = Environment.GetCommandLineArgs();
        if (cli.Length == 2 && cli[1] is RegisterSwitch or UnregisterSwitch)
            return ChangeFileTypeRegistration(cli[1] == RegisterSwitch);
        if (cli.Length == 2 && cli[1] == SelfCheckSwitch) return SelfCheck();

        AppInstance current = AppInstance.GetCurrent();
        AppActivationArguments args = current.GetActivatedEventArgs();
        // Subscribed before the key is registered: other launches redirect here as soon as it is, and
        // the SDK discards a redirected activation that arrives while nothing is subscribed.
        current.Activated += (_, e) => App.OnRedirectedActivation(e);
        AppInstance main = AppInstance.FindOrRegisterForKey(InstanceKey);
        if (!main.IsCurrent)
        {
            RedirectAndWait(args, main);
            return 0;
        }
        StartLog(args);
        if (!InterfaceResourcesPresent())
        {
            StartupFailure.Report("This Syna installation is incomplete: the interface resources in Syna.pri are missing.");
            return 1;
        }
        Application.Start(p =>
        {
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            try
            {
                _ = new App(args);
            }
            catch (Exception ex)
            {
                StartupFailure.Report("Syna could not load its interface.", ex);
            }
        });
        return 0;
    }

    // An installed (self-contained) build has WinUI next to Syna.exe and carries WinUI's own resources
    // in Syna.pri. Without them XAML fails fast inside Microsoft.UI.Xaml.dll before any app code runs,
    // so the window would silently never appear. Development builds use the Windows App Runtime's copy.
    private static bool InterfaceResourcesPresent()
    {
        if (!File.Exists(Path.Combine(AppContext.BaseDirectory, "Microsoft.UI.Xaml.dll"))) return true;
        try
        {
            return new ResourceManager().MainResourceMap.TryGetValue("Files/Microsoft.UI.Xaml/Themes/themeresources.xbf") != null;
        }
        catch (Exception ex) when (ex is COMException or FileNotFoundException)
        {
            Log.Error("app", $"cannot read Syna.pri: {ex.Message}");
            return false;
        }
    }

    // What a window launch needs before any of it is visible: WinUI's resources (2 if missing), and the
    // engine with the FFmpeg libraries it imports (3 if they cannot be loaded). Opens no window.
    private static int SelfCheck()
    {
        if (!InterfaceResourcesPresent()) return 2;
        try
        {
            MediaEngine.Initialize(null, verbose: false);
            return 0;
        }
        catch (Exception)
        {
            return 3;
        }
    }

    // One line per window process: how Windows started it (Start menu, Explorer, a script) and what it
    // asked to open. Runs before any XAML loads, so startup failures are logged too.
    private static void StartLog(AppActivationArguments args)
    {
        string? imported = AppPaths.ImportLegacyData();
        Log.Initialize(AppPaths.Logs);
        Log.Info("app", $"start exe=\"{Environment.ProcessPath}\" cwd=\"{Environment.CurrentDirectory}\" " +
                        $"activation={args.Kind} files={ActivationParser.GetPaths(args).Count}");
        if (imported != null) Log.Info("app", imported);
    }

    // Prints the registration it reads back, which the installer writes to its log; exit code 0 when
    // every supported type (or, after unregistering, none) lists this executable.
    private static int ChangeFileTypeRegistration(bool register)
    {
        try
        {
            if (register) ShellIntegration.RegisterAssociations();
            else ShellIntegration.UnregisterAssociations();
        }
        catch (Exception ex) when (ex is COMException or UnauthorizedAccessException or IOException)
        {
            Console.Error.WriteLine($"Changing the file type registration failed: {ex.Message}");
            return 1;
        }
        IReadOnlyList<string> registered = ShellIntegration.RegisteredExtensions();
        Console.WriteLine($"File types that open with {ShellIntegration.ExecutablePath}: " +
                          (registered.Count == 0 ? "none" : string.Join(" ", registered)));
        return registered.Count == (register ? MediaFiles.Extensions.Length : 0) ? 0 : 1;
    }

    private static void RedirectAndWait(AppActivationArguments args, AppInstance target)
    {
        // This process received the user's input, so it may hand foreground rights to the target.
        AllowSetForegroundWindow(target.ProcessId);
        nint done = CreateEventW(0, 1, 0, null);
        Task.Run(() =>
        {
            target.RedirectActivationToAsync(args).AsTask().Wait();
            SetEvent(done);
        });
        // Pump COM calls while waiting: required by RedirectActivationToAsync on an STA thread.
        _ = CoWaitForMultipleObjects(0, 0xFFFFFFFF, 1, [done], out _);
        CloseHandle(done);
    }

    [LibraryImport("kernel32.dll", EntryPoint = "CreateEventW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint CreateEventW(nint attributes, int manualReset, int initialState, string? name);

    [LibraryImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetEvent(nint handle);

    [LibraryImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CloseHandle(nint handle);

    [LibraryImport("ole32.dll")]
    private static partial int CoWaitForMultipleObjects(uint flags, uint timeout, uint count, [In] nint[] handles, out uint index);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool AllowSetForegroundWindow(uint processId);
}
