using System.Runtime.InteropServices;
using Microsoft.Windows.AppLifecycle;
using Windows.ApplicationModel.Activation;
using Windows.Storage;

namespace Syna.Services;

// Extracts file paths from launch and file activations (command line, Explorer double-click,
// "Open with").
public static partial class ActivationParser
{
    public static IReadOnlyList<string> GetPaths(AppActivationArguments args)
    {
        try
        {
            switch (args.Kind)
            {
                case ExtendedActivationKind.File when args.Data is IFileActivatedEventArgs file:
                    return file.Files.OfType<IStorageItem>().Select(f => f.Path).Where(p => !string.IsNullOrEmpty(p)).ToList();
                case ExtendedActivationKind.Launch when args.Data is ILaunchActivatedEventArgs launch:
                    return FromCommandLine(launch.Arguments);
            }
        }
        catch (Exception ex)
        {
            Log.Error("activation", $"cannot read activation payload: {ex.Message}");
        }
        return [];
    }

    public static IReadOnlyList<string> FromCommandLine(string? commandLine)
    {
        if (string.IsNullOrWhiteSpace(commandLine)) return [];
        string self = Environment.ProcessPath ?? "";
        return Split(commandLine)
            .Where(a => !a.StartsWith('-') && !string.Equals(Path.GetFullPath(a.Trim('"')), self, StringComparison.OrdinalIgnoreCase))
            .Select(a => Path.GetFullPath(a))
            .Where(p => File.Exists(p) || Directory.Exists(p))
            .ToList();
    }

    private static IEnumerable<string> Split(string commandLine)
    {
        nint argv = CommandLineToArgvW(commandLine, out int count);
        if (argv == 0) return [];
        try
        {
            var result = new string[count];
            for (int i = 0; i < count; i++) result[i] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, i * nint.Size)) ?? "";
            return result;
        }
        finally
        {
            LocalFree(argv);
        }
    }

    [LibraryImport("shell32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint CommandLineToArgvW(string commandLine, out int count);

    [LibraryImport("kernel32.dll")]
    private static partial nint LocalFree(nint memory);
}
