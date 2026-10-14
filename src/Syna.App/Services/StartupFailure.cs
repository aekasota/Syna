using System.Runtime.InteropServices;

namespace Syna.Services;

/// <summary>
/// Last resort when the app cannot start (missing or damaged files, the engine cannot be loaded):
/// the details go to the log, and the user gets a plain message box, which needs no XAML, instead of
/// the window silently never appearing.
/// </summary>
public static partial class StartupFailure
{
    public static void Report(string what, Exception? ex = null)
    {
        Log.Error("app", $"startup failed: {what} {ex}");
        _ = MessageBoxW(0, $"{what}\n\nReinstalling Syna usually fixes this. Details are in the log folder:\n{AppPaths.Logs}", "Syna",
                        0x10 /* MB_ICONERROR */ | 0x10000 /* MB_SETFOREGROUND */);
        Environment.Exit(1);
    }

    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int MessageBoxW(nint owner, string text, string caption, uint type);
}
