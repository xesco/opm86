#pragma once

#include "Resampler.h"
#include "SegaPcm.h"
#include "VgmSong.h"

#include <ymfm_opm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
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

    // An operator's envelope as it runs: its stage (ymfm's envelope_state)
    // and its attenuation, 0 loudest to 1023 silent. slot is the register
    // slot, channel + 8 x (M1, M2, C1, C2).
    void envelope(uint32_t slot, uint32_t& stage, uint32_t& attenuation) const
    {
        const auto* op = m_fm.debug_operator(slot);
        stage = op->debug_eg_state();
        attenuation = op->debug_eg_attenuation();
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

// The chips as they play, for a view of them: each FM operator's envelope
// and each PCM channel's place in the ROM.
struct ChipState
{
    enum class Stage : uint8_t { Off, Attack, Decay, Sustain, Release };
    std::array<Stage, 32> stage {};         // per FM operator, by register slot: channel + 8 x (M1, M2, C1, C2)
    std::array<float, 32> level {};         // and its envelope's level, 1 loudest, 0 silent
    std::array<uint32_t, 16> pcmAddress {}; // per PCM channel: the ROM address it plays from now
    uint16_t pcmPlaying = 0;                // bit n: PCM channel n is playing
    std::array<float, ChannelMix::kChannels> gain {}; // per chip channel, 8 FM then 16 PCM: the gain the mix gives it now, 0 when silenced
    bool valid = false;                     // false until the chips have played
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
    void setMix(std::shared_ptr<const ChannelMix> mix) { mix_ = std::move(mix); }

    // Playback speed, 1 as the song was made. The chips keep running at their
    // own rate while their writes come faster or slower, so the notes keep
    // their pitch and the drums their sound: only the tempo changes.
    void setSpeed(double speed);

    // Per chip channel (8 FM, then 16 PCM): the loudest output since the
    // last call, 1 at a chip's full scale. Safe to call from another thread.
    static constexpr int kMeterChannels = 24;
    std::array<float, kMeterChannels> takePeaks();
    // The playing chips as of the last block rendered. Safe to call from
    // another thread.
    ChipState chipState() const;
    // Each chip's output, FM left and right then PCM left and right: the
    // loudest since the last call, 1 at full scale. Safe to call from
    // another thread.
    std::array<float, 4> takeChipPeaks();

    // Play from a song time in VGM samples; a negative time starts with
    // silence. While playing, the change crossfades over 3 ms. Returns
    // false, playing nothing, when no snapshot reaches that time yet.
    bool play(double songTime);
    void stop();               // fades out over 3 ms

    // Switch to a new version of the song while playing. A deck that has not
    // yet reached the first changed write (by index in each chip's write
    // list) carries on with the new version seamlessly; otherwise playback
    // jumps to the same position in the new version with the crossfade.
    // Returns false when the snapshots do not reach the position yet.
    // The caller keeps old versions alive: a deck lets go of one here.
    bool replaceSong(std::shared_ptr<const VgmSong> song, std::shared_ptr<const SongSnapshots> snapshots,
                     size_t ymUnchanged, size_t pcmUnchanged);
    // More snapshots of the same version, as they are made.
    void updateSnapshots(std::shared_ptr<const SongSnapshots> snapshots) { snapshots_ = std::move(snapshots); }
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
        uint64_t fmAnchor = 0, pcmAnchor = 0;
        double fmAnchorTime = 0, pcmAnchorTime = 0;
        int64_t silence = 0;     // output samples before the song starts
        bool on = false;
        float gain = 0, target = 0;
    };

    bool seek(Deck& deck, double songTime);
    void seekMix(Deck& deck, bool glide); // the mix's cursors to where the deck is; glide from the gains heard
    double fmNow(const Deck& deck) const;  // song time of the deck's next FM chip sample
    double pcmNow(const Deck& deck) const; // and of its next PCM one
    void renderDeck(Deck& deck, float* left, float* right, int count);

    std::shared_ptr<const VgmSong> song_;
    std::shared_ptr<const SongSnapshots> snapshots_;
    std::array<Deck, 2> decks_;
    int active_ = 0;
    double hostRate_ = 48000.0;
    float fade_ = 1.0f;          // gain change per output sample
    double speed_ = 1.0;
    std::shared_ptr<const ChannelMix> mix_;
    uint8_t fmMask_ = 0xFF;
    uint16_t pcmMask_ = 0xFFFF;
    float fmLevel_ = 0.7f, pcmLevel_ = 0.7f;
    std::vector<uint8_t> state_; // scratch for restoring a snapshot
    std::array<std::atomic<float>, kMeterChannels> peaks_ {};
    std::array<std::atomic<float>, 4> chipPeaks_ {}; // FM left, right, PCM left, right
    void publishChips(const Deck& deck); // after each block, for chipState
    ChipState chips_;                    // written by the audio thread under chipsSequence_
    mutable std::atomic<uint32_t> chipsSequence_ { 0 }; // odd while chips_ is being written
};

} // namespace opm86
