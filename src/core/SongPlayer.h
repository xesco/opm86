#pragma once

#include "Chip.h"
#include "FmPatch.h"
#include "Resampler.h"
#include "SegaPcm.h"
#include "VgmSong.h"

#include <ymfm_opm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <cstdint>
#include <memory>
#include <vector>

namespace opm86
{

// A YM2151 whose output can leave channels out. Every channel still runs,
// so the ones played sound exactly as in the full mix.
class MaskedYm2151 : public ymfm::ym2151
{
public:
    using ymfm::ym2151::ym2151;
    // With peaks (one per channel), each channel's louder side raises its
    // peak. Output only reads the chip, so measuring leaves the mix exact.
    void generate(output_data* output, uint32_t channelMask, int32_t* peaks = nullptr)
    {
        m_fm.clock(fm_engine::ALL_CHANNELS);
        if (peaks)
            for (uint32_t ch = 0; ch < 8; ++ch)
                if ((channelMask >> ch) & 1)
                {
                    output_data one;
                    m_fm.output(one.clear(), 0, 32767, 1u << ch);
                    peaks[ch] = std::max({ peaks[ch], std::abs(one.data[0]), std::abs(one.data[1]) });
                }
        m_fm.output(output->clear(), 0, 32767, channelMask);
        output->roundtrip_fp();
        // The chip keeps each channel's feedback from its own output, so the
        // channels left out still compute theirs, unheard: when they come
        // back they sound as they would have all along.
        if ((channelMask & 0xFF) != 0xFF)
        {
            output_data unheard;
            m_fm.output(unheard.clear(), 0, 32767, ~channelMask & 0xFF);
        }
    }

    // Some channels' output (bit n for channel n), alone, for a display; and
    // a channel's four operators' envelopes (in patch order M1, C1, M2, C2):
    // the level, 1 at full and 0 silent, and the stage (1 attack, 2 decay,
    // 3 sustain, 4 release).
    int32_t channelsOutput(uint32_t channels) const
    {
        output_data some;
        m_fm.output(some.clear(), 0, 32767, channels);
        return (some.data[0] + some.data[1]) / 2;
    }
    void envelopes(uint32_t ch, float* levels, int* stages) const
    {
        for (uint32_t op = 0; op < 4; ++op)
        {
            const auto* o = m_fm.debug_channel(ch)->debug_operator(op);
            levels[op] = 1.0f - static_cast<float>(o->debug_eg_attenuation()) / 1023.0f;
            stages[op] = static_cast<int>(o->debug_eg_state());
        }
    }

