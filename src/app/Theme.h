#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86::theme
{

// Out Run sunset: night-blue panels, sunset orange and magenta accents,
// cyan highlights.
inline const juce::Colour background { 0xff0b0e1a };
inline const juce::Colour panel { 0xff141a30 };
inline const juce::Colour panelLight { 0xff1b2340 };
inline const juce::Colour grid { 0xff232b4d };
inline const juce::Colour gridBar { 0xff39436f };
inline const juce::Colour orange { 0xffff8a3d };
inline const juce::Colour magenta { 0xffff4f9a };
inline const juce::Colour cyan { 0xff3de0ff };
inline const juce::Colour text { 0xffe8e6ff };
inline const juce::Colour dim { 0xff8d8fc4 };
inline const juce::Colour faint { 0xff4c5280 };
inline const juce::Colour danger { 0xffff3b4e };

// Press Start 2P for titles and counters; JetBrains Mono for everything else.
juce::Font pixel(float height);
juce::Font mono(float height, bool bold = false);

// Twelve pitch-class colours at matched brightness, so no note stands out
// by its pitch alone; C is red-orange, going round the colour wheel.
juce::Colour pitchColour(int midiNote);

// A colour per instrument, cycling through the accents.
juce::Colour instrumentColour(int index);

// Tracker-style note names: "C-4" is middle C (MIDI 60), "C#4" the next.
juce::String noteName(int midiNote);

// A middle dot between two spaced items.
inline juce::String dot() { return juce::String::fromUTF8("  \xc2\xb7  "); }

} // namespace opm86::theme
