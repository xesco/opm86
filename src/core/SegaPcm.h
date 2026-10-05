#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opm86
{

// Sega 315-5218 PCM chip: 16 channels of unsigned 8-bit samples with
// 7-bit stereo volume and an 8.8 fixed-point step. Behaviour follows
// MAME's segapcm.cpp. Its banks are wired per board: the VGM header's
// interface register says how (see banking), and with none the chip runs
// in the 512 KB bank mode of Out Run.
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

    // How the bank bits of a channel's flags select its place in the ROM: a
    // ROM offset is (flags & mask) << shift. MAME's decoding of the VGM
    // header's interface register: the low byte is the shift, and bits
    // 16-23 widen the mask. 0, as most rips have it, is the 512 KB bank mode.
    struct Banking
    {
        int shift = 12;
        uint32_t mask = 0x70;
        uint32_t offset(uint8_t flags) const { return static_cast<uint32_t>(flags & mask) << shift; }
    };
    static Banking banking(uint32_t interfaceRegister);

    // The ROM, of size bytes, and the bank decoding from the VGM header's
    // interface register. A read past the ROM's end plays silence.
    void setRom(const uint8_t* rom, size_t size = kRomSize, uint32_t interfaceRegister = 0);
    void reset();

    void write(int offset, uint8_t value);
    uint8_t read(int offset) const { return ram_[offset & 0xFF]; }

    // Advance by one sample at kSampleRate. Channels outside the mask
    // still play on, but are left out of the output. With peaks (one per
    // channel), each output channel's louder side raises its peak.
    // With gains (one per channel), each channel's output is scaled by its gain.
    void generate(int32_t& left, int32_t& right, uint16_t channelMask = 0xFFFF, int32_t* peaks = nullptr, const float* gains = nullptr);

private:
    std::array<uint8_t, 0x100> ram_ {};
    std::array<uint8_t, kChannels> low_ {};
    const uint8_t* rom_ = nullptr;
    size_t romSize_ = kRomSize;
    uint32_t romMask_ = kRomSize - 1; // the ROM's size rounded up to a power of two, less one
    Banking banking_;
};

} // namespace opm86
