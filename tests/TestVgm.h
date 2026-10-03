#pragma once

// A minimal VGM 1.71 file with an Out Run style chip setup, shared by the tests.
// It holds two FM patches and one 512-byte PCM sample in bank 1.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opm86::test
{

inline void put32(std::vector<uint8_t>& d, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        d[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

inline std::vector<uint8_t> makeVgm()
{
    std::vector<uint8_t> d(0x100, 0);
    d[0] = 'V', d[1] = 'g', d[2] = 'm', d[3] = ' ';
    put32(d, 0x08, 0x171);
    put32(d, 0x30, 4000000);          // YM2151
    put32(d, 0x34, 0x100 - 0x34);     // data starts at 0x100
    put32(d, 0x38, 4000000);          // SegaPCM
    put32(d, 0x3C, 0x0070000C);       // 512 KB bank mode

    auto ym = [&](uint8_t reg, uint8_t value) { d.insert(d.end(), { 0x54, reg, value }); };
    auto pcm = [&](uint16_t at, uint8_t value) {
        d.insert(d.end(), { 0xC0, static_cast<uint8_t>(at), static_cast<uint8_t>(at >> 8), value });
    };

    // PCM ROM block: 512 bytes of ramp in bank 1.
    const uint32_t size = 8 + 512;
    d.insert(d.end(), { 0x67, 0x66, 0x80 });
    for (uint32_t v : { size, 0x80000u, 0x10000u })
        for (int i = 0; i < 4; ++i)
            d.push_back(static_cast<uint8_t>(v >> (8 * i)));
    for (int i = 0; i < 512; ++i)
        d.push_back(static_cast<uint8_t>(i & 0xFF));

    // Channel 0: algorithm 4, feedback 5, M2 multiple 3; key on twice
    // with different carrier levels, as a driver applying note volume would.
    ym(0x20, 0xC0 | 5 << 3 | 4);
    ym(0x40 + 8, 0x03);
    for (uint8_t tl : { 20, 10 })
    {
        ym(0x60 + 16, tl); // C1
        ym(0x60 + 24, tl); // C2
        ym(0x08, 0x78);
        d.insert(d.end(), { 0x62 });
        ym(0x08, 0x00);
    }
    // Channel 1: a different patch.
    ym(0x21, 0xC0 | 7);
    ym(0x08, 0x79);

    // PCM channel 3: bank 1, start 0, end page 1, step 0x40, one shot.
    const uint16_t base = 3 * 8;
    pcm(base + 0x86, 0x13);
    pcm(base + 0x02, 100);
    pcm(base + 0x03, 100);
    pcm(base + 0x06, 0x01);
    pcm(base + 0x07, 0x40);
    pcm(base + 0x84, 0x00);
    pcm(base + 0x85, 0x00);
    pcm(base + 0x86, 0x12);

    d.push_back(0x66);
    return d;
}

} // namespace opm86::test

namespace opm86::test
{

// Builds VGM files command by command.
class VgmBuilder
{
public:
    VgmBuilder()
    {
        d_.assign(0x100, 0);
        d_[0] = 'V', d_[1] = 'g', d_[2] = 'm', d_[3] = ' ';
        put32(d_, 0x08, 0x171);
        put32(d_, 0x30, 4000000);
        put32(d_, 0x34, 0x100 - 0x34);
        put32(d_, 0x38, 4000000);
        put32(d_, 0x3C, 0x0070000C);
    }

    void ym(uint8_t reg, uint8_t value) { d_.insert(d_.end(), { 0x54, reg, value }); }

    void pcm(uint16_t at, uint8_t value)
    {
        d_.insert(d_.end(), { 0xC0, static_cast<uint8_t>(at), static_cast<uint8_t>(at >> 8), value });
    }

    void wait(uint32_t samples)
    {
        while (samples > 0)
        {
            const uint32_t n = samples > 65535 ? 65535 : samples;
            d_.insert(d_.end(), { 0x61, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8) });
            samples -= n;
        }
    }

    void rom(const std::vector<uint8_t>& data, uint32_t start)
    {
        const auto size = static_cast<uint32_t>(8 + data.size());
        d_.insert(d_.end(), { 0x67, 0x66, 0x80 });
        for (uint32_t v : { size, 0x80000u, start })
            for (int i = 0; i < 4; ++i)
                d_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        d_.insert(d_.end(), data.begin(), data.end());
    }

    std::vector<uint8_t> finish()
    {
        d_.push_back(0x66);
        return d_;
    }

private:
    std::vector<uint8_t> d_;
};

// A 3 s song: a lead on YM channel 1 (one note louder than the rest, one
// that slides up two semitones), a bass on channel 2 with another patch, and
// two drum samples.
inline std::vector<uint8_t> makeSong()
{
    VgmBuilder v;

    // Drum samples: a 2 KB decaying low tone at 0x0000, a 1 KB burst at 0x0800.
    std::vector<uint8_t> rom(0xC00, 0x80);
    for (size_t i = 0; i < 0x800; ++i)
        rom[i] = static_cast<uint8_t>(0x80 + 100.0 * std::exp(-static_cast<double>(i) / 500.0)
                                                 * std::sin(static_cast<double>(i) * 0.08));
    uint32_t noise = 1;
    for (size_t i = 0x800; i < 0xC00; ++i)
    {
        noise = noise * 1103515245u + 12345u;
        rom[i] = static_cast<uint8_t>(0x80 + static_cast<int>((noise >> 16) % 120) - 60);
    }
    v.rom(rom, 0);

    // Patch for channel 0: algorithm 4, feedback 5. Channel 1: algorithm 7.
    const uint8_t slots[4] = { 0, 16, 8, 24 };
    auto voice = [&](uint8_t ch, uint8_t con, const uint8_t tl[4], const uint8_t mul[4]) {
        v.ym(0x20 + ch, con);
        for (int i = 0; i < 4; ++i)
        {
            const auto s = static_cast<uint8_t>(ch + slots[i]);
            v.ym(0x40 + s, mul[i]);
            v.ym(0x60 + s, tl[i]);
            v.ym(0x80 + s, 31);
            v.ym(0xA0 + s, 6);
            v.ym(0xC0 + s, 2);
            v.ym(0xE0 + s, 0x27);
        }
    };
    const uint8_t leadTl[4] = { 30, 8, 34, 8 }, leadMul[4] = { 1, 1, 3, 1 };
    const uint8_t bassTl[4] = { 20, 24, 28, 32 }, bassMul[4] = { 1, 2, 1, 1 };
    voice(0, 0xC0 | 5 << 3 | 4, leadTl, leadMul);
    voice(1, 0xC0 | 7, bassTl, bassMul);

    auto key = [&](uint8_t ch, uint8_t kc) {
        v.ym(0x28 + ch, kc);
        v.ym(0x30 + ch, 0);
        v.ym(0x08, static_cast<uint8_t>(0x78 | ch));
    };
    auto drum = [&](uint8_t ch, uint16_t start, uint8_t endPage, uint8_t volume) {
        const uint16_t base = static_cast<uint16_t>(ch * 8);
        v.pcm(base + 0x86, 0x03);
        v.pcm(base + 0x02, volume);
        v.pcm(base + 0x03, volume);
        v.pcm(base + 0x06, endPage);
        v.pcm(base + 0x07, 0x80);
        v.pcm(base + 0x84, static_cast<uint8_t>(start));
        v.pcm(base + 0x85, static_cast<uint8_t>(start >> 8));
        v.pcm(base + 0x86, 0x02);
    };

    // Key codes: octave in bits 4-6, note code below; A = 10, C = 14, E = 4.
    const uint8_t lead[6] = { 0x4A, 0x4E, 0x54, 0x4A, 0x4E, 0x4A };
    for (int step = 0; step < 12; ++step)
    {
        if (step % 2 == 0)
        {
            const int n = step / 2;
            // Key off one sample before key on, so the chip restarts the
            // envelopes, as a game's driver does.
            v.ym(0x08, 0x00);
            v.ym(0x08, 0x01);
            v.wait(1);
            // The second note is 8 level steps (6 dB) quieter, as a driver would play it.
            const uint8_t carrier = n == 1 ? 16 : 8;
            v.ym(0x60 + 16, carrier);
            v.ym(0x60 + 24, carrier);
            key(0, lead[n]);
            key(1, 0x2A);
            drum(0, 0x0000, 0x07, 100);
        }
        else
            drum(1, 0x0800, 0x0B, 80);

        const uint32_t rest = step % 2 == 0 ? 11024 : 11025;
        if (step == 6)
        {
            // Slide: half a semitone, then the full two semitones.
            v.wait(4410);
            v.ym(0x30, 128);
            v.wait(4410);
            v.ym(0x28, 0x4D);
            v.ym(0x30, 0);
            v.wait(rest - 8820);
        }
        else
            v.wait(rest);
    }
    v.ym(0x08, 0x00);
    v.ym(0x08, 0x01);
    v.wait(22050);
    return v.finish();
}

} // namespace opm86::test
