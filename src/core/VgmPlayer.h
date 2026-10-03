#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opm86
{

struct VgmRender
{
    std::string error; // empty on success; a late error keeps the audio before it
    std::vector<float> left, right;
};

// Replay a VGM file's register writes into the emulated chips, as the game
// played them, and mix the result like the engine does. It is the reference
// a converted MIDI file is compared against. Where the log ends, the game
// would loop; the render releases every note there instead, as the
// converted MIDI file does.
VgmRender renderVgm(const std::vector<uint8_t>& file, double sampleRate, double tailSeconds);

} // namespace opm86