    // Each channel at its own gain (one per channel), then through the
    // chip's output stage; the peaks are after the gain. Every channel is
    // computed, as above.
    void generate(output_data* output, uint32_t channelMask, const float* gains, int32_t* peaks)
    {
        m_fm.clock(fm_engine::ALL_CHANNELS);
        float left = 0, right = 0;
        for (uint32_t ch = 0; ch < 8; ++ch)
        {
            output_data one;
            m_fm.output(one.clear(), 0, 32767, 1u << ch);
            if (((channelMask >> ch) & 1) && gains[ch] > 0.0f)
            {
                const float l = static_cast<float>(one.data[0]) * gains[ch], r = static_cast<float>(one.data[1]) * gains[ch];
                left += l, right += r;
                if (peaks)
                    peaks[ch] = std::max({ peaks[ch], static_cast<int32_t>(std::abs(l)), static_cast<int32_t>(std::abs(r)) });
            }
        }
        output->data[0] = static_cast<int32_t>(std::clamp(left, -65536.0f, 65536.0f));
        output->data[1] = static_cast<int32_t>(std::clamp(right, -65536.0f, 65536.0f));
        output->roundtrip_fp();
    }
};

// The chips' state at points through a song, so playback can start
// anywhere without running the song from its beginning.
struct SongSnapshots
{
    struct Fm
    {
        uint64_t sample;   // chip sample this state comes before
        size_t write;      // next write to apply
        std::vector<uint8_t> state;
        std::array<uint8_t, 256> regs {}; // every register as the song last wrote it, the vibrato depth at 0x1A (see shadow)
        std::array<uint8_t, 8> keys {};   // per channel, its operators as the song last keyed them (0x08's bits 3-6)
    };
    struct Pcm
    {
        uint64_t sample;
        size_t write;
        SegaPcm chip;
    };
    std::vector<Fm> fm;
    std::vector<Pcm> pcm;
};

// Run the song once and keep its state every quarter second. It takes a
// few seconds for a long song, so it belongs on a background thread.
std::shared_ptr<SongSnapshots> makeSnapshots(const VgmSong& song);

// Continue the snapshots from the last one (or from the start) up to a song
// time in VGM samples.
void extendSnapshots(const VgmSong& song, SongSnapshots& snapshots, double until);

// The snapshots of an earlier version of a song that still hold for a new
// one: those taken before the first write that differs, given by its index
// in each chip's write list.
std::shared_ptr<SongSnapshots> keepSnapshots(const SongSnapshots& previous, size_t ymUnchanged, size_t pcmUnchanged);

// The song time snapshots run to: the end of the song and its releases.
double songEnd(const VgmSong& song);

// Only the state at the start: enough to play the song from its beginning.
std::shared_ptr<SongSnapshots> makeStartSnapshot(const VgmSong& song);

// The chip-wide settings as the song has written them by a song time, in
// VGM samples, from the last snapshot before it.
ChipState chipAt(const VgmSong& song, const SongSnapshots& snapshots, double songTime);

// Edited instruments, for the player to apply as it writes to the chip: an
// instrument's registers on whichever FM channel plays its notes take the
// edited patch's values. A carrier's or modulator's level keeps the song's
// change from the instrument as imported, so a note's own volume stays.
// The chip's edited settings take the place of the song's writes to them.
struct PatchEdits
{
    // Per FM chip channel, in order of song time: the track whose notes it
    // plays from then on, from the first write of each note, with kNotePan
    // added for a note that sets its own pan.
    std::array<std::vector<std::pair<double, int>>, 8> owner;
    // A note's pan wins over its voice's output sides as edited.
    static constexpr int kNotePan = 1 << 20;
    struct Edit
    {
        FmPatch edited, original;
        std::array<uint8_t, 8> sides {}; // per channel, the output sides as edited (0x40 left, 0x80 right, 0xC0 both); 0 as the song
    };
    std::vector<std::optional<Edit>> tracks; // per track, its edit if it has one
    ChipEdits chip;                          // the LFO and noise, field by field
    // The patch window's audition mutes: one track's operators (bit i for
    // op[i]) left out of every key on, as a switched-off operator is, to hear
    // the others alone. Never part of the tune, so never saved or exported.
    int mutedTrack = -1;
    uint8_t muted = 0;
};

// The edits' rules for the YM2151's writes. The player applies them as it
// plays, and bakeSong writes them into a song, so the two cannot differ.
// owner holds, per FM channel, the track whose notes it plays now, or -1,
// with PatchEdits::kNotePan added while its note sets its own pan.

// A YM2151 write kept in a register shadow, as the chip keeps it. Register
// 0x19 takes the tremolo depth, or with bit 7 set the vibrato depth, which
// the shadow keeps at the unused 0x1A, as ymfm does; a write to 0x1A itself
// goes nowhere. A key on keeps the operators it keys on its channel.
void shadowWrite(std::array<uint8_t, 256>& regs, std::array<uint8_t, 8>& keys, const VgmWrite& w);

// A write as the edits have it; with none, as the song has it.
uint8_t editedWrite(const PatchEdits* edits, const std::array<int, 8>& owner, uint16_t address, uint8_t value);

// The writes that set a channel's registers as its owner's edit has them,
// from what the song last wrote (a shadow, see shadowWrite): output and
// connection, LFO sensitivity, the key on less the operators switched off,
// then every operator register. With no edits, as the song wrote them.
struct RegWrite
{
    uint8_t reg, value;
};
std::array<RegWrite, 27> channelRewrite(const PatchEdits* edits, const std::array<int, 8>& owner,
                                        const std::array<uint8_t, 256>& regs, const std::array<uint8_t, 8>& keys, int channel);
// The writes that set the LFO's and the noise's registers as the edits have them.
std::array<RegWrite, 5> chipRewrite(const PatchEdits* edits, const std::array<uint8_t, 256>& regs);

// Moves a channel's owner cursor past the changes due by a song time.
// Returns whether the track it plays changed.
bool advanceOwner(const PatchEdits& edits, size_t channel, double now, size_t& at, int& owner);

// Per chip channel (8 FM, then 16 PCM), in order of song time: the gain its
// output has from then on, 0 for silent. A channel before its first change
// has gain 1.
struct ChannelMix
{
    static constexpr int kChannels = 24;
    struct Change
    {
        double time;
        float gain;
    };
    std::array<std::vector<Change>, kChannels> changes;
};

// How song time moves from one version of a song to the next: a song with
// no rip whose tempo changes keeps its notes on their beats, so a time t
// becomes shift + scale x t. The default leaves every time where it is.
struct TimeMap
{
    double shift = 0.0, scale = 1.0;
    bool moves = false; // false: the identity

