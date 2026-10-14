namespace Syna.Core.State;

/// <summary>
/// A clip's mixer, per audio track in probe order: gain, mute, solo and timing offset. Saved with the
/// file, applied to playback, and captured when a trim starts so the export hears the tracks as the
/// player plays them.
/// </summary>
public sealed record MixerState(double[] Gain, bool[] Mute, bool[] Solo, int[] OffsetMs)
{
    /// <summary>
    /// The level a track plays at: 0 when it is muted or another track is soloed, its gain otherwise
    /// (the player's rule, audio/mixer.cpp Mixer::mix_block). A track past the arrays has no mixer
    /// controls and plays at 100 % unless a solo silences it.
    /// </summary>
    public double EffectiveGain(int track)
    {
        bool muted = track < Mute.Length && Mute[track];
        bool soloed = track < Solo.Length && Solo[track];
        if (muted || (Solo.Any(s => s) && !soloed)) return 0;
        return track < Gain.Length ? Math.Max(0, Gain[track]) : 1.0;
    }

    public int Offset(int track) => track < OffsetMs.Length ? OffsetMs[track] : 0;
}
