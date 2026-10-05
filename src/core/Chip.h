#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace opm86
{

// The YM2151's chip-wide settings, which every voice shares: the LFO's rate,
// wave and two depths, and the noise generator, which takes the place of
// voice V8's C2 operator while it is on.
struct ChipState
{
    uint8_t lfoRate = 0;   // LFRQ, register 0x18, 0-255
    uint8_t lfoWave = 0;   // W, register 0x1B bits 0-1: 0 saw, 1 square, 2 triangle, 3 noise
    uint8_t pmd = 0;       // vibrato depth, register 0x19 with bit 7 set, 0-127
    uint8_t amd = 0;       // tremolo depth, register 0x19 with bit 7 clear, 0-127
    bool noise = false;    // NE, register 0x0F bit 7
    uint8_t noiseFreq = 0; // NFRQ, register 0x0F bits 0-4, 0-31

    bool operator==(const ChipState&) const = default;
};

// A song's chip settings as edited in the tracker. A field set takes the
// place of the game's writes to it all through the song; a field left unset
// plays as the game wrote it.
struct ChipEdits
{
    std::optional<uint8_t> lfoRate, lfoWave, pmd, amd;
    std::optional<bool> noise;
    std::optional<uint8_t> noiseFreq;

    bool any() const { return lfoRate || lfoWave || pmd || amd || noise || noiseFreq; }
    // The settings heard: each field set here, else the game's.
    ChipState over(const ChipState& game) const
    {
        return { lfoRate.value_or(game.lfoRate), lfoWave.value_or(game.lfoWave), pmd.value_or(game.pmd),
                 amd.value_or(game.amd), noise.value_or(game.noise), noiseFreq.value_or(game.noiseFreq) };
    }

    bool operator==(const ChipEdits&) const = default;
};

// The chip-wide settings in a register shadow that keeps the vibrato depth
// at the unused register 0x1A, as ymfm does (see SongPlayer).
inline ChipState chipOf(const std::array<uint8_t, 256>& regs)
{
    return { regs[0x18], static_cast<uint8_t>(regs[0x1B] & 3), static_cast<uint8_t>(regs[0x1A] & 0x7F),
             static_cast<uint8_t>(regs[0x19] & 0x7F), (regs[0x0F] & 0x80) != 0, static_cast<uint8_t>(regs[0x0F] & 0x1F) };
}

} // namespace opm86
