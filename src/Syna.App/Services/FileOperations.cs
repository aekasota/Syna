using System.Diagnostics;
using System.Runtime.InteropServices;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Streams;

namespace Syna.Services;

public static partial class FileOperations
{
    /// <summary>Opens File Explorer with the file selected.</summary>
    public static void Reveal(string path)
    {
        nint pidl = ILCreateFromPathW(path);
        if (pidl == 0)
        {
            string? dir = Path.GetDirectoryName(path);
            if (dir != null && Directory.Exists(dir)) Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true });
            return;
        }
        try { SHOpenFolderAndSelectItems(pidl, 0, 0, 0); }
        finally { ILFree(pidl); }
    }

    public static void CopyText(string text)
    {
        var package = new DataPackage();
        package.SetText(text);
        Clipboard.SetContent(package);
        Clipboard.Flush();
    }

    public static async Task CopyImageFileAsync(string path)
    {
        StorageFile file = await StorageFile.GetFileFromPathAsync(path);
        var package = new DataPackage();
        package.SetBitmap(RandomAccessStreamReference.CreateFromFile(file));
        package.SetStorageItems([file]);
        Clipboard.SetContent(package);
        Clipboard.Flush();
    }

    /// <summary>Moves the file to the Recycle Bin (never a permanent delete).</summary>
    public static bool Recycle(string path, out string? error)
    {
        var op = new SHFILEOPSTRUCTW
        {
            wFunc = FO_DELETE,
            pFrom = path + "\0\0",
            fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING,
        };
        int r = SHFileOperationW(ref op);
        error = r == 0 && !op.fAnyOperationsAborted ? null : $"The file could not be moved to the Recycle Bin (code {r}).";
        return error == null;
    }

    /// <summary>Renames within the same folder; refuses to overwrite.</summary>
    public static string Rename(string path, string newName)
    {
        newName = newName.Trim();
        if (newName.Length == 0 || newName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new IOException("The name contains characters Windows does not allow.");
        if (Path.GetExtension(newName).Length == 0) newName += Path.GetExtension(path);
        string target = Path.Combine(Path.GetDirectoryName(path)!, newName);
        if (string.Equals(target, path, StringComparison.Ordinal)) return path;
        if (File.Exists(target) && !string.Equals(target, path, StringComparison.OrdinalIgnoreCase))
            throw new IOException("A file with that name already exists.");
        File.Move(path, target);
        return target;
    }

    public static void OpenWith(string executable, string path)
    {
        Process.Start(new ProcessStartInfo(executable) { ArgumentList = { path }, UseShellExecute = false });
    }

    private const uint FO_DELETE = 3;
    private const ushort FOF_SILENT = 0x0004, FOF_NOCONFIRMATION = 0x0010, FOF_ALLOWUNDO = 0x0040, FOF_NOERRORUI = 0x0400, FOF_WANTNUKEWARNING = 0x4000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct SHFILEOPSTRUCTW
    {
        public nint hwnd;
        public uint wFunc;
        public string pFrom;
        public string? pTo;
        public ushort fFlags;
        [MarshalAs(UnmanagedType.Bool)] public bool fAnyOperationsAborted;
        public nint hNameMappings;
        public string? lpszProgressTitle;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHFileOperationW(ref SHFILEOPSTRUCTW op);

    [LibraryImport("shell32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint ILCreateFromPathW(string path);

    [LibraryImport("shell32.dll")]
    private static partial void ILFree(nint pidl);

    [LibraryImport("shell32.dll")]
    private static partial int SHOpenFolderAndSelectItems(nint pidlFolder, uint count, nint children, uint flags);
}
