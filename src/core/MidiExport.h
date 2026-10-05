#pragma once

#include "Arrangement.h"
#include "VgmImport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opm86
{

// Controllers at the start of a track that set up the plugin to play it,
// after CC 87-90 name the song (see SongTrack.h): CC 102 loads the song's
// patch of that number, CC 103 loads the song's drums onto the pads.
inline constexpr int kPatchSelectController = 102;
inline constexpr int kKitSelectController = 103;

// A standard MIDI file made from a VGM import, and what went into it.
struct MidiExport
{
    struct Track
    {
        std::string name;
        std::vector<int> channels; // MIDI channels, 1-16
        int patch = 0;             // patch number, from 1; 0 for the drums
        size_t notes = 0;
    };

    std::vector<uint8_t> file;
    std::vector<Track> tracks; // the drum track, when there is one, is last
    size_t skippedHits = 0;    // hits of samples beyond the 16 pads
    double tempo = 120.0;      // BPM written to the file
    uint32_t leadIn = 0;       // VGM samples before the song's time 0
};

// One track per instrument: an FM patch, whose notes keep their YM2151
// channel's MIDI channel when free, or one drum sound on MIDI channel 10,
// whose notes play its pad (36 + sound). The file runs at the song's tempo
// and meter with a bar of lead-in, so the notes sit on a music program's
// beats. A note's level is its velocity, at the synth's default velocity
// sensitivity; slides, panning and later level changes are controllers. Each
// track sets, with RPN 0, a pitch bend range wide enough for the song's
// widest slide.
MidiExport exportMidi(const VgmImport& song, const std::string& title);
MidiExport exportMidi(const Arrangement& arrangement);

} // namespace opm86
