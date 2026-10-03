#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opm86
{

// Sega 315-5218 PCM chip: 16 channels of unsigned 8-bit samples with
// 7-bit stereo volume and an 8.8 fixed-point step. Behaviour follows
// MAME's segapcm.cpp in its 512 KB bank mode, as wired in Out Run.
//
// Per-channel registers, at channel * 8 + offset:
//   0x02 left volume    0x03 right volume
//   0x04 loop low       0x05 loop high
//   0x06 end page       0x07 step (0x80 = half a byte per sample)
//   0x84 address low    0x85 address high
//   0x86 flags: bit 0 off, bit 1 no loop, bits 4-6 bank
class SegaPcm
{
public:
    static constexpr double kClock = 4000000.0;
    static constexpr double kSampleRate = kClock / 128.0; // 31250 Hz
    static constexpr int kChannels = 16;
    static constexpr size_t kRomSize = 0x80000;           // 8 banks of 64 KB

    void setRom(const uint8_t* rom) { rom_ = rom; }
    void reset();

    void write(int offset, uint8_t value);
    uint8_t read(int offset) const { return ram_[offset & 0xFF]; }

    // Advance by one sample at kSampleRate. Channels outside the mask
    // still play on, but are left out of the output. With peaks (one per
    // channel), each output channel's louder side raises its peak.
    void generate(int32_t& left, int32_t& right, uint16_t channelMask = 0xFFFF, int32_t* peaks = nullptr);

private:
    std::array<uint8_t, 0x100> ram_ {};
    std::array<uint8_t, kChannels> low_ {};
    const uint8_t* rom_ = nullptr;
};

} // namespace opm86
