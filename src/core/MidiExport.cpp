#include "MidiExport.h"

#include "Arrangement.h"
#include "Engine.h"
#include "PcmBank.h"
#include "SongTrack.h"
#include "VgmSong.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace opm86
{

namespace
{

// Song time to MIDI ticks: one tick is one VGM sample, so every event keeps
// its recorded time, and a whole number of ticks of lead-in puts the song's
// first beat at the start of the second bar. A quarter note is the song's
// beat rounded to whole ticks; the bar lines drift from the song's beats by
// less than half a tick per beat.
struct Timeline
{
    uint32_t leadIn = 0;
    int ticksPerQuarter = 22050;

    uint32_t operator()(uint32_t vgmSamples) const { return vgmSamples + leadIn; }
};

Timeline ticks;

struct Event
{
    uint32_t tick;
    int order; // at one tick: note offs, then setup, then note ons
    std::vector<uint8_t> bytes;
};

class TrackWriter
{
public:
    void add(uint32_t tick, int order, std::vector<uint8_t> bytes)
    {
        events_.push_back({ tick, order, std::move(bytes) });
    }

    void meta(uint8_t type, const std::string& text)
    {
        std::vector<uint8_t> bytes { 0xFF, type };
        appendLength(bytes, static_cast<uint32_t>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
        add(0, -1, std::move(bytes));
    }

    // Append this track as an MTrk chunk.
    void write(std::vector<uint8_t>& out)
    {
        std::stable_sort(events_.begin(), events_.end(),
                         [](const Event& a, const Event& b) { return std::tie(a.tick, a.order) < std::tie(b.tick, b.order); });
        std::vector<uint8_t> body;
        uint32_t last = 0;
        for (const Event& e : events_)
        {
            appendLength(body, e.tick - last);
            body.insert(body.end(), e.bytes.begin(), e.bytes.end());
            last = e.tick;
        }
        body.insert(body.end(), { 0x00, 0xFF, 0x2F, 0x00 });

        out.insert(out.end(), { 'M', 'T', 'r', 'k' });
        appendBig(out, static_cast<uint32_t>(body.size()), 4);
        out.insert(out.end(), body.begin(), body.end());
    }

    static void appendLength(std::vector<uint8_t>& out, uint32_t value)
    {
        uint8_t bytes[5];
        int n = 0;
        do
        {
            bytes[n++] = value & 0x7F;
            value >>= 7;
        } while (value != 0);
        while (n > 0)
        {
            --n;
            out.push_back(static_cast<uint8_t>(bytes[n] | (n > 0 ? 0x80 : 0)));
        }
    }

    static void appendBig(std::vector<uint8_t>& out, uint32_t value, int bytes)
    {
        for (int i = bytes - 1; i >= 0; --i)
            out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }

private:
    std::vector<Event> events_;
};

} // namespace

MidiExport exportMidi(const VgmImport& song, const std::string& title)
{
    return exportMidi(arrangementFromVgm(song, title));
}

MidiExport exportMidi(const Arrangement& arrangement)
{
    MidiExport result;
    const std::string& title = arrangement.name;
    const ArrangementMidi midi = arrangementMidi(arrangement, false);
    result.skippedHits = midi.skippedHits;

    // A beat longer than MIDI allows (below 81 BPM) is written as two, and
    // a bar then has twice as many quarter notes.
    double beat = arrangement.beatLength;
    int quartersPerBar = std::clamp(arrangement.beatsPerBar, 1, 64);
    while (beat > 32767.0)
        beat /= 2.0, quartersPerBar *= 2;
    ticks.ticksPerQuarter = static_cast<int>(std::lround(beat));
    // The lead-in is a whole number of 20 ms steps (882 VGM samples): a whole
    // number of samples at 44.1 kHz, at 48 kHz and at both chips' rates, so
    // shifting the song by it moves every event by the same number of chip
    // samples. The first beat lands within 10 ms of the second bar.
    constexpr double kStep = 882.0;
    ticks.leadIn = static_cast<uint32_t>(
        kStep * std::max(1.0, std::round((quartersPerBar * ticks.ticksPerQuarter - arrangement.firstBeat) / kStep)));
    result.tempo = 60.0 * kVgmRate / ticks.ticksPerQuarter;
    result.leadIn = ticks.leadIn;

    TrackWriter tempo;
    tempo.meta(0x03, title);
    tempo.add(0, 0, { 0xFF, 0x58, 0x04, static_cast<uint8_t>(quartersPerBar), 2, 24, 8 }); // the meter, in quarter notes

    // One MIDI track per arrangement track, each starting with the
    // controllers that name the song and the instrument or drum kit.
    const uint32_t id = songId(title);
    std::vector<TrackWriter> writers(arrangement.tracks.size());
    for (size_t t = 0; t < arrangement.tracks.size(); ++t)
    {
        const ArrTrack& track = arrangement.tracks[t];
        TrackWriter& w = writers[t];
        const bool drums = track.kind == ArrTrack::Kind::Drum;
        w.meta(0x03, title + " " + track.name);
        for (int part = 0; part < 4; ++part)
            w.add(0, 0, { 0xB0, static_cast<uint8_t>(kSongIdController + part), static_cast<uint8_t>((id >> (21 - 7 * part)) & 0x7F) });
        if (drums)
            w.add(0, 0, { 0xB0, static_cast<uint8_t>(kKitSelectController), 1 });
        else
            w.add(0, 0, { 0xB0, static_cast<uint8_t>(kPatchSelectController), static_cast<uint8_t>(std::min(track.source + 1, 127)) });

        MidiExport::Track info;
        info.name = title + " " + track.name;
        info.patch = drums ? 0 : track.source + 1;
        info.notes = track.notes.size();
        for (int ch : midi.channels[t])
            info.channels.push_back(ch + 1);
        std::sort(info.channels.begin(), info.channels.end());
        result.tracks.push_back(info);
    }
    for (const ArrangementEvent& e : midi.setup)
    {
        // Gating belongs to every drum track; a bend range to its own track.
        for (size_t t = 0; t < arrangement.tracks.size(); ++t)
            if (static_cast<int>(t) == e.track || (e.track < 0 && arrangement.tracks[t].kind == ArrTrack::Kind::Drum))
                writers[t].add(0, 1, std::vector<uint8_t>(e.bytes, e.bytes + e.size));
    }
    for (const ArrangementEvent& e : midi.events)
        writers[static_cast<size_t>(e.track)].add(ticks(static_cast<uint32_t>(e.time)), e.order,
                                                  std::vector<uint8_t>(e.bytes, e.bytes + e.size));

    // Tracks without notes are left out.
    std::vector<TrackWriter> kept;
    std::vector<MidiExport::Track> keptInfo;
    for (size_t t = 0; t < writers.size(); ++t)
        if (!arrangement.tracks[t].notes.empty() && !midi.channels[t].empty())
        {
            kept.push_back(std::move(writers[t]));
            keptInfo.push_back(result.tracks[t]);
        }
    result.tracks = keptInfo;

    // Ticks run at exactly 44100 a second, but a MIDI tempo is a whole
    // number of microseconds per quarter note. Each quarter takes the nearer
    // of the two values that keep the elapsed time within a microsecond, so
    // the file does not drift from the song; the tempo changes only when needed.
    const double quarter = ticks.ticksPerQuarter / kVgmRate; // seconds
    const int beats = static_cast<int>(std::ceil((arrangement.length + 3.0 * kVgmRate + ticks.leadIn) / ticks.ticksPerQuarter)) + 1;
    uint32_t written = 0;
    for (int k = 0; k < beats; ++k)
    {
        const auto now = static_cast<int64_t>(std::llround(k * quarter * 1e6));
        const auto next = static_cast<int64_t>(std::llround((k + 1) * quarter * 1e6));
        const auto value = static_cast<uint32_t>(next - now);
        if (value != written)
        {
            tempo.add(static_cast<uint32_t>(k * ticks.ticksPerQuarter), 0,
                      { 0xFF, 0x51, 0x03, static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value) });
            written = value;
        }
    }

    std::vector<uint8_t>& out = result.file;
    out = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1 };
    TrackWriter::appendBig(out, static_cast<uint32_t>(kept.size() + 1), 2);
    TrackWriter::appendBig(out, static_cast<uint32_t>(ticks.ticksPerQuarter), 2);
    tempo.write(out);
    for (TrackWriter& track : kept)
        track.write(out);
    return result;
}

} // namespace opm86
