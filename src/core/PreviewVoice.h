#pragma once

#include "FmPatch.h"
#include "Resampler.h"
#include "SongPlayer.h"
#include "Tune.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace opm86
{

// A YM2151 of its own to hear an instrument with no song playing: the
// tracker's test keyboard and its note previews. A note sounds with the
// registers a written note has in the build (see writtenRegs), the operators
// switched off left out of its key on, and the patch's LFO set on the chip,
// so it sounds as the note would in the song. Up to kChannels keys sound at
// once, each on a chip channel of its own; a key let go plays its release.
// A SegaPCM of its own plays a drum track's sample the same way, on one
// channel, as a written hit plays it in the build.
// Everything here runs on the audio thread and does not allocate, except
// where marked.
class PreviewVoice
{
public:
    static constexpr int kChannels = 4;

    PreviewVoice();

    void prepare(double hostRate);
    // The chip as the song has it: its sample rate, and the semitones its clock
    // adds to every pitch (see Tune::clockShift). Resets the chip.
    void setChip(double chipRate, double clockShift);
    // The instrument, for the keys to come and those sounding now: a held key
    // changes its sound at once. Only the patch's numbers are read.
    void setPatch(const FmPatch& patch);
    // The operators muted to audition the others (bit i for op[i]): left out
    // of the key ons, as those switched off are, without changing the patch.
    // A held key drops or takes them back at once.
    void setMuted(uint8_t operators);
    // A MIDI key down; with seconds above 0, it lets go by itself after
    // that long. A key already down starts again. A note's expression can
    // go with it, as the build writes it: fine tune in 64ths of a semitone,
    // its carriers' total level that many 0.75 dB steps lower, and its
    // output sides (0x40 left, 0x80 right, 0xC0 both).
    void noteOn(int key, double seconds = 0.0, int fine = 0, int quieter = 0, uint8_t sides = 0xC0);
    void noteOff(int key);
    void releaseAll(); // every key let go, each playing its release
    // Every key let go at once, with the fastest release, for the song to
    // take over: the patch set from now on changes no channel sounding.
    void stop();

    // The song's SegaPCM, to audition a drum track's sample: a chip of its
    // own fed the song's ROM, with its banking and rate. Resets the chip; the
    // song is kept, and let go only by the next one.
    void setPcm(std::shared_ptr<const VgmSong> song);
    // A sample started on one channel as a written hit starts it in the
    // build (see Tune::sampleVoice), so it sounds as the hit would in the
    // song; a sample sounding already starts again. With seconds above 0 it
    // stops after that long; else a one-shot plays out and a loop plays
    // until pcmOff.
    void pcmOn(const PcmVoice& voice, double seconds = 0.0);
    void pcmOff();
    bool pcmSounding() const { return pcmSounding_.load(std::memory_order_relaxed); }

    // Adds the chips' output into left and right, at the host rate.
    void render(float* left, float* right, int count);

    // For a display on another thread: whether a key is down or a release
    // still sounds, the lowest key down (or the last one, while it
    // releases; -1 when silent), the latest output and the envelopes of the
    // loudest channel's operators, as SongPlayer's probe has them.
    static constexpr int kProbeSize = SongPlayer::kProbeSize;
    bool sounding() const { return sounding_.load(std::memory_order_relaxed); }
    int pitch() const { return pitch_.load(std::memory_order_relaxed); }
    void probeOutput(float* out, int count) const;
    void probeEnvelopes(float* levels, int* stages) const;

private:
    struct Interface : ymfm::ymfm_interface {};

    struct Channel
    {
        int key = -1;                  // the key it plays, or played last
        int fine = 0;                  // 64ths of a semitone on top of the key
        int quieter = 0;               // 0.75 dB steps off the carriers' total level
        uint8_t sides = 0xC0;          // output sides, register 0x20 bits 6 and 7
        bool held = false;             // keyed on on the chip
        bool pending = false;          // to key on at the next chip sample, after a sample keyed off
        uint64_t since = 0;            // when it was keyed on or let go, to pick the next channel
        uint64_t releaseAt = UINT64_MAX; // chip sample to let go at, for a note of a set length
    };

    void write(int reg, int value);
    void writeChannel(int ch);   // the patch's registers at the channel's key, with its expression
    void writeLfo();             // the patch's LFO, where it differs from the chip's
    void release(int ch);
    void step(float& left, float& right); // one chip sample
    void updateProbe();          // the envelopes, whether anything sounds, and the pitch

    Interface interface_;
    MaskedYm2151 chip_ { interface_ };
    Resampler resampler_;
    StereoLowPass filter_;
    double hostRate_ = 48000.0;
    double chipRate_ = 3579545.0 / 64.0;
    double clockShift_ = 0.0;

    const FmPatch blank_ {};               // for writtenRegs to give a key's code and fraction, which no patch changes
    std::array<uint8_t, kFmRegs> regs_ {}; // writtenRegs at middle C; each channel sets its own key
    uint8_t keys_ = 0x78;                  // the operators a key on starts
    uint8_t patchKeys_ = 0x78;             // the patch's, before the audition mutes
    uint8_t muted_ = 0;                    // the operators muted to audition, bit i for op[i]
    std::array<uint8_t, 4> lfo_ {};        // LFO rate, PMD, AMD and wave, as the patch has them
    std::array<int, 4> lfoOnChip_ {};      // and as written to the chip, -1 not yet
    std::array<Channel, kChannels> channels_ {};
    int lastKey_ = -1;     // the key last started
    uint64_t clock_ = 0;   // order of key ons and releases
    uint64_t sample_ = 0;  // chip samples generated
    bool quiet_ = true;    // nothing keyed and every envelope at silence: the chip need not run

    // The SegaPCM: its song, the chip, and the chip sample a timed sample stops at.
    std::shared_ptr<const VgmSong> pcmSong_;
    SegaPcm pcm_;
    Resampler pcmResampler_;
    StereoLowPass pcmFilter_;
    uint64_t pcmSample_ = 0;
    uint64_t pcmStopAt_ = UINT64_MAX;
    bool pcmQuiet_ = true;
    std::atomic<bool> pcmSounding_ { false };
    void renderPcm(float* left, float* right, int count);

    std::atomic<bool> sounding_ { false };
    std::atomic<int> pitch_ { -1 };
    std::array<std::atomic<float>, kProbeSize> probeOutput_ {};
    std::atomic<uint32_t> probeAt_ { 0 };
    std::array<std::atomic<float>, 4> probeLevel_ {};
    std::array<std::atomic<int>, 4> probeStage_ {};
};

} // namespace opm86
