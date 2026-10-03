// Checks of song mode: a song track played straight through sounds like the
// song itself, and a rearranged one jumps to exact copies of its beats.

#include "SongTrack.h"
#include "TestVgm.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstdio>

using namespace opm86;

namespace
{

int failures = 0;
constexpr double kRate = 48000.0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        ++failures;
}

// Play every track with notes through its own player, as GarageBand plays
// each track through its own plugin, and mix them.
std::vector<float> play(const juce::MidiFile& midi, std::shared_ptr<const VgmSong> song,
                        std::shared_ptr<const SongSnapshots> snapshots, size_t total)
{
    std::vector<float> mix(total, 0.0f), left(total), right(total);
    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto& track = *midi.getTrack(t);
        SongTrackPlayer player;
        player.player().prepare(kRate);
        player.player().setSong(song, snapshots);
        size_t pos = 0;
        int i = 0;
        while (i < track.getNumEvents())
        {
            const auto at = std::min(total, static_cast<size_t>(std::lround(track.getEventPointer(i)->message.getTimeStamp() * kRate)));
            if (at > pos)
                player.player().render(&left[pos], &right[pos], static_cast<int>(at - pos));
            pos = std::max(pos, at);
            for (; i < track.getNumEvents()
                   && std::min(total, static_cast<size_t>(std::lround(track.getEventPointer(i)->message.getTimeStamp() * kRate))) <= at;
                 ++i)
                player.handleMidi(track.getEventPointer(i)->message.getRawData(), track.getEventPointer(i)->message.getRawDataSize());
            player.endOfEvents();
        }
        if (total > pos)
            player.player().render(&left[pos], &right[pos], static_cast<int>(total - pos));
        for (size_t k = 0; k < total; ++k)
            mix[k] += left[k] + right[k];
    }
    return mix;
}

juce::MidiFile read(const std::vector<uint8_t>& bytes)
{
    juce::MemoryInputStream in(bytes.data(), bytes.size(), false);
    juce::MidiFile midi;
    midi.readFrom(in, false);
    midi.convertTimestampTicksToSeconds();
    return midi;
}

// Difference between two spans, in dB below the second.
double difference(const std::vector<float>& a, size_t fromA, const std::vector<float>& b, size_t fromB, size_t n)
{
    double d = 0, p = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double x = a[fromA + i], y = b[fromB + i];
        d += (x - y) * (x - y);
        p += y * y;
    }
    return 10.0 * std::log10(d / p + 1e-30);
}

} // namespace

int main()
{
    const std::vector<uint8_t> vgm = test::makeSong();
    std::string error;
    const auto song = loadVgmSong(vgm, "Test Song", error);
    check(song != nullptr && error.empty(), "the test song loads");
    if (!song)
        return 1;
    std::printf("      tempo %.2f BPM, first beat %.4f s, %d beats\n", song->tempo(), song->firstBeat / kVgmRate, song->beatCount);
    check(std::abs(song->tempo() - 120.0) < 0.5, "the beat grid finds the song's 120 BPM");
    const std::shared_ptr<const SongSnapshots> snapshots = makeSnapshots(*song);

    // Straight through, one track per stem, against the whole song played
    // by one player from beat -1 at the time the track's first note starts.
    const SongTrackFile file = writeSongTrack(*song, true);
    check(file.stems.size() == 3, "the song track has the two FM stems and the drums");
    const juce::MidiFile straight = read(file.file);
    const double beatSeconds = 60.0 / file.tempo;
    const auto start = static_cast<size_t>(std::lround(3.0 * beatSeconds * kRate));
    const auto length = static_cast<size_t>((song->length / kVgmRate + 1.0) * kRate);
    const auto out = play(straight, song, snapshots, start + length);

    SongPlayer whole;
    whole.prepare(kRate);
    whole.setSong(song, snapshots);
    whole.play(song->beatTime(-1));
    std::vector<float> wl(length), wr(length), ref(length);
    whole.render(wl.data(), wr.data(), static_cast<int>(length));
    for (size_t i = 0; i < length; ++i)
        ref[i] = wl[i] + wr[i];
    // The stems differ from the whole only by the rounding of the YM2151's
    // output converter, which each stem goes through on its own.
    const double straightDb = difference(out, start, ref, 0, length);
    std::printf("      straight through, stems mixed: %.1f dB from the whole song\n", straightDb);
    check(straightDb < -50.0, "a song track played straight through matches the song");

    // Rearranged: the first four beats, then the same four again.
    juce::MidiMessageSequence beats;
    beats.addEvent(juce::MidiMessage::controllerEvent(1, kStemController, 0), 0.0);
    double t = 0.0;
    for (int beat : { -1, 0, 1, 2, 3, 0, 1, 2, 3 })
    {
        uint8_t note = 0, velocity = 1;
        beatToNote(beat, note, velocity);
        beats.addEvent(juce::MidiMessage::noteOn(1, note, velocity), t);
        beats.addEvent(juce::MidiMessage::noteOff(1, note), t + beatSeconds);
        t += beatSeconds;
    }
    beats.updateMatchedPairs();
    juce::MidiFile rearranged;
    rearranged.setTicksPerQuarterNote(960);
    rearranged.addTrack(beats);
    // Times are already in seconds here: one tick per second keeps them.
    const auto again = play(rearranged, song, snapshots, static_cast<size_t>(t * kRate) + 4800);
    // The jump back must play beat 0 on exactly as a start at beat 0 does.
    // (Played straight, a beat may run a fraction of a sample off the grid,
    // as the host's notes fall on whole samples; a jump lands on it.)
    SongPlayer fresh;
    fresh.prepare(kRate);
    fresh.setSong(song, snapshots);
    fresh.play(song->beatTime(0));
    const auto span = static_cast<size_t>(4.0 * beatSeconds * kRate);
    std::vector<float> fl(span), fr(span), freshMix(span);
    fresh.render(fl.data(), fr.data(), static_cast<int>(span));
    for (size_t i = 0; i < span; ++i)
        freshMix[i] = fl[i] + fr[i];
    const auto repeat = static_cast<size_t>(std::lround(5.0 * beatSeconds * kRate));
    const auto settle = static_cast<size_t>(0.02 * kRate);
    const double repeatDb = difference(again, repeat + settle, freshMix, settle, span - settle);
    std::printf("      jump back to beat 0: %.1f dB from a start at beat 0\n", repeatDb);
    check(repeatDb < -100.0, "a jump plays its beats exactly, 20 ms after the crossfade");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "some checks failed");
    return failures == 0 ? 0 : 1;
}
