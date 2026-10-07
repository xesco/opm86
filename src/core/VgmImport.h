#pragma once

#include "FmPatch.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opm86
{

// VGM files count time in samples at this rate.
inline constexpr double kVgmRate = 44100.0;

inline constexpr size_t kMaxSampleBytes = 0xFF00; // the longest sample kept, short of a bank

// One sample as the chip stores it: unsigned 8-bit, 0x80 = silence.
struct PadSample
{
    std::string name;
    std::string group; // the song a sample was imported from; empty otherwise
    std::vector<uint8_t> data;
    uint8_t step = 0x80;    // chip step that plays the sample at its own pitch
    bool loop = false;
    uint32_t loopStart = 0; // byte offset into data
};

// A YM2151 channel's settings from a given time: what a note sounds with.
struct VgmNoteState
{
    uint32_t time = 0;
    int patch = 0;       // index into VgmImport::patches
    int attenuation = 0; // carrier level below the patch's loudest use, in 0.75 dB steps
    FmPan pan = FmPan::Both;

    bool operator==(const VgmNoteState&) const = default;
};

// One YM2151 note as the song plays it. Times are in VGM samples.
struct VgmNote
{
    uint32_t start = 0;
    uint32_t end = 0;
    int channel = 0;  // YM2151 channel, 0-7
    int patch = 0;    // index into VgmImport::patches, at the start
    // The settings at the start, and each change until the channel's next
    // note, including changes during the release after the note ends.
    std::vector<VgmNoteState> state;
    // The sounding pitch as a MIDI note number with a fraction, and each
    // later change while the note is held, starting with the pitch at start.
    std::vector<std::pair<uint32_t, double>> pitch;
};

// A SegaPCM channel's settings from a given time.
struct VgmHitState
{
    uint32_t time = 0;
    int left = 0;          // volume, 0-127
    int right = 0;         // volume, 0-127
    uint8_t step = 0x80;   // chip step, which sets the playback pitch
};

// One SegaPCM sample start. Times are in VGM samples.
struct VgmHit
{
    uint32_t start = 0;
    uint32_t end = 0;
    int sample = 0; // index into VgmImport::samples
    // The settings at the start, and each later change while the sample plays.
    std::vector<VgmHitState> state;
};

// What a VGM log of an arcade tune reveals about its instruments and notes.
struct VgmImport
{
    std::string error;                // empty on success
    uint32_t ym2151Clock = 0;
    uint32_t segaPcmClock = 0;
    std::vector<FmPatch> patches;     // most used first
    std::vector<PadSample> samples;   // most used first
    std::vector<VgmNote> notes;       // in order of start
    std::vector<VgmHit> hits;         // in order of start
    uint32_t length = 0;              // in VGM samples, up to the end command
};

// Receives the commands of a VGM file in order.
struct VgmHeader
{
    uint32_t ym2151Clock = 0;
    uint32_t segaPcmClock = 0;
    uint32_t segaPcmInterface = 0;
};

class VgmVisitor
{
public:
    virtual ~VgmVisitor() = default;
    virtual void begin(const VgmHeader& header) = 0; // before the first command
    virtual void wait(uint32_t samples) = 0;
    virtual void ym2151(uint8_t reg, uint8_t value) = 0;
    virtual void segaPcm(uint16_t offset, uint8_t value) = 0;
    virtual void pcmRom(const uint8_t* data, size_t size, uint32_t romSize, uint32_t start) = 0;
};

// Read a .vgm or gzipped .vgz file and pass its commands to the visitor.
// Returns an error, or an empty string; an error after some commands have
// been passed leaves those commands delivered.
std::string parseVgm(const std::vector<uint8_t>& file, VgmVisitor& visitor);

// Parse a .vgm or gzipped .vgz file. Every YM2151 key on records the
// channel's registers as a patch and a note, and every SegaPCM key on
// records a hit of the sample it starts, cut from the ROM image the file
// embeds.
VgmImport importVgm(const std::vector<uint8_t>& file, const std::string& name);

} // namespace opm86
