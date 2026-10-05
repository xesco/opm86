#pragma once

#include "Chip.h"
#include "SongLog.h"
#include "VgmImport.h"
#include "VgmSong.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opm86
{

// A song as the tracker edits it: tracks of notes over the split VGM log.
// A track is one part: an FM patch on one chip voice, or one drum sound. A
// note points at its part of the log and records only what was edited: its
// start, length, transposition, voice and expression (velocity, fine tune
// and pan). A ripped note plays on the game's
// chip channel unless it was moved to another voice; a written note records
// the voice it plays on. Building the tune rewrites the
// edited notes' writes and leaves every other write as it was, so an unedited
// tune is the original log, and an edited note is still played by the game's
// own writes.
//
// A written note has no part of the log: the tracker wrote it. It plays the
// track's instrument at its key, on its voice, and the build writes its
// registers, key on and key off. A written drum hit plays the track's
// sample, as edited, at its key on a free SegaPCM channel: the build writes
// its registers and trigger, lets a one-shot sample play out and cuts the
// sample at the hit's end when that comes first.

struct TuneNote
{
    int id = 0;            // unique in the tune, kept through edits
    int source = 0;        // index into the log's notes (FM) or hits (drums); copies share it
    int64_t start = 0;     // VGM samples
    int64_t length = 1;    // key on to key off; for a drum, until it is cut
    int transpose = 0;     // semitones; a written note's key is 60 plus this
    int voice = -1;        // the chip channel it plays on, FM 0-7; -1 for a ripped note on the game's channel, or a written drum hit on any free channel
    // The note's expression, set in the tracker; each at its default plays
    // the note as written or as the game played it. A ripped note's writes
    // after its key on keep their moves around it: a fade still fades from
    // the new velocity, and a bend still bends from the new fine pitch.
    int velocity = 0;       // level in 0.75 dB steps, kMinVelocity (-48 dB) to 0: FM carriers' total level, or a drum hit's volumes
    int fine = 0;           // 64ths of a semitone on top of the transposition, -kMaxFine to kMaxFine
    std::optional<int> pan; // none as played; -kPanRange to kPanRange: FM out of the left side below 0, both at 0, the right above; a drum hit's balance

    static constexpr int kMinVelocity = -64;
    static constexpr int kMaxFine = 63;
    static constexpr int kPanRange = 64;

    bool written() const { return source < 0; } // source -1: written in the tracker
    float velocityDb() const { return static_cast<float>(velocity) * 0.75f; }
    // The output sides an FM note's pan gives: 0x40 left, 0x80 right, 0xC0 both.
    static uint8_t sides(int pan) { return pan < 0 ? 0x40 : pan > 0 ? 0x80 : 0xC0; }
};

// A drum track's sample as edited, applied to every hit of the track: where
// it starts and ends in the ROM, its loop, its pitch and its two volumes.
// A field left unset keeps what the game wrote for each hit, or for a
// written hit the sample's own (see PadSample). Addresses are in the
// sample's bank, as the chip's registers hold them; the end is a page,
// the last 256 bytes played.
struct SampleEdit
{
    std::optional<uint16_t> start;      // start address, registers 0x84 and 0x85
    std::optional<uint8_t> endPage;     // the last page played, register 0x06
    std::optional<bool> loop;           // loops back at the end, register 0x86 bit 1 clear
    std::optional<uint16_t> loopStart;  // where a loop starts again, registers 0x04 and 0x05
    std::optional<uint8_t> step;        // pitch at no transposition, register 0x07: step / 256 bytes a chip sample
    std::optional<uint8_t> left, right; // volumes 0-127, registers 0x02 and 0x03

    bool any() const { return start || endPage || loop || loopStart || step || left || right; }
    bool operator==(const SampleEdit&) const = default;
    // The edit with its addresses inside the sample's ROM range, the end after
    // the start and the loop between them, and the volumes at 127 at most.
    SampleEdit fitted(const PadSample& sample) const;
};

// Where a sample may play in its bank: its first address and the last page
// its data reaches.
uint16_t sampleFirst(const PadSample& sample);
uint8_t sampleLastPage(const PadSample& sample);

struct TuneTrack
{
    enum class Kind { Fm, Drum };
    Kind kind = Kind::Fm;
    int instrument = 0;    // FM: patch index; drums: sample index (from the importer); -1 for a track added in the tracker
    std::string name;
    bool mute = false, solo = false;
    uint16_t mutedVoices = 0;  // FM: bit c mutes the notes the game played on chip channel c
    uint16_t soloedVoices = 0; // FM: bit c solos them, as a track's solo does
    std::array<float, 8> voiceVolume {}; // FM: dB on top of the track's, for the notes on chip channel c; the player applies it
    std::optional<FmPatch> patch;        // FM: the instrument as edited; none, as imported. The player applies it
    uint8_t addedVoices = 0;              // FM: chip channels the track has as voices, notes on them or not
    std::array<uint8_t, 8> voiceSides {}; // FM: the output sides of the notes on chip channel c, 0x40 left, 0x80 right, 0xC0 both; 0 as the game. A note's own pan wins. The player applies it
    SampleEdit sample;                    // drums: the sample as edited; nothing set, as the game plays it. It needs a new build
    // Level in dB, 0 as the game played it. The chips set it: FM carriers'
    // total level, in 0.75 dB steps, or the PCM channel volumes. A boost goes
    // as far as the chip allows: a carrier at full level or a PCM volume at
    // 127 stays there.
    float volume = 0.0f;
    static constexpr float kSilent = -60.0f; // at or below, the track is silent
    static constexpr float kMaxVolume = 6.0f;
    std::vector<TuneNote> notes; // in order of start
};

// The song's beat grid and meter, for the tracker's ruler, snap and counters:
// beat k starts at firstBeat + k x beatLength, in VGM samples, and a bar
// has beatsPerBar beats, each a quarter note.
struct SongGrid
{
    static constexpr double kMinTempo = 20.0, kMaxTempo = 300.0; // beats a minute
    static constexpr int kMaxBeatsPerBar = 16;

    double beatLength = kVgmRate / 2.0;
    double firstBeat = 0.0;
    int beatsPerBar = 4;

    double tempo() const { return 60.0 * kVgmRate / beatLength; }
    double barLength() const { return beatLength * beatsPerBar; }
};

// A stretch of the song that playback repeats, from start to end in VGM
// samples, while it is on. Off, or with its end not after its start, the
// song plays through.
struct LoopRegion
{
    static constexpr int64_t kMinLength = 441; // 10 ms, the shortest loop

    int64_t start = 0, end = 0;
    bool on = false;

    bool valid() const { return end > start; }
    bool active() const { return on && valid(); }
    // The loop at least kMinLength long, its end moved later if need be.
    LoopRegion fitted() const
    {
        LoopRegion loop = *this;
        if (loop.valid() && loop.end - loop.start < kMinLength)
            loop.end = loop.start + kMinLength;
        return loop;
    }
};

// What a build did with each note, for the tracker to show.
struct BuildReport
{
    // Per track, per note: the chip channel it plays on, or -1 when every
    // channel was busy and it had to share one, cutting another note short.
    std::vector<std::vector<int>> channels;
    size_t conflicts = 0;
};

// A note written in the tracker sets 28 registers on its chip channel:
// output and connection, key code, key fraction, LFO sensitivity, then six
// registers for each of its four operators (slots ch, ch + 8, ch + 16,
// ch + 24). fmRegister gives one's address by its index in that order.
inline constexpr int kFmRegs = 28;
uint16_t fmRegister(int channel, int index);
// A written note's registers: the instrument at a MIDI key, out of both
// sides. A key between two keys sets the key fraction. The chip's clock
// moves every pitch by clockShift semitones (see Tune::clockShift), so the
// key code takes it off.
std::array<uint8_t, kFmRegs> writtenRegs(const FmPatch& patch, double key, double clockShift);
// A key code and key fraction moved by semitones and 64ths of a semitone,
// the fraction carrying into the key code, within the chip's range. With no
// fine move the fraction stays and the key code is transposed alone.
std::pair<uint8_t, uint8_t> shiftKey(uint8_t kc, uint8_t kf, int semitones, int fine);
// The operators a connection hears directly, its carriers: bit k for the
// slot block k of fmRegister's list (M1, M2, C1, C2). A note's velocity and
// a track's volume move their total level.
uint8_t carrierSlots(int connection);

// A drum hit sets eight registers on its SegaPCM channel: the left and right
// volumes, the loop address (low, high), the end page, the step and the start
// address (low, high). pcmRegister gives one's address by its index in that
// order. A hit as the build triggers it: those registers, and the flags it
// starts with (register 0x86: the bank bits, bit 1 set for no loop, bit 0
// clear to play).
inline constexpr int kPcmRegs = 8;
uint16_t pcmRegister(int channel, int index);
struct PcmVoice
{
    std::array<uint8_t, kPcmRegs> regs {};
    uint8_t flags = 0x02;
};
// The chip's step moved by semitones and 64ths of a semitone, from 1 to 255.
uint8_t transposeStep(uint8_t step, int semitones, int fine = 0);
// A drum hit's left and right volumes with its note's expression: the
// balance of its pan, when set, shares their sum between the sides, and
// its velocity scales both, up to 127.
std::array<uint8_t, 2> noteVolumes(int left, int right, const TuneNote& note);
// A value the game wrote, moved as the edit moves the value at the trigger,
// from 0 to high: a fade or a bend keeps its shape around the edited level
// or pitch.
uint8_t scaleTo(uint8_t value, uint8_t from, uint8_t to, int high);

// A note's own expression over its length: how its pitch and level move
// after its key on, as the driver wrote them. Each point holds from its
// offset until the next; before the first, the value is 0, the key on's.
// A curve has points only where its value changes, so it is empty for a
// note that keeps its key on's pitch or level.
struct NoteExpression
{
    struct Point
    {
        int64_t offset = 0; // VGM samples from the note's start
        float value = 0.0f;
    };
    std::vector<Point> pitch; // semitones from the pitch at the key on
    std::vector<Point> level; // dB from the level at the key on

    bool empty() const { return pitch.empty() && level.empty(); }
};

// The song's drum samples as edited: those loaded from audio files, in the
// order they were loaded, and what they make of the rip's samples and ROM.
// The ROM itself is not kept: it is the rip's, grown to romSize, with each
// loaded sample written at its place (see Tune::rom). The places are pages
// the rip's song never reads, so a hit of a sample replaced plays on as it
// did until the build moves it to the new one.
struct SampleSet
{
    std::vector<LoadedSample> loaded;
    std::vector<PadSample> samples; // the rip's, those loaded in place of some, then the new ones
    size_t romSize = 0;             // the ROM's size, with the banks the loaded samples added
};

// A tune is cheap to copy: what never changes is shared between copies,
// and only the tracks and the chip's edits are copied, so an edit can hand a
// copy to a builder. Its samples as edited are shared too: an edit of them
// makes a new set.
class Tune
{
public:
    // Reads a .vgm or .vgz file. Returns nullptr and sets error when it holds
    // no YM2151 or SegaPCM writes.
    static std::unique_ptr<Tune> load(const std::vector<uint8_t>& file, const std::string& name, std::string& error);
    // A song with no rip and no tracks, at a tempo in beats a minute in 4/4, to write.
    static std::unique_ptr<Tune> empty(double tempo = 120.0);
    bool ripped() const { return !data_->log.notes.empty() || !data_->log.hits.empty(); }

    const VgmSong& source() const { return *data_->source; }
    std::shared_ptr<const VgmSong> sourcePointer() const { return data_->source; }
    const SongLog& log() const { return data_->log; }
    const std::vector<FmPatch>& patches() const { return data_->import.patches; }
    // The drum samples as edited: the rip's, with those loaded from audio
    // files in place of some, then the new ones (see SampleSet).
    const std::vector<PadSample>& samples() const { return samples_ ? samples_->samples : data_->import.samples; }
    // The ROM the samples play from, made anew on each call: the rip's, with
    // the loaded samples in it. romByte reads one byte of it without making
    // it, 0x80 past its end.
    std::vector<uint8_t> rom() const;
    uint8_t romByte(size_t at) const;
    // The samples loaded from audio files, in the order loaded; and whether
    // the sample at an index is one of them.
    const std::vector<LoadedSample>& loadedSamples() const;
    bool sampleLoaded(int index) const;
    // Loads a sample's sound, as encodeSample makes it, in place of the
    // sample at index, or with an index past the samples as a new one after
    // them. Every loaded sample is placed in the ROM again, in the order
    // loaded. Returns the sample's index, or -1, changing nothing, when no
    // bank has room for it.
    int loadSample(int index, const PadSample& sample);
    // Every loaded sample at once, as a project keeps them: each index the
    // rip's or the next new one. Returns false, changing nothing, when one
    // does not fit or has no sound.
    bool setLoadedSamples(const std::vector<LoadedSample>& loaded);
    // The samples as edited, to keep and put back as they were, as undo does;
    // none for the rip's.
    const std::shared_ptr<const SampleSet>& sampleSet() const { return samples_; }
    void setSampleSet(std::shared_ptr<const SampleSet> set) { samples_ = std::move(set); }
    // What the importer read from the rip: its notes and hits, in the order of the log's.
    const VgmImport& imported() const { return data_->import; }
    // The instrument an FM track's notes are built with, before its edits:
    // the rip's patch, or for a track added in the tracker the default one.
    const FmPatch& basePatch(const TuneTrack& track) const;
    static const FmPatch& defaultPatch();
    // Semitones the song's YM2151 clock adds to every pitch, against the
    // usual 3.58 MHz; written notes take it off their key codes.
    double clockShift() const { return data_->clockShift; }
    // VGM samples up to the end of the song: the rip's, or the last written
    // note's. A song with no rip has eight bars at its tempo and meter to write in.
    int64_t length() const;

    std::vector<TuneTrack> tracks;
    // The chip's LFO and noise as edited, for the whole song; the player
    // applies it, so it needs no new build.
    ChipEdits chip;
    // The beat grid and meter: the grid fitted to the rip's notes, or 120
    // beats a minute for a song with no rip, in 4/4. It changes no write, so
    // it needs no new build.
    SongGrid grid;
    // The loop, off at first. It is not part of an edit: it is set as the
    // playhead is.
    LoopRegion loop;

    double beatLength() const { return grid.beatLength; } // VGM samples
    double firstBeat() const { return grid.firstBeat; }   // song time of beat 0, the start of bar 1
    int beatsPerBar() const { return grid.beatsPerBar; }
    double barLength() const { return grid.barLength(); }
    double tempo() const { return grid.tempo(); }         // beats a minute
    // A new tempo, in beats a minute, from kMinTempo to kMaxTempo. A rip's
    // music keeps the game's timing, so only the grid changes, its first beat
    // where it was. A song with no rip keeps its notes on their beats: their
    // starts, counted from the first beat, and their lengths scale with the
    // beat, and the song's length follows.
    void setTempo(double bpm);

    // The note's original values in the log.
    int64_t originalStart(const TuneTrack& track, const TuneNote& note) const;
    int64_t originalLength(const TuneTrack& track, const TuneNote& note) const;
    // The chip channel a note asks for: a written note's voice, or a ripped
    // note's, the game's unless it was moved to another voice.
    int originalChannel(const TuneTrack& track, const TuneNote& note) const;
    // Moves an FM note to a chip channel of its track. A ripped note moved
    // back to the game's channel has no voice of its own again.
    void moveToVoice(const TuneTrack& track, TuneNote& note, int channel) const;
    // An FM note as a written one on a chip channel, for another track: its
    // id, start and length, at the pitch it sounds at. A written note keeps
    // all but its voice.
    TuneNote writtenCopy(const TuneTrack& track, const TuneNote& note, int channel) const;
    // When an FM note's writes begin on the chip channel it plays on: its
    // start, or earlier for the set-up the driver writes before the key on,
    // which a note moved in time or to another channel does not have.
    int64_t firstWrite(const TuneTrack& track, const TuneNote& note, int channel) const;
    // The output sides an FM track's notes on a chip channel have in the game,
    // the most usual: 0x40 left, 0x80 right, 0xC0 both; 0 when none play there.
    uint8_t voiceSides(const TuneTrack& track, int channel) const;
    // An FM note's operator level as the game set it at the key on, by
    // operator in patch order (M1, C1, M2, C2): 0 loudest, 127 silent.
    int noteLevel(const TuneTrack& track, const TuneNote& note, int op) const;

    // The MIDI key a note sounds at its key on, transposition and fine tune
    // included, rounded to the nearest key; for a drum, 60 plus its
    // transposition, and its fine tune rounded.
    int pitch(const TuneTrack& track, const TuneNote& note) const;
    // The same pitch, unrounded: a ripped note's key fraction and every
    // note's fine tune in it.
    double exactPitch(const TuneTrack& track, const TuneNote& note) const;
    // A ripped note's expression as the build plays it: its writes after the
    // key on, transposed and, past its original length, moved with its end,
    // up to its end. FM pitch comes from the key code and fraction, and level
    // from the loudest carrier's total level under the connection at each
    // moment; a drum hit's pitch comes from its step, and its level from the
    // louder of its two volumes, with the track's sample edit over them.
    // The track's volume and patch edits, and the note's velocity and fine
    // tune, move every level or pitch alike, so they leave the curves as
    // they are. A written note has no writes after its key on, so its curves
    // are empty.
    NoteExpression expression(const TuneTrack& track, const TuneNote& note) const;

    // A drum track's sample, by its instrument, as edited; nullptr for none.
    const PadSample* sample(const TuneTrack& track) const;
    // A drum track's sample as a written hit plays it, transposed by
    // semitones: the track's edit over the sample's own settings.
    PcmVoice sampleVoice(const TuneTrack& track, int transpose, int fine = 0) const;
    // How a drum hit starts on the chip: a written hit as sampleVoice has
    // it, a ripped one with the game's registers and the track's edit over
    // them. With expressed, the note's fine tune moves the step, and its
    // velocity and pan its volumes (see noteVolumes), as the build triggers it.
    PcmVoice hitVoice(const TuneTrack& track, const TuneNote& note, bool expressed = false) const;
    // VGM samples a written hit of the track plays for, transposed and fine
    // tuned, before the sample ends by itself; 0 for a sample that loops.
    int64_t sampleLength(const TuneTrack& track, int transpose, int fine = 0) const;

    bool audible(const TuneTrack& track) const;
    bool audible(const TuneTrack& track, const TuneNote& note) const; // with the voice mutes

    // The chip channels an FM track's notes came from, and those added to it,
    // highest sounding first; a chord part has one per note of the chord.
    // Empty for a drum track.
    std::vector<int> voices(const TuneTrack& track) const;
    // The lowest of an FM track's voices that none of its notes plays on
    // from start up to end, for a new note there; else its lowest voice.
    // -1 for a drum track, or a track with no voices.
    int freeVoice(const TuneTrack& track, int64_t start, int64_t end) const;

    // An id no note in the tune uses yet, for a copy or a new note.
    int newNoteId() const;

    // The song with every edit applied, ready for the exact player. With mix
    // false, mute, solo and volume are left out, for the player to apply
    // with a ChannelMix (see mixOf).
    std::shared_ptr<VgmSong> build(BuildReport* report = nullptr, bool mix = true) const;
    // The channels a build gives the notes, without building.
    BuildReport channels() const;

    // Moves every FM note the build gives another channel, because the one
    // it asks for is busy, to the channel it plays on, so its row shows
    // where it sounds; the build stays the same. That channel joins the
    // track as a voice, and a voice its notes leave stays with its track.
    // Returns how many notes moved, and sets stuck to how many have no free
    // channel at all: every channel is busy when they start, so no move
    // gives them one.
    size_t moveConflicts(size_t& stuck);

private:
    struct Ref
    {
        size_t track, note;
    };
    // The notes of one chip in order of start, each kept on its channel when
    // free, else moved to the channel free longest; the report gets each
    // one's channel, or -1 when none is free. Written drum hits come after
    // the game's, into the time their channels are free, so they move none
    // of the game's hits.
    std::vector<Ref> allocate(bool fm, BuildReport& report) const;
    int logChannel(const TuneTrack& track, const TuneNote& note) const; // the game's channel, or a written note's voice
    PcmVoice rippedVoice(const TuneTrack& track, const TuneNote& note, int fine) const; // a ripped hit's start, as hitVoice has it, fine tuned
    // A drum track's sample loaded in place of the rip's, which its ripped
    // hits play instead; nullptr when the rip's is as it was.
    const PadSample* replacement(const TuneTrack& track) const;
    // The sample edit a ripped hit plays with: the track's, inside its
    // sample's range. A hit of a replaced sample plays the new one's range
    // once, unless the track's edit sets its own, at the game's step moved
    // as far as the new sample's own step is from the rip's.
    SampleEdit rippedEdit(const TuneTrack& track, const TuneNote& note) const;
    uint8_t withBank(const TuneTrack& track, uint8_t flags) const; // a ripped hit's flags, with the bank of its sample's replacement

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
    std::shared_ptr<const SampleSet> samples_; // none: the rip's samples and ROM
};

} // namespace opm86
