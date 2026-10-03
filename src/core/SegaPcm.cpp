#include "SegaPcm.h"

#include <algorithm>
#include <cstdlib>

namespace opm86
{

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

void SegaPcm::generate(int32_t& left, int32_t& right, uint16_t channelMask, int32_t* peaks)
{
    left = right = 0;
    if (rom_ == nullptr)
        return;

    for (int ch = 0; ch < kChannels; ++ch)
    {
        uint8_t* regs = &ram_[ch * 8];
        if (regs[0x86] & 1)
            continue;

        const uint32_t bank = static_cast<uint32_t>(regs[0x86] & 0x70) << 12;
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
            const int v = static_cast<int>(rom_[(bank + (addr >> 8)) & (kRomSize - 1)]) - 0x80;
            left += v * (regs[2] & 0x7F);
            right += v * (regs[3] & 0x7F);
            if (peaks)
                peaks[ch] = std::max(peaks[ch], std::abs(v) * std::max(regs[2] & 0x7F, regs[3] & 0x7F));
        }

        addr = (addr + regs[7]) & 0xFFFFFF;
        regs[0x84] = static_cast<uint8_t>(addr >> 8);
        regs[0x85] = static_cast<uint8_t>(addr >> 16);
        low_[ch] = static_cast<uint8_t>(addr);
    }
}

} // namespace opm86
