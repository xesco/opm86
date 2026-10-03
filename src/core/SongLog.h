#pragma once

#include "VgmSong.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace opm86
{

// A VGM log split into the notes it plays. Every register write belongs to
// one YM2151 note, one SegaPCM hit, or the song as a whole, so a note can be
// moved, stretched or transposed by changing its own writes, and the song
// rebuilt from the parts. Times are in VGM samples (1/44100 s).

struct LogWrite
{
    int64_t offset = 0;  // from its note's or hit's start; from 0 for song-wide writes
    uint32_t order = 0;  // place in the original log, which orders writes at one time
    uint16_t address = 0;
    uint8_t value = 0;
};

// One YM2151 note: its channel's writes from just after the previous note's
// key off up to and including its own key off. It carries the patch the
// driver loads for it, its key on, pitch and level changes, and its key off.
struct LogNote
{
    int channel = 0;     // YM2151 channel, 0-7
    int64_t start = 0;   // time of the key on
    int64_t end = 0;     // time of the key off; the song's end when there is none
    std::vector<LogWrite> writes;
};

// One SegaPCM hit: its channel's writes from its set-up, written just
// before the trigger, up to the next hit's set-up.
struct LogHit
{
    int channel = 0;     // SegaPCM channel, 0-15
    int64_t start = 0;   // time of the trigger
    int64_t end = 0;     // when the next hit on the channel starts, or the song ends
    std::vector<LogWrite> writes;
};

struct SongLog
{
    std::vector<LogWrite> ymSong, pcmSong; // song-wide writes
    std::vector<LogNote> notes;            // in order of start
    std::vector<LogHit> hits;              // in order of start
};

// Writes this close before a hit's trigger set the hit up (about 0.2 ms).
inline constexpr int64_t kHitSetUp = 8;

SongLog splitLog(const VgmSong& song);

// The song with its writes rebuilt from the log's parts. With no edits it
// holds the same writes, in the same order, as the song the log came from.
std::shared_ptr<VgmSong> rebuildSong(const VgmSong& source, const SongLog& log);

} // namespace opm86
