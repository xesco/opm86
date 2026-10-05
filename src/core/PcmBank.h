#pragma once

#include "SegaPcm.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace opm86
{

inline constexpr int kPads = 16;
inline constexpr int kFirstPadNote = 36; // pads follow the General MIDI drum map
inline constexpr size_t kMaxSampleBytes = 0xFF00;
inline constexpr size_t kMaxRomSize = 0x1000000; // the chip's 24-bit address space, 16 MB

// One sample as the chip stores it: unsigned 8-bit, 0x80 = silence.
struct PadSample
{
    std::string name;
    std::string group; // the song a sample was imported from; empty otherwise
    std::vector<uint8_t> data;
    uint8_t step = 0x80;    // chip step that plays the sample at its own pitch
    bool loop = false;
    uint32_t loopStart = 0; // byte offset into data
    // Where a sample imported from a song sits in that song's ROM: its bank's
    // offset, the bank bits of the channel flags (register 0x86) that select
    // it, and its start address in the bank (registers 0x84 and 0x85). The
    // data runs from there. The game's usual volumes for it, 0-127.
    uint32_t romOffset = 0;
    uint8_t bank = 0;
    uint16_t address = 0;
    uint8_t left = 127, right = 127;
};

using PadSet = std::array<PadSample, kPads>;

// Where a pad's sample landed in the ROM image.
struct PcmSlot
{
    bool valid = false;
    uint8_t bank = 0;
    uint16_t start = 0;
    uint16_t loopStart = 0;
    uint8_t endPage = 0;
    uint8_t step = 0x80;
    bool loop = false;
};

// A ROM image ready for the chip, plus the slot of each pad.
struct PcmBank
{
    std::vector<uint8_t> rom = std::vector<uint8_t>(SegaPcm::kRomSize, 0x80);
    std::array<PcmSlot, kPads> slots {};
};

// Pack the pad samples into 64 KB banks. A sample that does not fit in
// the space left is truncated, as the hardware would require.
std::shared_ptr<const PcmBank> buildBank(const PadSet& pads);

// A drum kit synthesised at the chip's native rate, used until the user
// loads or imports samples.
PadSet makeDefaultKit();

// Convert float audio to a chip sample at 15625 Hz (step 0x80).
PadSample makeSample(const std::string& name, const float* audio, size_t length, double sampleRate);

// How audio becomes a chip sample (see encodeSample).
struct SampleEncoding
{
    uint8_t step = 0x80;   // the rate it is made at and plays at: step / 256 x the chip's 31250 Hz, 15625 Hz at 0x80
    bool trim = true;      // the silence at its start and its end left out
    bool normalise = true; // its loudest moment brought to full scale
};

// Audio of one or more channels, each length frames at sampleRate, as a
// chip sample that plays once at the encoding's step: mixed down to mono,
// trimmed and normalised as asked, resampled to the step's rate and cut at
// kMaxSampleBytes. Its place in a ROM is left to placeSample.
PadSample encodeSample(const std::string& name, const float* const* channels, int channelCount, size_t length, double sampleRate,
                       const SampleEncoding& encoding);

// A sample loaded from an audio file for a song: the index it has among the
// song's samples, in place of the rip's sample there or after them as a new
// one, and its sound, as encodeSample makes it.
struct LoadedSample
{
    int index = 0;
    PadSample sample;
};

// The ROM's byte ranges in use, each from its first byte to the first past it.
using RomRanges = std::vector<std::pair<size_t, size_t>>;

// The ranges a song's samples read in its ROM, each from the start of its
// first page to the end of its last, as the chip plays whole pages.
RomRanges samplePages(const std::vector<PadSample>& samples);

// Finds a sample's place in a song's ROM of romSize bytes: the start of the
// first run of pages free for it in a bank the board's banking reaches (see
// SegaPcm::banking), the banks in order of their place in the ROM. A bank
// past the ROM's end is a new one: romSize grows to hold it, up to maxSize
// and never past kMaxRomSize. The sample keeps to its bank and ends before
// its last page, as the chip requires. Its place is set: romOffset, bank
// and address; writeSample then puts its data there. The range joins used.
// Returns false, changing nothing, when no bank has room.
bool placeSample(size_t& romSize, uint32_t interfaceRegister, RomRanges& used, PadSample& sample, size_t maxSize = kMaxRomSize);
// Writes a placed sample's data into a ROM at its place, the rest of its
// last page silent. A ROM too small for it is left as it is.
void writeSample(std::vector<uint8_t>& rom, const PadSample& sample);

} // namespace opm86
