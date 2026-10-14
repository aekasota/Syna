using System.Text;

namespace Syna.Services;

// Managed side of the structured log (native writes its own file in the same folder).
public static class Log
{
    private static readonly object Gate = new();
    private static StreamWriter? _writer;

    public static void Initialize(string directory)
    {
        lock (Gate)
        {
            if (_writer != null) return;
            try
            {
                Directory.CreateDirectory(directory);
                foreach (var old in new DirectoryInfo(directory).GetFiles("app-*.log").OrderByDescending(f => f.LastWriteTimeUtc).Skip(9))
                    old.Delete();
                string file = Path.Combine(directory, $"app-{DateTime.Now:yyyyMMdd-HHmmss}-{Environment.ProcessId}.log");
                _writer = new StreamWriter(new FileStream(file, FileMode.Append, FileAccess.Write, FileShare.Read), Encoding.UTF8) { AutoFlush = true };
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }

    public static void Info(string component, string message) => Write("INFO ", component, message);
    public static void Warn(string component, string message) => Write("WARN ", component, message);
    public static void Error(string component, string message) => Write("ERROR", component, message);

    private static void Write(string level, string component, string message)
    {
        lock (Gate) _writer?.WriteLine($"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} {level} [{component}] {message}");
    }
}
