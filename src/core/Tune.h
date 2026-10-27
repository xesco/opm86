#pragma once

#include "SongLog.h"
#include "VgmImport.h"
#include "VgmSong.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opm86
{

// A song as the player shows it: tracks of notes over the split VGM log.
// A track is one part: an FM patch on one chip voice, or one drum sound. A
// note points at its part of the log and records only what was edited: its
// start, length, transposition and channel. Building the tune rewrites the
// edited notes' writes and leaves every other write as it was, so an unedited
// tune is the original log, and an edited note is still played by the game's
// own writes.

struct TuneNote
{
    int id = 0;            // unique in the tune, kept through edits
    int source = 0;        // index into the log's notes (FM) or hits (drums); copies share it
    int64_t start = 0;     // VGM samples
    int64_t length = 1;    // key on to key off; for a drum, until it is cut
    int transpose = 0;     // semitones
};

struct TuneTrack
{
    enum class Kind { Fm, Drum };
    Kind kind = Kind::Fm;
    int instrument = 0;    // FM: patch index; drums: sample index (from the importer)
    std::string name;
    bool mute = false, solo = false;
    uint16_t mutedVoices = 0; // FM: bit c mutes the notes the game played on chip channel c
    // Level in dB, 0 as the game played it. The chips set it: FM carriers'
    // total level, in 0.75 dB steps, or the PCM channel volumes. A boost goes
    // as far as the chip allows: a carrier at full level or a PCM volume at
    // 127 stays there.
    float volume = 0.0f;
    static constexpr float kSilent = -60.0f; // at or below, the track is silent
    static constexpr float kMaxVolume = 6.0f;
    std::vector<TuneNote> notes; // in order of start
};

// What a build did with each note, for the player to show.
struct BuildReport
{
    // Per track, per note: the chip channel it plays on, or -1 when every
    // channel was busy and it had to share one, cutting another note short.
    std::vector<std::vector<int>> channels;
    size_t conflicts = 0;
};

// A tune is cheap to copy: what never changes is shared between copies,
// and only the tracks are copied, so an edit can hand a copy to a builder.
class Tune
{
public:
    // Reads a .vgm or .vgz file. Returns nullptr and sets error when it holds
    // no YM2151 or SegaPCM writes.
    static std::unique_ptr<Tune> load(const std::vector<uint8_t>& file, const std::string& name, std::string& error);

    const VgmSong& source() const { return *data_->source; }
    std::shared_ptr<const VgmSong> sourcePointer() const { return data_->source; }
    const SongLog& log() const { return data_->log; }
    const std::vector<FmPatch>& patches() const { return data_->import.patches; }
    const std::vector<PadSample>& samples() const { return data_->import.samples; }

    std::vector<TuneTrack> tracks;

    // The note's original values in the log.
    int64_t originalStart(const TuneTrack& track, const TuneNote& note) const;
    int64_t originalLength(const TuneTrack& track, const TuneNote& note) const;
    int originalChannel(const TuneTrack& track, const TuneNote& note) const;

    // The MIDI key a note sounds at its key on, transposition included; for a
    // drum, 60 plus its transposition.
    int pitch(const TuneTrack& track, const TuneNote& note) const;
    // The pitch of a note while it is held, as a MIDI key with a fraction and
    // the transposition included. The first entry is the pitch at the key on,
    // and each later entry is a change the game makes without a new key on.
    // A drum has one pitch.
    std::vector<std::pair<int64_t, double>> bends(const TuneTrack& track, const TuneNote& note) const;

    bool audible(const TuneTrack& track) const;
    bool audible(const TuneTrack& track, const TuneNote& note) const; // with the voice mutes, and never a silent hit
    // A drum hit the game triggers at volume 0 on both sides: a driver
    // restarting a channel, which makes no sound.
    bool silentHit(const TuneTrack& track, const TuneNote& note) const;

    // The chip channels an FM track's notes came from, highest sounding first;
    // a chord part has one per note of the chord. Empty for a drum track.
    std::vector<int> voices(const TuneTrack& track) const;

    // An id no note in the tune uses yet, for a copy or a new note.
    int newNoteId() const;

    // The song with every edit applied, ready for the exact player. With mix
    // false, mute, solo and volume are left out, for the player to apply
    // with a ChannelMix (see mixOf).
    std::shared_ptr<VgmSong> build(BuildReport* report = nullptr, bool mix = true) const;

private:
    void splitParts(); // one track per FM patch and chip voice, in voice order

    // The registers a note needs from its channel, as the game had them at
    // its key on (FM) or trigger (drums), by register offset from the channel.
    struct FmState
    {
        std::array<uint8_t, 28> regs {};
    };
    struct PcmState
    {
        std::array<uint8_t, 8> regs {};
    };

    struct Data
    {
        std::shared_ptr<const VgmSong> source;
        SongLog log;
        VgmImport import;
        std::vector<FmState> fmState;   // per log note
        std::vector<PcmState> pcmState; // per log hit
        std::vector<int64_t> hitLength; // per log hit: until cut or ended
        double clockShift = 0.0;        // semitones the chip's clock adds
    };
    std::shared_ptr<const Data> data_;
};

} // namespace opm86