    // The tempo scaled by scale about a song time, as Tune::setTempo moves the notes.
    static TimeMap scaled(double origin, double scale) { return { origin * (1.0 - scale), scale, true }; }
    double operator()(double t) const { return moves ? shift + scale * t : t; }
    double inverse(double t) const { return moves ? (t - shift) / scale : t; } // a time in the new version, back in the old
    // This move, then the next.
    TimeMap then(const TimeMap& next) const
    {
        if (!moves || !next.moves)
            return moves ? *this : next;
        return { next.shift + next.scale * shift, next.scale * scale, true };
    }
};

// Plays a VGM song exactly, from any point, with a choice of channels.
// Everything here runs on the audio thread and does not allocate.
class SongPlayer
{
public:
    SongPlayer();

    void prepare(double hostRate);
    void setSong(std::shared_ptr<const VgmSong> song, std::shared_ptr<const SongSnapshots> snapshots);
    const VgmSong* song() const { return song_.get(); }

    // Which channels sound: bit n of fm is YM2151 channel n, bit n of pcm
    // SegaPCM channel n.
    void setChannels(uint8_t fm, uint16_t pcm) { fmMask_ = fm, pcmMask_ = pcm; }
    void setLevels(float fm, float pcm) { fmLevel_ = fm, pcmLevel_ = pcm; }
    // Set channels' gains over time, as a tune's mute, solo and volume do,
    // without a new song: the chips run the same, only their output is
    // scaled. A new mix glides in over 20 ms. The caller keeps it alive.
    // The mix and the edits below are for the newest version: a deck still
    // playing an older one, while a switch waits or as it fades out, keeps
    // those it has.
    void setMix(std::shared_ptr<const ChannelMix> mix) { mix_ = std::move(mix); }
    // Edited instruments and chip settings, applied as the song writes to the
    // chip and at once to the channels playing them and to the chip's LFO and
    // noise; none, the song as it is. Audio thread; the caller keeps the
    // edits alive.
    void setPatchEdits(std::shared_ptr<const PatchEdits> edits) { edits_ = std::move(edits); }

    // The chip-wide settings as the song has written them where the deck
    // heard is, whatever the edits make of them. Written as it plays, and
    // safe to call from another thread.
    ChipState gameChip() const;

    // A probe on some FM channels (bit n for channel n, 0 for none) for a
    // display: the audio thread keeps their latest output, summed, and the
    // envelopes of the loudest one's operators, which another thread reads.
    // With an owner, a channel counts only while its gain there is above 0,
    // whatever the mix. Audio thread; the caller keeps the owner alive.
    static constexpr int kProbeSize = 8192; // chip samples kept
    void setProbe(int channels, std::shared_ptr<const ChannelMix> owner = nullptr)
    {
        probe_.store(channels);
        probeOwner_ = std::move(owner);
    }
    int probe() const { return probe_.load(); }
    void probeOutput(float* out, int count) const; // the latest samples, oldest first, 1 at full scale
    void probeEnvelopes(float* levels, int* stages) const;

    // Playback speed, 1 as the song was made. The chips keep running at their
    // own rate while their writes come faster or slower, so the notes keep
    // their pitch and the drums their sound: only the tempo changes.
    void setSpeed(double speed);

    // Per chip channel (8 FM, then 16 PCM): the loudest output since the
    // last call, 1 at a chip's full scale. Safe to call from another thread.
    static constexpr int kMeterChannels = 24;
    std::array<float, kMeterChannels> takePeaks();

    // Play from a song time in VGM samples; a negative time starts with
    // silence. While playing, the change crossfades over 3 ms. Returns
    // false, playing nothing, when no snapshot reaches that time yet.
    bool play(double songTime);
    void stop();               // fades out over 3 ms
    // Repeat a stretch of the song, in VGM samples: when the deck heard
    // reaches the end, playback jumps back to the start at that output
    // sample, with the crossfade. With the end at or before the start, the
    // song plays through, as it does with a loop no longer than the
    // crossfade. A deck that is already past the end plays on. The loop is
    // in the newest version's time: while a switch waits, the deck heard
    // reaches its end in its own time, by the switch's map.
    void setLoop(double start, double end) { loopStart_ = start, loopEnd_ = end; }

    // Switch to a new version of the song while playing. A deck that has not
    // yet reached the first changed write (by index in each chip's write
    // list, against the version before) carries on with the new version
    // seamlessly; otherwise playback jumps to the same position in the new
    // version, moved by the map, with the crossfade. The new version's chips
    // start from its own snapshots, so a note the old one was sounding there
    // ends with the fade, and only the new version's notes sound after it.
    // When the snapshots do not reach the position yet, the old version plays
    // on and the switch waits for them (see updateSnapshots); it returns
    // false then. A newer version while one waits takes its place, against
    // what plays. The caller keeps old versions alive: a deck lets go of one here.
    bool replaceSong(std::shared_ptr<const VgmSong> song, std::shared_ptr<const SongSnapshots> snapshots,
                     size_t ymUnchanged, size_t pcmUnchanged, TimeMap map = {});
    // More snapshots of the same version, as they are made; a switch that
    // waits takes place once they reach the position.
    void updateSnapshots(std::shared_ptr<const SongSnapshots> snapshots);
    bool switching() const { return switching_; } // a switch waits for snapshots
    bool playing() const { return decks_[static_cast<size_t>(active_)].on; }
    double position() const;   // song time of the next output sample

