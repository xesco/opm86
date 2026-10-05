#include "Arrangement.h"

#include "Engine.h"
#include "VgmSong.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace opm86
{

namespace
{

constexpr int kDrumChannel = 9;

// The synth plays a pad at level x volume / 127, and pads start at level
// 100, so volume = chip volume x 1.27 gives the game's level.
uint8_t volumeValue(int left, int right)
{
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(std::max(left, right) * 1.27)), 0, 127));
}

// The pan, as CC 10, that turns one volume into this left and right pair:
// the quieter side is scaled by its ratio to the louder one.
uint8_t panValue(int left, int right)
{
    double pan = 0.0;
    if (left > right)
        pan = static_cast<double>(right) / left - 1.0;
    else if (right > left)
        pan = 1.0 - static_cast<double>(left) / right;
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(64.0 + pan * 63.0)), 1, 127));
}

// CC 10 for a YM2151 output: left, both or right.
uint8_t panValue(FmPan pan)
{
    return pan == FmPan::Left ? 0 : pan == FmPan::Right ? 127 : 64;
}

class Emitter
{
public:
    explicit Emitter(ArrangementMidi& out) : out_(out) {}

    void add(int64_t time, int order, int track, uint8_t a, uint8_t b, uint8_t c)
    {
        out_.events.push_back({ time, order, track, { a, b, c }, 3 });
    }
    void add2(int64_t time, int order, int track, uint8_t a, uint8_t b)
    {
        out_.events.push_back({ time, order, track, { a, b, 0 }, 2 });
    }
    void bend(int64_t time, int order, int track, int channel, double semitones)
    {
        const int range = out_.bendRange;
        const int value = std::clamp(static_cast<int>(std::lround(8192.0 + semitones / range * 8192.0)), 0, 16383);
        add(time, order, track, static_cast<uint8_t>(0xE0 | channel), static_cast<uint8_t>(value & 0x7F),
            static_cast<uint8_t>(value >> 7));
    }

private:
    ArrangementMidi& out_;
};

// RPN 0, the pitch bend range of a channel, then RPN deselected.
void bendRange(std::vector<ArrangementEvent>& setup, int track, int channel, int range)
{
    const auto cc = static_cast<uint8_t>(0xB0 | channel);
    for (auto [number, value] : { std::pair<int, int> { 101, 0 }, { 100, 0 }, { 6, range }, { 38, 0 }, { 101, 127 }, { 100, 127 } })
        setup.push_back({ 0, 0, track, { cc, static_cast<uint8_t>(number), static_cast<uint8_t>(value) }, 3 });
}

std::string shortName(const std::string& name, const std::string& song)
{
    return name.rfind(song + " ", 0) == 0 ? name.substr(song.size() + 1) : name;
}

} // namespace

bool Arrangement::audible(const ArrTrack& track) const
{
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const ArrTrack& t) { return t.solo; });
    return anySolo ? track.solo : !track.mute;
}

