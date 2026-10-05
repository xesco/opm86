#pragma once

#include "Arrangement.h"
#include "MidiExport.h"
#include "SongPlayer.h"
#include "Tune.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace opm86
{

// The song as the tracker plays it, with every edit in its writes, for a
// file that any VGM player plays. The tracker builds a song without its mix
// and applies the mix, the edited instruments and the chip's edits as it
// plays; here they become writes. The edits go in exactly as the player
// applies them (see editedWrite): the same rules, at the same writes, by the
// same channel owners. The mix becomes what the chips can do: a silent FM
// channel loses its output sides, and another channel's gain moves its
// carriers' total level, by the connection as edited, in the chip's 0.75 dB
// steps; a PCM channel's gain scales its volumes. So the song can differ
// from the tracker's where the chips run out: a level rounds to its 0.75 dB
// step, a boost stops at a carrier's full level or a PCM volume of 127, a cut
// stops at a carrier's total level of 127, and a PCM volume rounds to a whole
// number. It also differs in timbre on connection 7, where M1 is a carrier
// that feeds back into itself: with feedback on, a gain that moves M1's level
// also moves its feedback, while the tracker's gain moves only the level. A
// mix the tracker changes while it plays glides in over 20 ms; here every
// change is at once, as it is when playback starts.
std::shared_ptr<VgmSong> bakeSong(const VgmSong& song, const ChannelMix* mix, const PatchEdits* edits);

// A tune's song with every edit baked in (see bakeSong), by the same build,
// mix and edits the tracker plays.
std::shared_ptr<VgmSong> bakedSong(const Tune& tune);

// A VGM 1.51 file of a song: a header with its chips' clocks and the SegaPCM
// interface, the sample memory, the logged writes to both chips with the
// waits between them, and the end at the song's length. With gzip, a .vgz.
// The writes that end a song in the tracker (see appendSongEnd) are left
// out, since loadVgmSong adds them again; a shortened drum's stop becomes a
// write of its channel's flags. The title goes in the GD3 tag.
std::vector<uint8_t> writeVgm(const VgmSong& song, const std::string& title, bool gzip);

// A WAV file's 44-byte header: stereo PCM of 16 or 24 bits.
std::vector<uint8_t> wavHeader(uint32_t rate, int bits, uint32_t frames);

// Writes a stereo WAV file as samples come, and sets its sizes when it closes.
class WavWriter
{
public:
    WavWriter() = default;
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter();

    bool open(const std::string& path, uint32_t rate, int bits); // bits 16 or 24
    // Samples from -1 to 1, rounded to the file's size; louder ones are clipped.
    void write(const float* left, const float* right, int count);
    bool close(); // false when a write failed
    uint32_t frames() const { return frames_; }

private:
    std::FILE* file_ = nullptr;
    uint32_t rate_ = 44100;
    int bits_ = 16;
    uint32_t frames_ = 0;
    bool failed_ = false;
    std::vector<uint8_t> buffer_;
};

// The tune played offline as the tracker plays it: its song built without
// the mix, played with the same mix, edited instruments and chip, at speed 1.
// It runs from the start through the song's end and on through the
// releases until they fall silent, at the latest at songEnd, where the
// tracker stops. With loopOnly and the tune's loop on, it plays the loop once.
class TuneRenderer
{
public:
    TuneRenderer(const Tune& tune, double rate, bool loopOnly);
    ~TuneRenderer();

    // Builds the song and runs the chips up to the start, a stretch at a
    // time while keepGoing returns true. Returns false when stopped first.
    bool prepare(const std::function<bool()>& keepGoing);
    // The next samples, up to count; 0 at the end.
    int render(float* left, float* right, int count);
    double progress() const; // 0 to 1
    bool loop() const { return loop_; } // it plays the loop

private:
    Tune tune_;
    double rate_;
    bool loop_;
    std::shared_ptr<const VgmSong> song_;
    std::shared_ptr<const ChannelMix> mix_;
    std::shared_ptr<const PatchEdits> edits_;
    std::unique_ptr<SongPlayer> player_;
    int64_t done_ = 0, total_ = 0, songEnd_ = 0; // output samples: rendered, at most, and up to the song's end
    int64_t quiet_ = 0;                          // the silent samples since the last sound
    bool started_ = false, finished_ = false;
};

// The tune's notes as a standard MIDI file (see exportMidi): a track for
// each track heard, named as it is, its notes as they are now, with their
// starts, lengths and pitches, at the song's tempo and meter. A ripped note
// keeps the game's slides and level changes. MIDI does not carry the sound:
// a track selects its rip's patch or drums by number, so an edited
// instrument, the chip's LFO and noise, and the mix are left out.
Arrangement arrangementFromTune(const Tune& tune, const std::string& title);
MidiExport exportMidi(const Tune& tune, const std::string& title);

} // namespace opm86
