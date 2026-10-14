using System.Runtime.InteropServices.WindowsRuntime;
using Syna.Core.Services;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace Syna.Controls;

/// <summary>
/// One audio lane. The cached envelope is rasterized into a single bitmap at physical-pixel width
/// (peaks as a translucent band, RMS as the solid core) on a dB scale so quiet speech stays visible.
/// Re-rendered only when size, zoom, data or theme change.
/// </summary>
public sealed partial class WaveformView : Grid
{
    private readonly Image _image = new() { Stretch = Microsoft.UI.Xaml.Media.Stretch.Fill };
    private WriteableBitmap? _bitmap;
    private byte[] _pixels = [];
    private float[] _peaks = [];
    private float[] _rms = [];
    private WaveformTrack? _track;
    private long _viewStart, _viewEnd = 1;
    private Color _color = Microsoft.UI.Colors.Gray;
    private bool _pending;

    public WaveformView()
    {
        Children.Add(_image);
        SizeChanged += (_, _) => Invalidate();
        IsHitTestVisible = false;
    }

    public void SetTrack(WaveformTrack? track, Color color)
    {
        _track = track;
        _color = color;
        Invalidate();
    }

    public void SetView(long startUs, long endUs)
    {
        _viewStart = startUs;
        _viewEnd = Math.Max(startUs + 1, endUs);
        Invalidate();
    }

    private void Invalidate()
    {
        if (_pending) return;
        _pending = true;
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, Render);
    }

    private static float ToDisplay(float v) => v <= 0.0001f ? 0 : Math.Clamp((20f * MathF.Log10(v) + 54f) / 54f, 0f, 1f);

    private void Render()
    {
        _pending = false;
        double scale = XamlRoot?.RasterizationScale ?? 1.0;
        int w = (int)Math.Round(ActualWidth * scale), h = (int)Math.Round(ActualHeight * scale);
        if (w <= 0 || h <= 0)
        {
            _image.Source = null;
            return;
        }
        if (_bitmap == null || _bitmap.PixelWidth != w || _bitmap.PixelHeight != h)
        {
            _bitmap = new WriteableBitmap(w, h);
            _pixels = new byte[w * h * 4];
            _image.Source = _bitmap;
        }
        Array.Clear(_pixels);
        if (_track != null)
        {
            if (_peaks.Length != w)
            {
                _peaks = new float[w];
                _rms = new float[w];
            }
            _track.Render(_viewStart, _viewEnd, _peaks, _rms);
            float mid = (h - 1) / 2f;
            for (int x = 0; x < w; x++)
            {
                float p = ToDisplay(_peaks[x]) * mid, r = ToDisplay(_rms[x]) * mid;
                int p0 = (int)MathF.Floor(mid - p), p1 = (int)MathF.Ceiling(mid + p);
                int r0 = (int)MathF.Floor(mid - r), r1 = (int)MathF.Ceiling(mid + r);
                for (int y = Math.Max(0, p0); y <= Math.Min(h - 1, p1); y++)
                {
                    bool core = y >= r0 && y <= r1;
                    Put(x, y, w, core ? (byte)230 : (byte)110);
                }
            }
        }
        using (var stream = _bitmap.PixelBuffer.AsStream())
        {
            stream.Write(_pixels, 0, _pixels.Length);
        }
        _bitmap.Invalidate();
    }

    // Premultiplied BGRA.
    private void Put(int x, int y, int w, byte alpha)
    {
        int i = (y * w + x) * 4;
        _pixels[i] = (byte)(_color.B * alpha / 255);
        _pixels[i + 1] = (byte)(_color.G * alpha / 255);
        _pixels[i + 2] = (byte)(_color.R * alpha / 255);
        _pixels[i + 3] = alpha;
    }
}