Arrangement arrangementFromVgm(const VgmImport& song, const std::string& name)
{
    Arrangement a;
    a.name = name;
    a.patches = song.patches;
    a.samples = song.samples;
    a.length = song.length;

    std::vector<uint32_t> starts;
    for (const VgmNote& note : song.notes)
        starts.push_back(note.start);
    for (const VgmHit& hit : song.hits)
        starts.push_back(hit.start);
    const BeatGrid grid = fitBeatGrid(starts);
    a.beatLength = grid.length;
    a.firstBeat = grid.first;

    // Patches and samples are already most used first.
    std::map<int, size_t> fmTrack, drumTrack;
    for (size_t p = 0; p < song.patches.size(); ++p)
    {
        ArrTrack t;
        t.kind = ArrTrack::Kind::Fm;
        t.source = static_cast<int>(p);
        t.name = shortName(song.patches[p].name, name);
        fmTrack[static_cast<int>(p)] = a.tracks.size();
        a.tracks.push_back(t);
    }
    for (size_t s = 0; s < song.samples.size(); ++s)
    {
        ArrTrack t;
        t.kind = ArrTrack::Kind::Drum;
        t.source = static_cast<int>(s);
        t.name = "Drum " + std::to_string(s + 1);
        drumTrack[static_cast<int>(s)] = a.tracks.size();
        a.tracks.push_back(t);
    }

    for (const VgmNote& vn : song.notes)
    {
        ArrNote n;
        n.start = vn.start;
        n.length = std::max<int64_t>(1, static_cast<int64_t>(vn.end) - vn.start);
        n.voice = vn.channel;
        const double first = vn.pitch.front().second;
        n.pitch = std::clamp(static_cast<int>(std::lround(first)), 0, 127);
        n.bend.clear();
        for (const auto& [time, pitch] : vn.pitch)
            n.bend.emplace_back(static_cast<int64_t>(time) - vn.start, pitch - n.pitch);
        n.changes.clear();
        for (const VgmNoteState& s : vn.state)
            n.changes.push_back({ static_cast<int64_t>(s.time) - vn.start, s.patch, s.attenuation, s.pan });
        a.tracks[fmTrack[vn.patch]].notes.push_back(std::move(n));
    }

    for (const VgmHit& vh : song.hits)
    {
        ArrNote n;
        n.start = vh.start;
        n.length = std::max<int64_t>(1, static_cast<int64_t>(vh.end) - vh.start);
        n.pitch = 60;
        n.bend.clear();
        n.changes.clear();
        for (const VgmHitState& s : vh.state)
            if (s.time < vh.end || n.hit.empty())
                n.hit.push_back({ static_cast<int64_t>(s.time) - vh.start, s.left, s.right, s.step });
        a.tracks[drumTrack[vh.sample]].notes.push_back(std::move(n));
    }

    // Tracks that no note uses (a patch seen only in a release) are dropped.
    a.tracks.erase(std::remove_if(a.tracks.begin(), a.tracks.end(), [](const ArrTrack& t) { return t.notes.empty(); }),
                   a.tracks.end());
    return a;
}

PadSet arrangementPads(const Arrangement& arrangement)
{
    PadSet pads = makeDefaultKit();
    for (size_t i = 0; i < pads.size() && i < arrangement.samples.size(); ++i)
        pads[i] = arrangement.samples[i];
    return pads;
}

