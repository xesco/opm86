#pragma once

#include "Resampler.h"
#include "SegaPcm.h"
#include "VgmSong.h"

#include <ymfm_opm.h>

#include <algorithm>
#include <array>
#include <atomic>
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

    // Per chip channel (8 FM, then 16 PCM): the loudest output since the
    // last call, 1 at a chip's full scale. Safe to call from another thread.
    static constexpr int kMeterChannels = 24;
    std::array<float, kMeterChannels> takePeaks();

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
        int64_t silence = 0;     // output samples before the song starts
        bool on = false;
        float gain = 0, target = 0;
    };

    bool seek(Deck& deck, double songTime);
    void renderDeck(Deck& deck, float* left, float* right, int count);

    std::shared_ptr<const VgmSong> song_;
    std::shared_ptr<const SongSnapshots> snapshots_;
    std::array<Deck, 2> decks_;
    int active_ = 0;
    double hostRate_ = 48000.0;
    float fade_ = 1.0f;          // gain change per output sample
    uint8_t fmMask_ = 0xFF;
    uint16_t pcmMask_ = 0xFFFF;
    float fmLevel_ = 0.7f, pcmLevel_ = 0.7f;
    std::vector<uint8_t> state_; // scratch for restoring a snapshot
    std::array<std::atomic<float>, kMeterChannels> peaks_ {};
};

} // namespace opm86
