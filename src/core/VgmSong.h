#pragma once

#include "SegaPcm.h"
#include "VgmImport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opm86
{

// One register write of a VGM file, at its time in VGM samples.
struct VgmWrite
{
    uint32_t time;
    uint16_t address;
    uint8_t value;
};

// A VGM file read for exact playback: the writes to each chip in order,
// the sample memory, and a beat grid for cutting the song into beats.
struct VgmSong
{
    std::string name;
    VgmHeader header;
    double fmRate = 62500.0;   // YM2151 samples per second
    double pcmRate = 31250.0;  // SegaPCM samples per second
    uint32_t length = 0;       // VGM samples up to the end of the log
    std::vector<VgmWrite> ym, pcm;
    size_t ymLogged = 0, pcmLogged = 0; // writes from the file; the rest end the song
    std::vector<uint8_t> rom = std::vector<uint8_t>(SegaPcm::kRomSize, 0x80);
    uint8_t fmChannelsUsed = 0;  // bit n: YM2151 channel n plays notes
    bool pcmUsed = false;

    // Beat k starts at firstBeat + k x beatLength, in VGM samples. The grid
    // is fitted to the notes' start times; beatCount beats cover the song.
    double beatLength = 22050.0;
    double firstBeat = 0.0;
    int beatCount = 0;
    double tempo() const { return 60.0 * kVgmRate / beatLength; }
    double beatTime(int beat) const { return firstBeat + beat * beatLength; }
};

// A beat grid fitted to note start times, in VGM samples: beat k starts
// at first + k x length, with first the beat of the first start.
struct BeatGrid
{
    double length = kVgmRate / 2.0;
    double first = 0.0;
};
BeatGrid fitBeatGrid(std::vector<uint32_t> starts);

// Read a .vgm or .vgz file. Returns nullptr and sets error when it holds no
// YM2151 or SegaPCM writes; a late parse error is set but keeps the song.
std::shared_ptr<const VgmSong> loadVgmSong(const std::vector<uint8_t>& file, const std::string& name,
                                           std::string& error);

// Append the writes that end a song where its log ends: every FM channel
// keyed off, and looping samples stopped (address 0x1000 marks that stop),
// while one-shot samples play out. The game would loop back instead.
void appendSongEnd(VgmSong& song);

// A stable number for a song name, sent in MIDI to name the song.
uint32_t songId(const std::string& name);

} // namespace opm86