ArrangementMidi arrangementMidi(const Arrangement& a, bool audibleOnly)
{
    ArrangementMidi out;
    out.channels.resize(a.tracks.size());
    Emitter emit(out);

    // The bend range covers the widest slide of a held note, so no note
    // restarts: at least 24 semitones, at most 96.
    double reach = 24.0;
    for (const ArrTrack& t : a.tracks)
        for (const ArrNote& n : t.notes)
        {
            for (const auto& [offset, semitones] : n.bend)
                reach = std::max(reach, std::abs(semitones));
            if (t.kind == ArrTrack::Kind::Drum && t.source < static_cast<int>(a.samples.size()))
                for (const HitChange& h : n.hit)
                    reach = std::max(reach, std::abs(12.0 * std::log2(std::max<uint8_t>(h.step, 1)
                                                                      / a.samples[static_cast<size_t>(t.source)].step)
                                                     + (n.pitch - 60) + n.fine));
        }
    out.bendRange = std::min(96, static_cast<int>(std::ceil(reach)));

    // Each FM note gets a MIDI channel of its own while it sounds: the game's
    // channel when free, so an untouched song plays as the game does; else
    // the channel free longest. Channel 10 is the drums'.
    struct Ref
    {
        size_t track, note;
    };
    std::vector<Ref> fm;
    for (size_t t = 0; t < a.tracks.size(); ++t)
        if (a.tracks[t].kind == ArrTrack::Kind::Fm && (!audibleOnly || a.audible(a.tracks[t])))
            for (size_t n = 0; n < a.tracks[t].notes.size(); ++n)
                fm.push_back({ t, n });
    std::stable_sort(fm.begin(), fm.end(), [&](const Ref& x, const Ref& y) {
        return a.tracks[x.track].notes[x.note].start < a.tracks[y.track].notes[y.note].start;
    });
    std::array<int64_t, 16> freeAt {};
    freeAt.fill(std::numeric_limits<int64_t>::min());
    freeAt[kDrumChannel] = std::numeric_limits<int64_t>::max();
    std::vector<int> channelOf(fm.size());
    for (size_t i = 0; i < fm.size(); ++i)
    {
        const ArrNote& n = a.tracks[fm[i].track].notes[fm[i].note];
        int ch = n.voice;
        if (ch < 0 || ch >= 16 || ch == kDrumChannel || freeAt[static_cast<size_t>(ch)] > n.start)
        {
            ch = -1;
            for (int c = 0; c < 16; ++c)
                if (c != kDrumChannel && freeAt[static_cast<size_t>(c)] <= n.start
                    && (ch < 0 || freeAt[static_cast<size_t>(c)] < freeAt[static_cast<size_t>(ch)]))
                    ch = c;
            if (ch < 0) // more than 15 notes at once: share the game's channel
                ch = n.voice >= 0 && n.voice < 16 && n.voice != kDrumChannel ? n.voice : 0;
        }
        channelOf[i] = ch;
        freeAt[static_cast<size_t>(ch)] = n.end();
    }

    // A note's settings run on into its release until the next note on its channel.
    std::array<std::vector<int64_t>, 16> channelStarts;
    for (size_t i = 0; i < fm.size(); ++i)
        channelStarts[static_cast<size_t>(channelOf[i])].push_back(a.tracks[fm[i].track].notes[fm[i].note].start);

    for (size_t i = 0; i < fm.size(); ++i)
    {
        const int track = static_cast<int>(fm[i].track);
        const ArrNote& n = a.tracks[fm[i].track].notes[fm[i].note];
        const int ch = channelOf[i];
        auto& used = out.channels[fm[i].track];
        if (std::find(used.begin(), used.end(), ch) == used.end())
            used.push_back(ch);

        const auto cc = static_cast<uint8_t>(0xB0 | ch);
        const auto pc = static_cast<uint8_t>(0xC0 | ch);
        const NoteChange& first = n.changes.front();
        // Up to 31 level steps (23 dB) go in the velocity, so the level moves
        // with the note; at the synth's default sensitivity a velocity of
        // 127 - 3.97 x steps is that many steps quieter. CC 11 holds the rest.
        const int inVelocity = std::min(first.attenuation, 31);
        const auto velocity = static_cast<uint8_t>(std::lround(127.0 - inVelocity * 127.0 / 32.0));
        auto level = [&](int attenuation) { return static_cast<uint8_t>(127 - std::clamp(attenuation - inVelocity, 0, 127)); };

        // Every note sets its program, output, level and bend, whatever the
        // last note on its channel left behind.
        emit.add2(n.start, 1, track, pc, static_cast<uint8_t>(std::min(first.patch, 127)));
        emit.add(n.start, 1, track, cc, 10, panValue(first.pan));
        emit.add(n.start, 1, track, cc, 11, level(first.attenuation));

        // The note, split where its pitch leaves the bend range.
        int key = -1;
        int64_t keyStart = n.start;
        double current = std::nan("");
        for (const auto& [offset, semitones] : n.bend)
        {
            if (offset >= n.length && key >= 0)
                break;
            const int64_t time = n.start + offset;
            const double pitch = n.pitch + semitones;
            if (key < 0 || std::abs(pitch - key) > out.bendRange)
            {
                if (key >= 0)
                    emit.add(time, 0, track, static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(key), 0);
                key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
                keyStart = time;
                current = pitch - key;
                emit.bend(time, 1, track, ch, current);
                emit.add(time, 2, track, static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(key), velocity);
            }
            else if (std::abs(pitch - key - current) > 1e-4)
            {
                current = pitch - key;
                emit.bend(time, 1, track, ch, current);
            }
        }
        emit.add(std::max(n.end(), keyStart + 1), 0, track, static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(key), 0);

        const auto& list = channelStarts[static_cast<size_t>(ch)];
        const auto next = std::upper_bound(list.begin(), list.end(), n.start);
        const int64_t limit = next == list.end() ? std::numeric_limits<int64_t>::max() : *next;
        for (size_t c = 1; c < n.changes.size(); ++c)
        {
            const NoteChange& was = n.changes[c - 1];
            const NoteChange& now = n.changes[c];
            const int64_t time = n.start + std::max<int64_t>(0, now.offset);
            if (time >= limit)
                break;
            if (now.patch != was.patch)
                emit.add2(time, 1, track, pc, static_cast<uint8_t>(std::min(now.patch, 127)));
            if (now.pan != was.pan)
                emit.add(time, 1, track, cc, 10, panValue(now.pan));
            if (now.attenuation != was.attenuation)
                emit.add(time, 1, track, cc, 11, level(now.attenuation));
        }
    }

    // Drums: one track per sound, all on MIDI channel 10, pad 36 + sound.
    bool anyDrums = false;
    for (size_t t = 0; t < a.tracks.size(); ++t)
    {
        const ArrTrack& tr = a.tracks[t];
        if (tr.kind != ArrTrack::Kind::Drum || (audibleOnly && !a.audible(tr)))
            continue;
        if (tr.source >= kPads)
        {
            out.skippedHits += tr.notes.size();
            continue;
        }
        anyDrums = true;
        out.channels[t] = { kDrumChannel };
        const auto note = static_cast<uint8_t>(kFirstPadNote + tr.source);
        const double padStep = tr.source < static_cast<int>(a.samples.size()) ? a.samples[static_cast<size_t>(tr.source)].step : 0x80;
        constexpr uint8_t cc = 0xB0 | kDrumChannel;
        for (const ArrNote& n : tr.notes)
        {
            auto semitones = [&](uint8_t step) {
                return 12.0 * std::log2(std::max<uint8_t>(step, 1) / padStep) + (n.pitch - 60) + n.fine;
            };
            // Each message group goes out whole and in order, so hits at one
            // time cannot take each other's settings. The note on or
            // aftertouch names the pad, and the bend after it retunes that pad.
            for (size_t i = 0; i < n.hit.size(); ++i)
            {
                const HitChange& now = n.hit[i];
                if (i > 0 && now.offset >= n.length)
                    break;
                const int64_t time = n.start + (i == 0 ? 0 : now.offset);
                const uint8_t volume = volumeValue(now.left, now.right);
                emit.add(time, 2, static_cast<int>(t), cc, 10, panValue(now.left, now.right));
                if (i == 0)
                    emit.add(time, 2, static_cast<int>(t), 0x90 | kDrumChannel, note, std::max<uint8_t>(volume, 1));
                else
                    emit.add(time, 2, static_cast<int>(t), 0xA0 | kDrumChannel, note, volume);
                if (i == 0 || now.step != n.hit[i - 1].step)
                    emit.bend(time, 2, static_cast<int>(t), kDrumChannel, semitones(now.step));
            }
            emit.add(std::max(n.end(), n.start + 1), 0, static_cast<int>(t), 0x80 | kDrumChannel, note, 0);
        }
    }

    // Setup: each channel's bend range, and drum gating, so note offs end
    // samples where the game cut them.
    for (size_t t = 0; t < out.channels.size(); ++t)
        for (int ch : out.channels[t])
            bendRange(out.setup, static_cast<int>(t), ch, out.bendRange);
    if (anyDrums)
        out.setup.push_back({ 0, 0, -1, { 0xB0 | kDrumChannel, static_cast<uint8_t>(Engine::kDrumGateController), 127 }, 3 });

    std::stable_sort(out.events.begin(), out.events.end(), [](const ArrangementEvent& x, const ArrangementEvent& y) {
        return x.time != y.time ? x.time < y.time : x.order < y.order;
    });
    return out;
}

} // namespace opm86
