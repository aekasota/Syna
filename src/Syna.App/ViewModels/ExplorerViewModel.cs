using System.Collections.ObjectModel;
using Syna.Core.Services;
using Syna.Core.State;
using Syna.Services;
using Microsoft.UI.Dispatching;

namespace Syna.ViewModels;

public enum ExplorerMode { Folder, Recent, Starred }

/// <summary>Clipboard format for dragging explorer items onto the player (newline-separated paths).</summary>
public static class ExplorerDrag
{
    public const string Format = "Syna.Paths";
}

public sealed class ExplorerItem : ObservableObject
{
    private string _meta = "";
    private bool _isNew;
    private bool _isFavorite;

    public required string Name { get; init; }
    public required string Path { get; init; }
    public bool IsFolder { get; init; }
    public DateTime Modified { get; init; }
    public long Size { get; init; }
    public bool IsMedia => !IsFolder;

    // Accessible name of the list row (ListViewItem falls back to ToString()).
    public override string ToString() => Name;

    public string Meta
    {
        get => _meta;
        set => Set(ref _meta, value);
    }

    public bool IsNew
    {
        get => _isNew;
        set => Set(ref _isNew, value);
    }

    public bool IsFavorite
    {
        get => _isFavorite;
        set => Set(ref _isFavorite, value);
    }

    public string Tooltip => Path;
}

public sealed class ExplorerViewModel(AppDatabase db, MediaIndex index, DispatcherQueue ui) : ObservableObject
{
    private readonly Stack<string> _back = new();
    private readonly Stack<string> _forward = new();
    private List<ExplorerItem> _all = [];
    private ObservableCollection<ExplorerItem> _items = [];
    private string _folder = "";
    private string _filter = "";
    private ExplorerMode _mode = ExplorerMode.Folder;
    private bool _loading;
    private string? _message;
    private int _generation;
    private readonly HashSet<string> _newClips = new(StringComparer.OrdinalIgnoreCase);

    public ObservableCollection<ExplorerItem> Items
    {
        get => _items;
        private set => Set(ref _items, value);
    }

    public string CurrentFolder
    {
        get => _folder;
        private set
        {
            if (Set(ref _folder, value)) Raise(nameof(Title));
        }
    }

    public string Title => _mode switch
    {
        ExplorerMode.Recent => "Recent clips",
        ExplorerMode.Starred => "Starred",
        _ => _folder.Length == 0 ? "This PC" : (System.IO.Path.GetFileName(_folder.TrimEnd('\\')) is { Length: > 0 } n ? n : _folder),
    };

    public ExplorerMode Mode
    {
        get => _mode;
        private set
        {
            if (Set(ref _mode, value))
            {
                Raise(nameof(Title));
                Raise(nameof(IsFolderMode));
            }
        }
    }

    public bool IsFolderMode => _mode == ExplorerMode.Folder;

    public string Filter
    {
        get => _filter;
        set
        {
            if (Set(ref _filter, value)) ApplyFilter();
        }
    }

    public bool IsLoading
    {
        get => _loading;
        private set => Set(ref _loading, value);
    }

    /// <summary>Empty/error state text for the list area, null when items are shown.</summary>
    public string? Message
    {
        get => _message;
        private set
        {
            if (Set(ref _message, value)) Raise(nameof(HasMessage));
        }
    }

    public bool HasMessage => _message != null;
    public bool CanGoBack => _back.Count > 0;
    public bool CanGoForward => _forward.Count > 0;
    public bool CanGoUp => _mode == ExplorerMode.Folder && _folder.Length > 0;

    public void MarkNew(string path)
    {
        _newClips.Add(path);
        foreach (var item in _all)
            if (string.Equals(item.Path, path, StringComparison.OrdinalIgnoreCase)) item.IsNew = true;
    }

    public async Task NavigateAsync(string folder, bool record = true)
    {
        if (record && (_mode != ExplorerMode.Folder || !string.Equals(folder, _folder, StringComparison.OrdinalIgnoreCase)))
        {
            if (_mode == ExplorerMode.Folder && _folder.Length > 0) _back.Push(_folder);
            _forward.Clear();
        }
        Mode = ExplorerMode.Folder;
        CurrentFolder = folder;
        RaiseNavigation();
        int generation = ++_generation;
        IsLoading = true;
        Message = null;
        try
        {
            var (items, error) = await Task.Run(() => Enumerate(folder));
            if (generation != _generation) return;
            _all = items;
            Message = error ?? (items.Count == 0 ? "No folders or clips here." : null);
            ApplyFilter();
            RequestMetadata(items);
        }
        finally
        {
            if (generation == _generation) IsLoading = false;
        }
    }

