#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opm86
{

// One YM2151 operator, with every field at its register width.
struct FmOperator
{
    uint8_t tl = 127;   // total level, 0-127 (0.75 dB steps, 0 = loudest)
    uint8_t ar = 31;    // attack rate, 0-31
    uint8_t d1r = 0;    // first decay rate, 0-31
    uint8_t d2r = 0;    // second decay rate, 0-31
    uint8_t rr = 15;    // release rate, 0-15
    uint8_t d1l = 0;    // first decay level, 0-15
    uint8_t mul = 1;    // frequency multiple, 0-15 (0 = x0.5)
    int8_t dt1 = 0;     // fine detune, -3..+3
    uint8_t dt2 = 0;    // coarse detune, 0-3
    uint8_t ks = 0;     // key scaling, 0-3
    bool amEn = false;  // LFO amplitude modulation enable

    bool operator==(const FmOperator&) const = default;
};

enum class FmPan : uint8_t { Both = 0, Left = 1, Right = 2 };

// A complete voice patch. Operators are in signal order M1, C1, M2, C2,
// so algorithm 0 is op[0] -> op[1] -> op[2] -> op[3].
struct FmPatch
{
    std::string name = "Init";
    std::string group;    // the song a patch was imported from; empty for built-in patches
    uint8_t alg = 7;      // connection, 0-7
    uint8_t fb = 0;       // M1 self-feedback, 0-7
    uint8_t lfoFreq = 0;  // 0-255
    uint8_t lfoWave = 0;  // 0 saw, 1 square, 2 triangle, 3 noise
    uint8_t pmd = 0;      // phase modulation depth, 0-127
    uint8_t amd = 0;      // amplitude modulation depth, 0-127
    uint8_t pms = 0;      // channel PM sensitivity, 0-7
    uint8_t ams = 0;      // channel AM sensitivity, 0-3
    FmPan pan = FmPan::Both;
    std::array<FmOperator, 4> op {};

    bool operator==(const FmPatch&) const = default;
};

inline constexpr const char* kOperatorNames[4] = { "M1", "C1", "M2", "C2" };

// Which operators reach the output for each algorithm (bit i = op[i]).
inline constexpr uint8_t kCarrierMask[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };

inline constexpr const char* kAlgorithmDiagrams[8] = {
    "M1 > C1 > M2 > C2",
    "(M1 + C1) > M2 > C2",
    "(M1 + (C1 > M2)) > C2",
    "((M1 > C1) + M2) > C2",
    "(M1 > C1) + (M2 > C2)",
    "M1 > (C1 + M2 + C2)",
    "(M1 > C1) + M2 + C2",
    "M1 + C1 + M2 + C2",
};

const std::vector<FmPatch>& factoryPatches();

} // namespace opm86
