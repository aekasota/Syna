using System.Buffers.Binary;

namespace Syna.Core.Services;

/// <summary>Peak/RMS envelopes produced by the engine (layout: media/waveform.h).</summary>
public sealed class WaveformData
{
    private WaveformData(int bucketsPerSecond, long durationUs, WaveformTrack[] tracks)
    {
        BucketsPerSecond = bucketsPerSecond;
        DurationUs = durationUs;
        Tracks = tracks;
    }

    public int BucketsPerSecond { get; }
    public long DurationUs { get; }
    public IReadOnlyList<WaveformTrack> Tracks { get; }

    public static WaveformData? Load(string path)
    {
        try
        {
            return Parse(File.ReadAllBytes(path));
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static WaveformData? Parse(ReadOnlySpan<byte> data)
    {
        if (data.Length < 24 || !data[..4].SequenceEqual("SYNW"u8)) return null;
        uint version = BinaryPrimitives.ReadUInt32LittleEndian(data[4..]);
        if (version != 1) return null;
        int count = (int)BinaryPrimitives.ReadUInt32LittleEndian(data[8..]);
        int bps = (int)BinaryPrimitives.ReadUInt32LittleEndian(data[12..]);
        long duration = BinaryPrimitives.ReadInt64LittleEndian(data[16..]);
        if (count is < 0 or > 16 || bps <= 0) return null;
        int p = 24;
        var tracks = new WaveformTrack[count];
        for (int t = 0; t < count; t++)
        {
            if (p + 4 > data.Length) return null;
            int n = (int)BinaryPrimitives.ReadUInt32LittleEndian(data[p..]);
            p += 4;
            if (n < 0 || p + (long)n * 4 > data.Length) return null;
            var peak = new ushort[n];
            var rms = new ushort[n];
            for (int i = 0; i < n; i++, p += 4)
            {
                peak[i] = BinaryPrimitives.ReadUInt16LittleEndian(data[p..]);
                rms[i] = BinaryPrimitives.ReadUInt16LittleEndian(data[(p + 2)..]);
            }
            tracks[t] = new WaveformTrack(peak, rms, bps);
        }
        return new WaveformData(bps, duration, tracks);
    }
}

public sealed class WaveformTrack(ushort[] peak, ushort[] rms, int bucketsPerSecond)
{
    /// <summary>
    /// Reduces the buckets covering [startUs, endUs) to one column per element of
    /// <paramref name="peaks"/> (max per column), normalized to 0..1. Columns between buckets
    /// (zoomed far in) reuse the nearest bucket.
    /// </summary>
    public void Render(long startUs, long endUs, Span<float> peaks, Span<float> rmsOut)
    {
        int columns = peaks.Length;
        if (columns == 0 || endUs <= startUs) return;
        double bucketsPerUs = bucketsPerSecond / 1e6;
        double span = (endUs - startUs) * bucketsPerUs;
        for (int c = 0; c < columns; c++)
        {
            double b0 = startUs * bucketsPerUs + span * c / columns;
            double b1 = startUs * bucketsPerUs + span * (c + 1) / columns;
            int i0 = (int)Math.Floor(b0), i1 = Math.Max(i0 + 1, (int)Math.Ceiling(b1));
            ushort mp = 0, mr = 0;
            for (int i = Math.Max(0, i0); i < Math.Min(i1, peak.Length); i++)
            {
                if (peak[i] > mp) mp = peak[i];
                if (rms[i] > mr) mr = rms[i];
            }
            peaks[c] = mp / 65535f;
            rmsOut[c] = mr / 65535f;
        }
    }
}
