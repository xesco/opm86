#pragma once

#include "FmPatch.h"

#include <string>
#include <vector>

namespace opm86
{

// VOPM's .opm instrument banks, the plain text that DefleMask, Furnace and
// chipsynth MD read too. Each voice is a block of lines:
//
//   @:0 Name
//   LFO: LFRQ AMD PMD WF NFRQ
//   CH: PAN FL CON AMS PMS SLOT NE
//   M1: AR D1R D2R RR D1L TL KS MUL DT1 DT2 AMS-EN
//   C1: ...   M2: ...   C2: ...
//
// with "//" starting a comment. DT1 is the register's value (4-7 detune
// down), SLOT the key on's operator bits (8 M1, 16 C1, 32 M2, 64 C2), and
// AMS-EN 0 or 128.
struct OpmVoice
{
    int number = 0;         // the voice's number in the bank, 0-127
    FmPatch patch;          // its name, settings and LFO; a field missing keeps FmPatch's default
    bool lfo = false;       // the voice has an LFO line
    bool noise = false;     // NE, the chip's noise on
    uint8_t noiseFreq = 0;  // NFRQ, 0-31
};

// The voices of a bank, in the order the file has them. A line that is not
// one of the above is skipped, a field missing keeps its default, and a value
// out of range is clamped to the field's register width.
std::vector<OpmVoice> parseOpm(const std::string& text);

// A bank of one voice, numbered 0, with the patch's settings and LFO, its
// operators switched off left out of SLOT, both output sides and the noise off.
std::string writeOpm(const FmPatch& patch);

} // namespace opm86
