#pragma once

#include "Tune.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <optional>
#include <vector>

// A project: the rip a song comes from and everything changed on it, kept in
// a compact binary file (.opm86), or exported as readable JSON. It holds the rip's place, relative to the
// project and in full, or none for a song written from empty, and every
// track as it stands: its notes, its mix (mute,
// solo, volume, and its voices' mute, solo, volume and output sides) and its
// instrument as edited, or for a drum track its sample as edited, the chip's
// LFO and noise as edited, the song's beat grid, meter and loop, and the
// samples loaded from audio files, their sound as the chip plays it (raw
// bytes in binary, base64 in JSON).
namespace opm86::project
{

inline constexpr const char* kExtension = ".opm86";

bool save(const Tune& tune, const juce::File& rip, const juce::File& to, juce::String& error);       // binary
bool exportJson(const Tune& tune, const juce::File& rip, const juce::File& to, juce::String& error); // the same, as JSON
bool isBinary(const juce::File& file); // a project saved in binary, not JSON

struct Contents
{
    juce::File rip;                 // the rip the project's tracks belong to; none for a song written from empty
    std::vector<TuneTrack> tracks;  // as saved, to check against the rip (see fits)
    ChipEdits chip;                 // the fields saved; one missing, as an older project has it, is unset
    std::optional<SongGrid> grid;   // none in an older project: the tune's own then
    LoopRegion loop;                // off in an older project
    std::vector<LoadedSample> samples; // in the order loaded; none in an older project
};
std::optional<Contents> read(const juce::File& from, juce::String& error); // either form

// Whether saved tracks belong to a tune loaded from their rip: every note's
// source within the rip's notes or hits, every instrument within its patches
// or samples; written notes and FM tracks added in the tracker fit any tune.
bool fits(const Tune& tune, const std::vector<TuneTrack>& tracks, juce::String& error);

// A project opened as the tracker opens it: its rip loaded, its samples
// loaded into it, the saved tracks checked against it (see fits) and put in
// place of the rip's, with the chip's edits, the grid and the loop. Sets rip to the rip's file, or to none
// for a song written from empty. Returns nullptr and sets error when the
// project, its rip or its tracks cannot be read.
std::unique_ptr<Tune> load(const juce::File& from, juce::File& rip, juce::String& error);

// A project (.opm86 or JSON), or a VGM rip opened alone, as a tune.
std::unique_ptr<Tune> open(const juce::File& file, juce::String& error);

} // namespace opm86::project
