using Syna.Core.Engine;
using Syna.Core.State;
using Xunit;

namespace Syna.Core.Tests;

// A trim exports the tracks as the player's mixer plays them: these check the rule, the request it
// produces and what the sheet says about it.
public class MixerTests
{
    // Video is stream 0 and the two audio tracks streams 1 and 2, as in a ShadowPlay recording.
    private static MediaInfo Clip() => new()
    {
        DurationUs = 60_000_000,
        Video = new VideoStreamInfo { Codec = "h264", Width = 1920, Height = 1080, FpsAvg = 60, BitDepth = 8 },
        Audio =
        [
            new AudioTrackInfo { StreamIndex = 1, Codec = "aac", SampleRate = 48000, Channels = 2, Label = "System sounds" },
            new AudioTrackInfo { StreamIndex = 2, Codec = "aac", SampleRate = 48000, Channels = 2, Label = "Microphone" },
        ],
    };

    private static MixerState Mixer(double g1 = 1, double g2 = 1, bool m1 = false, bool m2 = false, bool s1 = false, bool s2 = false, int o1 = 0, int o2 = 0) =>
        new([g1, g2], [m1, m2], [s1, s2], [o1, o2]);

    private static TrimPlan Plan(MixerState? mixer, TrimAudio audio = TrimAudio.Mix, TrimProfile profile = TrimProfile.Original, string[]? names = null) =>
        TrimPlanner.Plan(new TrimPlanInput
        {
            Mode = TrimMode.Quick, Profile = profile, Arrangement = audio, Source = Clip(), InUs = 12_500_000, OutUs = 30_000_000,
            Keyframes = Enumerable.Range(0, 30).Select(k => k * 2_000_000L).ToArray(), Mixer = mixer, TrackNames = names ?? [],
        });

    [Fact]
    public void Mute_and_solo_follow_the_players_rule()
    {
        var m = Mixer(g1: 0.3, m2: true);
        Assert.Equal(0.3, m.EffectiveGain(0), 6);
        Assert.Equal(0, m.EffectiveGain(1));
        var solo = Mixer(g2: 0.5, s2: true);
        Assert.Equal(0, solo.EffectiveGain(0));  // silenced by the other track's solo
        Assert.Equal(0.5, solo.EffectiveGain(1), 6);
        var both = Mixer(m1: true, s1: true);
        Assert.Equal(0, both.EffectiveGain(0));  // muted wins over soloed,
        Assert.Equal(0, both.EffectiveGain(1));  // and the solo still silences the others
        Assert.Equal(1, Mixer().EffectiveGain(5));          // a track past the mixer plays as recorded,
        Assert.Equal(0, Mixer(s1: true).EffectiveGain(5));  // unless a solo silences it
    }

    [Fact]
    public void The_request_carries_the_mixer_by_stream_and_the_sheet_says_what_it_does()
    {
        var p = Plan(Mixer(g1: 0.3, m2: true));
        Assert.Equal(new[] { new TrimTrackMix(1, 0.3, 0), new TrimTrackMix(2, 0, 0) }, p.Output.Tracks);
        Assert.Contains("Mixer applied: System sounds at 30%, Microphone muted.", p.Notes);
        Assert.Equal("2 tracks mixed into one · AAC 320 kb/s (encoded)", p.Audio);

        var untouched = Plan(Mixer());
        Assert.All(untouched.Output.Tracks, t => Assert.False(t.Adjusted));
        Assert.DoesNotContain(untouched.Notes, n => n.StartsWith("Mixer"));
        Assert.Empty(Plan(null).Output.Tracks);  // no mixer: every track as recorded
    }

    [Fact]
    public void Separate_tracks_copy_what_the_mixer_leaves_alone()
    {
        Assert.Equal("2 tracks kept separate · copied without re-encoding", Plan(Mixer(), TrimAudio.Separate).Audio);

        var halfMic = Plan(Mixer(g2: 0.5), TrimAudio.Separate);
        Assert.Equal("2 tracks kept separate · 1 copied, 1 re-encoded as AAC 320 kb/s", halfMic.Audio);
        Assert.Equal((AudioCodec.Auto, 320), (halfMic.Output.AudioCodec, halfMic.Output.AudioBitrateKbps));  // Automatic, so the other track can be copied

        var delayed = Plan(Mixer(o2: 40), TrimAudio.Separate);
        Assert.Contains("Mixer applied: Microphone delayed 40 ms.", delayed.Notes);
        Assert.StartsWith("2 tracks kept separate · 1 copied, 1 re-encoded", delayed.Audio);

        var solo = Plan(Mixer(g2: 0.8, s2: true), TrimAudio.Separate, TrimProfile.Balanced);
        Assert.Contains("Mixer applied: System sounds silent, Microphone soloed at 80%.", solo.Notes);
        Assert.Equal("2 tracks kept separate · re-encoded as AAC 192 kb/s", solo.Audio);
    }

    [Fact]
    public void Names_are_only_text_and_the_plan_keeps_its_own_copy()
    {
        var state = Mixer(g1: 0.3, m2: true);
        var renamed = Plan(state, names: ["Game", "Voice"]);
        Assert.Equal(Plan(state).Output.Tracks, renamed.Output.Tracks);  // renaming does not move the mixer to another stream
        Assert.Contains("Mixer applied: Game at 30%, Voice muted.", renamed.Notes);
        state.Gain[0] = 1;  // the clip's mixer changes after the trim started
        state.Mute[1] = false;
        Assert.Equal(new[] { new TrimTrackMix(1, 0.3, 0), new TrimTrackMix(2, 0, 0) }, renamed.Output.Tracks);
    }

    [Fact]
    public void Two_clips_get_their_own_mixers()
    {
        var a = Plan(Mixer(m2: true)).Output.Tracks;
        var b = Plan(Mixer(g1: 0.5)).Output.Tracks;
        Assert.Equal(new[] { new TrimTrackMix(1, 1, 0), new TrimTrackMix(2, 0, 0) }, a);
        Assert.Equal(new[] { new TrimTrackMix(1, 0.5, 0), new TrimTrackMix(2, 1, 0) }, b);
    }

    [Fact]
    public void Reset_volume_returns_to_the_configured_default()
    {
        var s = new AppSettings();
        Assert.Equal(1.0, s.DefaultGain(0));
        s.DefaultTrackGains[1] = 0.7;
        Assert.Equal(0.7, s.DefaultGain(1));
        Assert.Equal(1.0, s.DefaultGain(12));  // no setting for that track
        s.DefaultTrackGains[0] = 0;
        Assert.Equal(1.0, s.DefaultGain(0));   // an unusable value falls back to 100 %
    }
}