    private (List<ExplorerItem> Items, string? Error) Enumerate(string folder)
    {
        var result = new List<ExplorerItem>();
        if (folder.Length == 0)
        {
            foreach (var drive in DriveInfo.GetDrives().Where(d => d.IsReady))
                result.Add(new ExplorerItem { Name = $"{drive.VolumeLabel} ({drive.Name.TrimEnd('\\')})".Trim(), Path = drive.RootDirectory.FullName, IsFolder = true });
            return (result, null);
        }
        try
        {
            var options = new EnumerationOptions { IgnoreInaccessible = true, AttributesToSkip = FileAttributes.Hidden | FileAttributes.System };
            var dir = new DirectoryInfo(folder);
            var folders = new List<ExplorerItem>();
            var files = new List<ExplorerItem>();
            foreach (FileSystemInfo entry in dir.EnumerateFileSystemInfos("*", options))
            {
                if (entry is DirectoryInfo d)
                    folders.Add(new ExplorerItem { Name = d.Name, Path = d.FullName, IsFolder = true, Modified = d.LastWriteTime });
                else if (entry is FileInfo f && MediaFiles.IsSupported(f.Name))
                    files.Add(new ExplorerItem { Name = f.Name, Path = f.FullName, Modified = f.LastWriteTime, Size = f.Length, IsNew = _newClips.Contains(f.FullName) });
            }
            folders.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.CurrentCultureIgnoreCase));
            files.Sort((a, b) => b.Modified.CompareTo(a.Modified));
            var favorites = db.Favorites().Select(f => f.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
            foreach (var f in files) f.IsFavorite = favorites.Contains(f.Path);
            result.AddRange(folders);
            result.AddRange(files);
            return (result, null);
        }
        catch (UnauthorizedAccessException)
        {
            return (result, "Access to this folder is denied.");
        }
        catch (IOException ex)
        {
            return (result, ex is DirectoryNotFoundException ? "This folder no longer exists." : ex.Message);
        }
    }

    public async Task ShowListAsync(ExplorerMode mode)
    {
        Mode = mode;
        RaiseNavigation();
        int generation = ++_generation;
        var records = await Task.Run(() => mode == ExplorerMode.Recent ? db.RecentFiles(200) : db.Favorites());
        if (generation != _generation) return;
        _all = records
            .Where(r => File.Exists(r.Path))
            .Select(r => new ExplorerItem { Name = System.IO.Path.GetFileName(r.Path), Path = r.Path, IsFavorite = r.Favorite, Modified = DateTimeOffset.FromUnixTimeMilliseconds(r.LastOpenedUnixMs).LocalDateTime })
            .ToList();
        Message = _all.Count == 0 ? (mode == ExplorerMode.Recent ? "Clips you open appear here." : "Star clips to keep them here.") : null;
        ApplyFilter();
        RequestMetadata(_all);
    }

    public void Refresh()
    {
        if (_mode == ExplorerMode.Folder) _ = NavigateAsync(_folder, record: false);
        else _ = ShowListAsync(_mode);
    }

    public void GoBack()
    {
        if (_back.Count == 0) return;
        _forward.Push(_folder);
        _ = NavigateAsync(_back.Pop(), record: false);
    }

    public void GoForward()
    {
        if (_forward.Count == 0) return;
        _back.Push(_folder);
        _ = NavigateAsync(_forward.Pop(), record: false);
    }

    public void GoUp()
    {
        if (!CanGoUp) return;
        string? parent = Directory.GetParent(_folder.TrimEnd('\\'))?.FullName;
        if (parent != null && _folder.TrimEnd('\\').Length > 2) _ = NavigateAsync(parent);
        else _ = NavigateAsync("");
    }

    private void ApplyFilter()
    {
        string f = _filter.Trim();
        Items = new ObservableCollection<ExplorerItem>(f.Length == 0 ? _all : _all.Where(i => i.Name.Contains(f, StringComparison.CurrentCultureIgnoreCase)));
    }

    private void RequestMetadata(IEnumerable<ExplorerItem> items)
    {
        // Pushed in reverse so the LIFO worker starts at the top of the list.
        foreach (var item in items.Where(i => i.IsMedia).Reverse())
        {
            var target = item;
            index.Request(item.Path, s => ui.TryEnqueue(() =>
            {
                string size = s.Width > 0 ? $"{s.Width}×{s.Height}" : "Audio";
                string fps = s.Fps > 0 ? $" · {s.Fps:0.##} fps" : "";
                target.Meta = $"{size}{fps} · {Timecode.Short(s.DurationUs)}";
            }));
        }
    }

    private void RaiseNavigation()
    {
        Raise(nameof(CanGoBack));
        Raise(nameof(CanGoForward));
        Raise(nameof(CanGoUp));
        Raise(nameof(Title));
    }
}
