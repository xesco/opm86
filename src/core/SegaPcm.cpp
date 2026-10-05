#include "SegaPcm.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace opm86
{

SegaPcm::Banking SegaPcm::banking(uint32_t interfaceRegister)
{
    Banking b;
    if (interfaceRegister == 0)
        return b;
    // A shift past the 24-bit address space would select nothing.
    b.shift = static_cast<int>(std::min<uint32_t>(interfaceRegister & 0xFF, 24));
    b.mask = 0x70 | ((interfaceRegister >> 16) & 0xFC);
    return b;
}

void SegaPcm::setRom(const uint8_t* rom, size_t size, uint32_t interfaceRegister)
{
    rom_ = rom;
    romSize_ = size;
    uint32_t mask = 1;
    while (mask < size && mask < 0x80000000u)
        mask <<= 1;
    romMask_ = mask - 1;
    banking_ = banking(interfaceRegister);
}

void SegaPcm::reset()
{
    ram_.fill(0);
    low_.fill(0);
    for (int ch = 0; ch < kChannels; ++ch)
        ram_[ch * 8 + 0x86] = 0x01;
}

void SegaPcm::write(int offset, uint8_t value)
{
    offset &= 0xFF;
    ram_[offset] = value;

    // A new start address restarts the fractional position.
    const int reg = offset & 0x87;
    if (reg == 0x84 || reg == 0x85)
        low_[(offset >> 3) & 15] = 0;
}

void SegaPcm::generate(int32_t& left, int32_t& right, uint16_t channelMask, int32_t* peaks, const float* gains)
{
    left = right = 0;
    if (rom_ == nullptr)
        return;

    for (int ch = 0; ch < kChannels; ++ch)
    {
        uint8_t* regs = &ram_[ch * 8];
        if (regs[0x86] & 1)
            continue;

        const uint32_t bank = banking_.offset(regs[0x86]);
        uint32_t addr = static_cast<uint32_t>(regs[0x85]) << 16 | static_cast<uint32_t>(regs[0x84]) << 8 | low_[ch];
        const uint32_t loop = static_cast<uint32_t>(regs[0x05]) << 16 | static_cast<uint32_t>(regs[0x04]) << 8;
        const auto end = static_cast<uint8_t>(regs[6] + 1);

        if ((addr >> 16) == end)
        {
            if (regs[0x86] & 2)
            {
                regs[0x86] |= 1;
                low_[ch] = 0;
                continue;
            }
            addr = loop;
        }

        if ((channelMask >> ch) & 1)
        {
            const uint32_t at = (bank + (addr >> 8)) & romMask_;
            const int v = static_cast<int>(at < romSize_ ? rom_[at] : 0x80) - 0x80;
            int l = v * (regs[2] & 0x7F), r = v * (regs[3] & 0x7F);
            if (gains)
                l = static_cast<int>(std::lround(static_cast<float>(l) * gains[ch])), r = static_cast<int>(std::lround(static_cast<float>(r) * gains[ch]));
            left += l;
            right += r;
            if (peaks)
                peaks[ch] = std::max({ peaks[ch], std::abs(l), std::abs(r) });
        }

        addr = (addr + regs[7]) & 0xFFFFFF;
        regs[0x84] = static_cast<uint8_t>(addr >> 8);
        regs[0x85] = static_cast<uint8_t>(addr >> 16);
        low_[ch] = static_cast<uint8_t>(addr);
    }
}

} // namespace opm86
