#pragma once

#include "FmPatch.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

// Instruments you save, to load onto any track later: one readable JSON file
// each, in the app's folder. A saved instrument holds the patch's own
// settings: the algorithm, feedback, the LFO sensitivities and every
// operator's settings. It also keeps the LFO it was saved with, which belongs
// to the whole chip, for a load to offer to the chip. The output sides belong
// to a song's voices, so they are not kept.
namespace opm86::instruments
{

juce::File folder();                                      // ~/Library/Application Support/OPM86 Tracker/Instruments
juce::File fileFor(const juce::String& name);             // where an instrument of that name is kept
bool save(const FmPatch& patch, const juce::String& name, juce::String& error);
std::vector<juce::File> list();                           // the saved instruments, by name
// The settings above; the rest as FmPatch's defaults. lfo, when given, says
// whether the file keeps an LFO: one saved before instruments kept it has none.
std::optional<FmPatch> load(const juce::File& file, bool* lfo = nullptr);
juce::var toJson(const FmPatch& patch, const juce::String& name);        // those settings, as a saved file holds them
std::optional<FmPatch> fromJson(const juce::var& json, bool* lfo = nullptr); // and back

} // namespace opm86::instruments
