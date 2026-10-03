#pragma once

#include "FmSection.h"
#include "PcmBank.h"
#include "Resampler.h"
#include "SegaPcm.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace opm86
{

struct PadParams
{
    int tune = 0;   // semitones, -24..+24
    int level = 100; // 0-127
    int pan = 0;    // -63 (left) .. +63 (right)
    int choke = 0;  // pads sharing a non-zero group cut each other off
};

// The whole sound board: YM2151 and SegaPCM driven from MIDI and mixed
// at the host sample rate. Everything here runs on the audio thread.
class Engine
{
public:
    Engine();

    void prepare(double hostRate);
    void reset();

    FmSection& fm() { return fm_; }

    void setBank(std::shared_ptr<const PcmBank> bank);
    const std::shared_ptr<const PcmBank>& bank() const { return bank_; }
    void setPadParams(int pad, const PadParams& params) { padParams_[static_cast<size_t>(pad)] = params; }
    // MIDI channel for the drum pads: 1-16, kDrumsOnAllChannels, or 0 for none.
    static constexpr int kDrumsOnAllChannels = 17;

    // On the drum channel, this controller at 64 or more makes a note off end
    // its hit, as a game's driver cuts samples; below 64, one-shot samples
    // play to their end, as on a drum machine. 85 is undefined in MIDI.
    static constexpr int kDrumGateController = 85;
    void setDrumChannel(int channel) { drumChannel_ = channel; }
    void setLevels(float fm, float pcm) { fmLevel_ = fm; pcmLevel_ = pcm; }

    // Patches that MIDI program changes select from; empty ignores them.
    void setPatchLibrary(std::vector<FmPatch> patches) { library_ = std::move(patches); }

    // Apply a MIDI message before the next chip sample.
    void handleMidi(const uint8_t* data, int size);

    // Queue a MIDI message for a time in host samples since prepare(),
    // fractions included. It takes effect at the first chip sample at or
    // after that time, as a register write in a VGM file does, whatever the
    // block size. Messages must be queued in time order.
    void scheduleMidi(const uint8_t* data, int size, double hostTime);
    double hostTime() const { return static_cast<double>(hostSamples_); }

    void render(float* left, float* right, int count);

private:
    void triggerPad(int pad, int velocity);
    void stopPad(int pad, bool all);        // the pad's oldest playing hit, or all its hits
    void setPadVolume(int pad, int volume); // of the pad's latest hit; 0-127, scaled by the pad's level
    void setChannelVolume(int ch, int volume);
    int latestChannel(int pad) const;       // chip channel of the pad's latest playing hit, or -1
    void setDrumBend(float semitones);

    FmSection fm_;
    SegaPcm pcm_;
    std::shared_ptr<const PcmBank> bank_;
    std::array<PadParams, kPads> padParams_ {};
    std::vector<FmPatch> library_;
    std::array<std::pair<int, int>, 16> rpn_ {}; // selected RPN per MIDI channel; 127 = none
    std::array<int, SegaPcm::kChannels> chipPad_ {};       // pad each chip channel plays
    std::array<uint64_t, SegaPcm::kChannels> chipAge_ {};  // hit count when it started
    uint64_t hitCount_ = 0;
    int drumPan_ = -1;       // last CC 10 on the drum channel, 0-127; -1 until one arrives
    float drumBend_ = 0.0f;  // semitones
    int lastPad_ = -1;       // pad of the last note on or aftertouch on the drum channel
    bool drumGate_ = false;  // note offs end one-shot samples too

    struct Pending
    {
        uint64_t chipSample;
        uint8_t bytes[3];
        int size;
    };

    // A fixed ring of queued messages for one chip, safe on the audio thread.
    class Queue
    {
    public:
        bool push(const Pending& p)
        {
            if (count_ == items_.size())
                return false;
            items_[(head_ + count_++) % items_.size()] = p;
            return true;
        }
        const Pending* front() const { return count_ ? &items_[head_] : nullptr; }
        void pop() { head_ = (head_ + 1) % items_.size(), --count_; }
        void clear() { head_ = count_ = 0; }

    private:
        std::array<Pending, 4096> items_ {};
        size_t head_ = 0, count_ = 0;
    };

    void applyDue(Queue& queue, uint64_t chipSample);

    Queue fmQueue_, pcmQueue_;
    uint64_t fmSamples_ = 0, pcmSamples_ = 0, hostSamples_ = 0;
    double hostRate_ = 48000.0;

    Resampler fmResampler_, pcmResampler_;
    StereoLowPass fmFilter_, pcmFilter_;

    int drumChannel_ = 10;
    float fmLevel_ = 0.7f;
    float pcmLevel_ = 0.7f;
};

} // namespace opm86