    void render(float* left, float* right, int count);

private:
    struct Interface : ymfm::ymfm_interface {};

    struct Deck
    {
        std::shared_ptr<const VgmSong> song; // the version this deck plays
        Interface interface;
        MaskedYm2151 fm { interface };
        SegaPcm pcm;
        Resampler fmResampler, pcmResampler;
        StereoLowPass fmFilter, pcmFilter;
        uint64_t fmSample = 0, pcmSample = 0;
        size_t ymWrite = 0, pcmWrite = 0;
        double songTime = 0;     // song time of the next output sample
        // Away from speed 1, song time runs from an anchor: the chip sample
        // and song time where the speed last changed. At speed 1 since the
        // last seek it is each chip sample's own time, exactly.
        bool stretched = false;
        std::shared_ptr<const ChannelMix> mix;          // the mix the cursors follow
        std::array<size_t, ChannelMix::kChannels> mixAt {}; // per channel, the next change
        // A new mix glides from the gains heard to its own over a moment,
        // from these song times (FM, PCM); a change at a note's start is at once.
        std::array<float, ChannelMix::kChannels> lastGain {}, fromGain {};
        double fadeFrom[2] = { -1e18, -1e18 };
        std::shared_ptr<const PatchEdits> edits;      // the edits the deck has applied
        std::array<size_t, 8> editAt {};              // per FM channel, its next change of owner
        std::array<int, 8> editOwner {};              // and the track it plays now, -1 none
        std::array<uint8_t, 256> regs {};             // every register as the song last wrote it, the vibrato depth at 0x1A
        std::array<uint8_t, 8> keys {};               // per channel, its operators as the song last keyed them
        std::shared_ptr<const ChannelMix> probeOwner; // the owner the probe's cursors follow
        std::array<size_t, 8> probeAt {};             // per FM channel, its next change
        uint64_t fmAnchor = 0, pcmAnchor = 0;
        double fmAnchorTime = 0, pcmAnchorTime = 0;
        int64_t silence = 0;     // output samples before the song starts
        bool on = false;
        float gain = 0, target = 0;
    };

    bool seek(Deck& deck, double songTime);
    void seekMix(Deck& deck, bool glide); // the mix's cursors to where the deck is; glide from the gains heard
    void seekEdits(Deck& deck);           // the edits' cursors to where the deck is, and every channel rewritten
    void rewrite(Deck& deck, int channel);  // a channel's registers, as its owner's edit has them
    void rewriteChip(Deck& deck);           // the LFO's and the noise's registers, as the edits have them
    double fmNow(const Deck& deck) const;  // song time of the deck's next FM chip sample
    double pcmNow(const Deck& deck) const; // and of its next PCM one
    void renderDeck(Deck& deck, float* left, float* right, int count);

    std::shared_ptr<const VgmSong> song_;
    std::shared_ptr<const SongSnapshots> snapshots_;
    // A switch that waits for snapshots: what the deck heard has in common
    // with song_, and how its time moves into it.
    bool switching_ = false;
    size_t switchYm_ = 0, switchPcm_ = 0;
    TimeMap switchMap_;
    std::array<Deck, 2> decks_;
    int active_ = 0;
    double hostRate_ = 48000.0;
    float fade_ = 1.0f;          // gain change per output sample
    double speed_ = 1.0;
    double loopStart_ = 0, loopEnd_ = 0; // song times; no loop while the end is not after the start
    std::shared_ptr<const ChannelMix> mix_;
    std::shared_ptr<const ChannelMix> probeOwner_;
    std::shared_ptr<const PatchEdits> edits_;
    std::atomic<int> probe_ { 0 };
    std::array<std::atomic<float>, kProbeSize> probeOutput_ {};
    std::atomic<uint32_t> probeAt_ { 0 }; // next slot to write
    std::array<std::atomic<float>, 4> probeLevel_ {};
    std::array<std::atomic<int>, 4> probeStage_ {};
    uint8_t fmMask_ = 0xFF;
    uint16_t pcmMask_ = 0xFFFF;
    float fmLevel_ = 0.7f, pcmLevel_ = 0.7f;
    std::vector<uint8_t> state_; // scratch for restoring a snapshot
    std::array<std::atomic<float>, kMeterChannels> peaks_ {};
    std::atomic<uint32_t> gameChip_ { 0 }; // the song's chip-wide settings, packed (see gameChip)
};

} // namespace opm86
