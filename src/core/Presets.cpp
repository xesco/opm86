#include "FmPatch.h"

namespace opm86
{

namespace
{

FmOperator op(uint8_t tl, uint8_t ar, uint8_t d1r, uint8_t d2r, uint8_t rr, uint8_t d1l,
              uint8_t mul, int8_t dt1 = 0, uint8_t dt2 = 0, uint8_t ks = 0, bool amEn = false)
{
    return FmOperator { tl, ar, d1r, d2r, rr, d1l, mul, dt1, dt2, ks, amEn };
}

FmPatch patch(const char* name, uint8_t alg, uint8_t fb, FmOperator m1, FmOperator c1,
              FmOperator m2, FmOperator c2)
{
    FmPatch p;
    p.name = name;
    p.alg = alg;
    p.fb = fb;
    p.op = { m1, c1, m2, c2 };
    return p;
}

FmPatch withVibrato(FmPatch p, uint8_t freq, uint8_t pmd, uint8_t pms)
{
    p.lfoFreq = freq;
    p.lfoWave = 2;
    p.pmd = pmd;
    p.pms = pms;
    return p;
}

std::vector<FmPatch> buildFactoryPatches()
{
    // Approximations of the sounds heard in the game, written by ear.
    // Operators are in signal order: M1, C1, M2, C2.
    return {
        withVibrato(patch("Shower Lead", 4, 6,
                          op(30, 20, 6, 0, 7, 2, 1, -1), op(2, 22, 4, 1, 7, 1, 1, -1),
                          op(34, 20, 6, 0, 7, 2, 1, 2), op(4, 22, 4, 1, 7, 1, 1, 2)),
                    200, 12, 3),

        patch("Slap Bass", 4, 5,
              op(28, 31, 12, 4, 10, 4, 1), op(4, 31, 8, 3, 9, 2, 1),
              op(36, 31, 18, 8, 10, 6, 3), op(8, 31, 6, 2, 9, 3, 1, 1)),

        patch("Beach Piano", 4, 3,
              op(40, 31, 10, 6, 8, 5, 14, 0, 0, 2), op(6, 31, 5, 3, 7, 3, 1),
              op(30, 31, 9, 4, 8, 4, 1), op(4, 31, 4, 2, 7, 2, 1, 1)),

        patch("Breeze Bell", 4, 0,
              op(32, 31, 8, 4, 5, 3, 7), op(4, 31, 4, 2, 5, 2, 2),
              op(38, 31, 10, 5, 5, 4, 5, 1), op(8, 31, 5, 3, 5, 3, 1)),

        withVibrato(patch("Sunset Strings", 4, 4,
                          op(36, 12, 2, 0, 6, 1, 1, -2), op(4, 11, 2, 0, 6, 1, 1, -2),
                          op(38, 12, 2, 0, 6, 1, 1, 2), op(6, 11, 2, 0, 6, 1, 1, 2)),
                    190, 10, 2),

        patch("Coin Organ", 7, 0,
              op(10, 31, 0, 0, 10, 0, 1), op(14, 31, 0, 0, 10, 0, 2),
              op(18, 31, 0, 0, 10, 0, 4), op(22, 31, 0, 0, 10, 0, 8)),

        patch("Init Sine", 7, 0,
              op(127, 31, 0, 0, 7, 0, 1), op(127, 31, 0, 0, 7, 0, 1),
              op(127, 31, 0, 0, 7, 0, 1), op(0, 31, 0, 0, 7, 0, 1)),
    };
}

} // namespace

const std::vector<FmPatch>& factoryPatches()
{
    static const std::vector<FmPatch> patches = buildFactoryPatches();
    return patches;
}

} // namespace opm86
