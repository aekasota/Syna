using Syna.Core.Services;
using Microsoft.Win32;
using Microsoft.Windows.AppLifecycle;

namespace Syna.Services;

/// <summary>
/// File associations and the default-app hand-off. The app registers per user through the Windows
/// App SDK (run by the installer, or from Settings). Choosing the default player is always left to
/// Windows Settings.
/// </summary>
public static class ShellIntegration
{
    public const string DisplayName = "Syna";

    public static string ExecutablePath => Environment.ProcessPath ?? "";

    /// <summary>True when this executable is registered as an "Open with" handler for every supported type.</summary>
    public static bool AreAssociationsRegistered() => RegisteredExtensions().Count == MediaFiles.Extensions.Length;

    /// <summary>The supported types that list this executable as an "Open with" handler.</summary>
    public static IReadOnlyList<string> RegisteredExtensions()
    {
        string exe = ExecutablePath;
        return MediaFiles.Extensions.Where(ext =>
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey($@"Software\Classes\{ext}\OpenWithProgids");
            return key?.GetValueNames().Any(progId => ProgIdTargets(progId, exe)) ?? false;
        }).ToList();
    }

    private static bool ProgIdTargets(string progId, string exe)
    {
        using RegistryKey? cmd = Registry.CurrentUser.OpenSubKey($@"Software\Classes\{progId}\shell\open\command");
        return cmd?.GetValue(null) is string command && command.Contains(exe, StringComparison.OrdinalIgnoreCase);
    }

    public static void RegisterAssociations()
    {
        string exe = ExecutablePath;
        ActivationRegistrationManager.RegisterForFileTypeActivation(MediaFiles.Extensions, $"{exe},0", DisplayName, ["open"], CommandPath(exe));
        if (RegisteredAppId() is { } appId) DescribeRegistration(appId, exe);
        Log.Info("shell", $"registered file associations for {exe}");
    }

    public static void UnregisterAssociations()
    {
        string? appId = RegisteredAppId();
        ActivationRegistrationManager.UnregisterForFileTypeActivation(MediaFiles.Extensions, CommandPath(ExecutablePath));
        if (appId != null) RemoveEmptyRegistration(appId);
        Log.Info("shell", "unregistered file associations");
    }

    // The Windows App SDK writes the executable into the shell command as given, so a path with
    // spaces must arrive quoted.
    private static string CommandPath(string exe) => exe.Contains(' ') ? $"\"{exe}\"" : exe;

    /// <summary>The name Windows lists this registration under (RegisteredApplications), or null.</summary>
    private static string? RegisteredAppId()
    {
        string exe = ExecutablePath;
        foreach (string ext in MediaFiles.Extensions)
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey($@"Software\Classes\{ext}\OpenWithProgids");
            string? progId = key?.GetValueNames().FirstOrDefault(p => ProgIdTargets(p, exe));
            if (progId != null) return progId.EndsWith(".File", StringComparison.Ordinal) ? progId[..^5] : progId;
        }
        return null;
    }

    private static string? CapabilitiesPath(string appId)
    {
        using RegistryKey? apps = Registry.CurrentUser.OpenSubKey(@"Software\RegisteredApplications");
        return apps?.GetValue(appId) as string;
    }

    // Default apps shows ApplicationName/Description from the capabilities key; the SDK leaves them empty.
    private static void DescribeRegistration(string appId, string exe)
    {
        if (CapabilitiesPath(appId) is not { } path) return;
        using RegistryKey? caps = Registry.CurrentUser.OpenSubKey(path, writable: true);
        if (caps == null) return;
        caps.SetValue("ApplicationName", DisplayName);
        caps.SetValue("ApplicationDescription", "Player and trimmer for local gameplay clips.");
        caps.SetValue("ApplicationIcon", $"{exe},0");
    }

    // The SDK removes the ProgID but keeps an empty capabilities key and its RegisteredApplications
    // entry, which would linger in Default apps. Remove both when nothing else is registered there.
    private static void RemoveEmptyRegistration(string appId)
    {
        const string sdkRoot = @"Software\Microsoft\WindowsAppRuntimeApplications\";
        if (CapabilitiesPath(appId) is not { } path || !path.StartsWith(sdkRoot + appId + "\\", StringComparison.OrdinalIgnoreCase)) return;
        using (RegistryKey? caps = Registry.CurrentUser.OpenSubKey(path))
        {
            foreach (string name in caps?.GetSubKeyNames() ?? [])
            {
                using RegistryKey? sub = caps!.OpenSubKey(name);
                if (sub?.ValueCount > 0) return;
            }
        }
        using (RegistryKey? apps = Registry.CurrentUser.OpenSubKey(@"Software\RegisteredApplications", writable: true))
            apps?.DeleteValue(appId, throwOnMissingValue: false);
        Registry.CurrentUser.DeleteSubKeyTree(sdkRoot + appId, throwOnMissingSubKey: false);
    }

    /// <summary>Opens Settings > Default apps, on this app's page when it is registered.</summary>
    public static async Task OpenDefaultAppsSettingsAsync()
    {
        string? query = RegisteredAppId() is { } appId ? "registeredAppUser=" + Uri.EscapeDataString(appId) : null;
        bool ok = query != null && await Windows.System.Launcher.LaunchUriAsync(new Uri("ms-settings:defaultapps?" + query));
        if (!ok) await Windows.System.Launcher.LaunchUriAsync(new Uri("ms-settings:defaultapps"));
    }
}
