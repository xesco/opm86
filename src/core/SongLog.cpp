#include "SongLog.h"

#include <algorithm>
#include <array>

namespace opm86
{

namespace
{

struct Timed
{
    int64_t time;
    uint32_t order;
    uint16_t address;
    uint8_t value;
};

LogWrite relative(const Timed& w, int64_t start)
{
    return { w.time - start, w.order, w.address, w.value };
}

// YM2151 registers below 0x20, except the key register, are the chip's:
// LFO, noise, timers. The rest belong to the channel in their low three bits.
int ymChannel(const VgmWrite& w)
{
    if (w.address == 0x08)
        return w.value & 7;
    if (w.address >= 0x20 && w.address <= 0xFF)
        return w.address & 7;
    return -1;
}

void splitYm(const VgmSong& song, SongLog& log)
{
    struct Channel
    {
        std::vector<Timed> pending; // since the last key off: the next note's set-up
        int note = -1;              // the note whose key is down, or -1
    };
    std::array<Channel, 8> channels;
    std::array<int, 8> lastNote;
    lastNote.fill(-1);

    for (size_t i = 0; i < song.ymLogged; ++i)
    {
        const VgmWrite& w = song.ym[i];
        const Timed t { w.time, static_cast<uint32_t>(i), w.address, w.value };
        const int ch = ymChannel(w);
        if (ch < 0)
        {
            log.ymSong.push_back(relative(t, 0));
            continue;
        }
        Channel& c = channels[static_cast<size_t>(ch)];
        const bool key = w.address == 0x08;
        const bool keyOn = key && (w.value & 0x78) != 0;

        if (keyOn)
        {
            // A key on with the key still down ends the note before it.
            if (c.note >= 0)
                log.notes[static_cast<size_t>(c.note)].end = w.time;
            LogNote note;
            note.channel = ch;
            note.start = w.time;
            note.end = static_cast<int64_t>(song.length);
            for (const Timed& p : c.pending)
                note.writes.push_back(relative(p, note.start));
            note.writes.push_back(relative(t, note.start));
            c.pending.clear();
            c.note = static_cast<int>(log.notes.size());
            lastNote[static_cast<size_t>(ch)] = c.note;
            log.notes.push_back(std::move(note));
        }
        else if (c.note >= 0)
        {
            LogNote& note = log.notes[static_cast<size_t>(c.note)];
            note.writes.push_back(relative(t, note.start));
            if (key) // key off
            {
                note.end = w.time;
                c.note = -1;
            }
        }
        else
            c.pending.push_back(t);
    }

    // Writes after a channel's last key off stay with its last note, as its
    // release; a channel that never keys on leaves them song-wide.
    for (int ch = 0; ch < 8; ++ch)
    {
        Channel& c = channels[static_cast<size_t>(ch)];
        const int owner = lastNote[static_cast<size_t>(ch)];
        for (const Timed& p : c.pending)
        {
            if (owner >= 0)
            {
                LogNote& note = log.notes[static_cast<size_t>(owner)];
                note.writes.push_back(relative(p, note.start));
            }
            else
                log.ymSong.push_back(relative(p, 0));
        }
    }
}

void splitPcm(const VgmSong& song, SongLog& log)
{
    struct Channel
    {
        std::vector<Timed> pending; // before the channel's first hit
        int hit = -1;               // the channel's latest hit, or -1
    };
    std::array<Channel, SegaPcm::kChannels> channels;

    for (size_t i = 0; i < song.pcmLogged; ++i)
    {
        const VgmWrite& w = song.pcm[i];
        const Timed t { w.time, static_cast<uint32_t>(i), w.address, w.value };
        const int ch = (w.address >> 3) & 15;
        Channel& c = channels[static_cast<size_t>(ch)];
        const bool trigger = (w.address & 0x87) == 0x86 && !(w.value & 1);

        if (!trigger)
        {
            if (c.hit >= 0)
            {
                LogHit& hit = log.hits[static_cast<size_t>(c.hit)];
                hit.writes.push_back(relative(t, hit.start));
            }
            else
                c.pending.push_back(t);
            continue;
        }

        // The writes just before the trigger set this hit up, so they move
        // from the previous hit to this one.
        LogHit hit;
        hit.channel = ch;
        hit.start = w.time;
        hit.end = static_cast<int64_t>(song.length);
        std::vector<Timed> setUp;
        if (c.hit >= 0)
        {
            LogHit& previous = log.hits[static_cast<size_t>(c.hit)];
            while (!previous.writes.empty() && previous.start + previous.writes.back().offset >= w.time - kHitSetUp
                   && previous.writes.back().offset > 0)
            {
                const LogWrite& back = previous.writes.back();
                setUp.insert(setUp.begin(), { previous.start + back.offset, back.order, back.address, back.value });
                previous.writes.pop_back();
            }
            previous.end = setUp.empty() ? w.time : setUp.front().time;
        }
        for (const Timed& p : c.pending)
            hit.writes.push_back(relative(p, hit.start));
        for (const Timed& p : setUp)
            hit.writes.push_back(relative(p, hit.start));
        hit.writes.push_back(relative(t, hit.start));
        c.pending.clear();
        c.hit = static_cast<int>(log.hits.size());
        log.hits.push_back(std::move(hit));
    }

    for (Channel& c : channels)
        for (const Timed& p : c.pending)
            log.pcmSong.push_back(relative(p, 0));
}

} // namespace

SongLog splitLog(const VgmSong& song)
{
    SongLog log;
    splitYm(song, log);
    splitPcm(song, log);
    auto byStart = [](const auto& a, const auto& b) { return a.start < b.start; };
    // Notes and hits were made in log order, which is start order already;
    // the sort is kept stable so equal starts keep that order.
    std::stable_sort(log.notes.begin(), log.notes.end(), byStart);
    std::stable_sort(log.hits.begin(), log.hits.end(), byStart);
    return log;
}

std::shared_ptr<VgmSong> rebuildSong(const VgmSong& source, const SongLog& log)
{
    auto song = std::make_shared<VgmSong>();
    song->name = source.name;
    song->header = source.header;
    song->fmRate = source.fmRate;
    song->pcmRate = source.pcmRate;
    song->length = source.length;
    song->rom = source.rom;
    song->fmChannelsUsed = source.fmChannelsUsed;
    song->pcmUsed = source.pcmUsed;
    song->beatLength = source.beatLength;
    song->firstBeat = source.firstBeat;
    song->beatCount = source.beatCount;

    // Every write at its time, then by its place in the original log.
    std::vector<Timed> ym, pcm;
    for (const LogWrite& w : log.ymSong)
        ym.push_back({ w.offset, w.order, w.address, w.value });
    for (const LogNote& n : log.notes)
        for (const LogWrite& w : n.writes)
            ym.push_back({ n.start + w.offset, w.order, w.address, w.value });
    for (const LogWrite& w : log.pcmSong)
        pcm.push_back({ w.offset, w.order, w.address, w.value });
    for (const LogHit& h : log.hits)
        for (const LogWrite& w : h.writes)
            pcm.push_back({ h.start + w.offset, w.order, w.address, w.value });
    auto earlier = [](const Timed& a, const Timed& b) { return a.time != b.time ? a.time < b.time : a.order < b.order; };
    std::sort(ym.begin(), ym.end(), earlier);
    std::sort(pcm.begin(), pcm.end(), earlier);

    for (const Timed& t : ym)
        song->ym.push_back({ static_cast<uint32_t>(std::max<int64_t>(0, t.time)), t.address, t.value });
    for (const Timed& t : pcm)
        song->pcm.push_back({ static_cast<uint32_t>(std::max<int64_t>(0, t.time)), t.address, t.value });
    appendSongEnd(*song);
    return song;
}

} // namespace opm86
