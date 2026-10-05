#pragma once

#include "FmPatch.h"
#include "PcmBank.h"
#include "VgmImport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opm86
{

// A song as tracks of notes, the form the tracker edits. Times are in VGM
// samples (1/44100 s). Each track is one instrument: an FM patch or one
// drum sound. A note carries what the game did while it played, relative
// to its own start, so it can be moved, stretched or transposed whole.

// An FM note's patch, level and output from a point in the note.
struct NoteChange
{
    int64_t offset = 0;   // from the note's start; may run past its end, into the release
    int patch = 0;        // index into Arrangement::patches
    int attenuation = 0;  // 0.75 dB steps below the patch's loudest level
    FmPan pan = FmPan::Both;
};

// A drum note's volume, output and pitch from a point in the hit.
struct HitChange
{
    int64_t offset = 0;
    int left = 0, right = 0; // chip volumes, 0-127
    uint8_t step = 0x80;     // chip step: the sample's playback pitch
};

struct ArrNote
{
    int64_t start = 0;
    int64_t length = 1;
    // FM: the MIDI key written for the note. Drums: 60 plays the sound at its
    // pitch in the game; each step above or below shifts it a semitone.
    int pitch = 60;
    int voice = -1; // the game's channel for the note, kept when free; -1 for none
    // FM: the sounding pitch in semitones from `pitch`, from offset 0 on.
    std::vector<std::pair<int64_t, double>> bend { { 0, 0.0 } };
    std::vector<NoteChange> changes { NoteChange {} }; // FM, first at offset 0
    std::vector<HitChange> hit;                         // drums, first at offset 0
    double fine = 0.0; // drums: semitones on top of the pitch and the steps, a hit's fine tune

    int64_t end() const { return start + length; }
};

struct ArrTrack
{
    enum class Kind { Fm, Drum };
    Kind kind = Kind::Fm;
    int source = 0;      // FM: patch index; drums: sample index
    std::string name;
    bool mute = false, solo = false;
    std::vector<ArrNote> notes; // in order of start
};

struct Arrangement
{
    std::string name;
    std::vector<FmPatch> patches;
    std::vector<PadSample> samples;
    double beatLength = kVgmRate / 2.0; // VGM samples per beat
    double firstBeat = 0.0;             // song time of beat 0
    int beatsPerBar = 4;                // beats in a bar, each a quarter note
    int64_t length = 0;                 // VGM samples
    std::vector<ArrTrack> tracks;

    bool audible(const ArrTrack& track) const;
};

// One track per FM patch and one per drum sound, most used first.
Arrangement arrangementFromVgm(const VgmImport& song, const std::string& name);

// The MIDI that plays an arrangement on the engine: timed messages, plus
// the setup that must reach the engine before any of them.
struct ArrangementEvent
{
    int64_t time = 0; // VGM samples
    int order = 0;    // at one time: note offs, then settings, then note ons
    int track = 0;
    uint8_t bytes[3] {};
    uint8_t size = 3;
};

struct ArrangementMidi
{
    std::vector<ArrangementEvent> setup;   // bend ranges and drum gating
    std::vector<ArrangementEvent> events;  // in order of time
    std::vector<std::vector<int>> channels; // MIDI channels (0-15) each track uses
    int bendRange = 24;
    size_t skippedHits = 0;                 // drum hits of sounds beyond the 16 pads
};

// audibleOnly leaves out muted tracks, or every track but the soloed ones.
ArrangementMidi arrangementMidi(const Arrangement& arrangement, bool audibleOnly);

// The pads an arrangement's drums play on: its first 16 sounds.
PadSet arrangementPads(const Arrangement& arrangement);

} // namespace opm86
