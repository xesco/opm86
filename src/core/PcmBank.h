#pragma once

#include "SegaPcm.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opm86
{

inline constexpr int kPads = 16;
inline constexpr int kFirstPadNote = 36; // pads follow the General MIDI drum map
inline constexpr size_t kMaxSampleBytes = 0xFF00;

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

} // namespace opm86
