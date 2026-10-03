#pragma once

#include "FmPatch.h"

#include <ymfm_opm.h>

#include <array>
#include <cstdint>

namespace opm86
{

// The YM2151 half of the sound board: one emulated chip plus the logic a
// sound driver would run on the Z80, turning notes into register writes.
//
// It is multitimbral: each of the 16 MIDI channels is a part with its own
// patch, and the chip's 8 channels are shared between all parts. The LFO
// is global on the chip, so it follows the patch of part 0.
class FmSection
{
public:
    // Out Run clocks the YM2151 from a 16 MHz crystal divided by 4.
    static constexpr double kClock = 4000000.0;
    static constexpr double kSampleRate = kClock / 64.0; // 62500 Hz
    static constexpr int kVoices = 8;
    static constexpr int kParts = 16;

    FmSection();

    void reset();

    void setPatch(const FmPatch& patch); // every part
    void setPartPatch(int part, const FmPatch& patch);
    const FmPatch& partPatch(int part) const { return parts_[static_cast<size_t>(part)].patch; }
    void setVelocitySensitivity(float amount); // 0..1, up to 48 dB at velocity 1
    void setBendRange(int semitones);                // every part without its own range
    void setPartBendRange(int part, int semitones);  // as set by MIDI RPN 0
    void setPartAttenuation(int part, int steps);    // carriers quieter by 0.75 dB steps (CC 11)
    void setPartPan(int part, FmPan pan);            // replaces the patch's output (CC 10)
    int partBendRange(int part) const
    {
        const int own = parts_[static_cast<size_t>(part)].bendRange;
        return own >= 0 ? own : bendRange_;
    }

    void noteOn(int part, int note, int velocity);
    void noteOff(int part, int note);
    void setSustain(int part, bool down);
    void setPitchBend(int part, float amount); // -1..1
    void setModWheel(int part, float amount);  // 0..1, adds vibrato depth
    void allNotesOff();

    // Advance the chip by one sample at kSampleRate.
    void generate(int32_t& left, int32_t& right);

private:
    struct Interface : ymfm::ymfm_interface {};

    struct Part
    {
        FmPatch patch;
        float bend = 0.0f;
        int bendRange = -1; // semitones; -1 uses the section's range
        int attenuation = 0;
        int pan = -1;       // an FmPan, or -1 to use the patch's
        float modWheel = 0.0f;
        bool sustain = false;
    };

    struct Voice
    {
        int part = 0;
        int note = -1;
        int velocity = 0;
        bool gate = false;
        bool sustained = false;
        bool pendingKeyOn = false;
        bool keyed = false;                 // key down on the chip
        uint64_t offSample = UINT64_MAX;    // chip sample of the last key off
        uint64_t age = 0;
    };

    void write(uint8_t reg, uint8_t value);
    void writeAlways(uint8_t reg, uint8_t value);
    void applyGlobal();
    void applyChannel(int ch);
    void applyPitch(int ch);
    void release(int ch);
    void keyOff(int ch);
    void keyOn(int ch);
    int pickVoice(int part, int note) const;

    Interface interface_;
    ymfm::ym2151 chip_;
    std::array<int16_t, 256> shadow_ {};
    int lastPmd_ = -1;
    int lastAmd_ = -1;

    std::array<Part, kParts> parts_ {};
    std::array<Voice, kVoices> voices_ {};
    uint64_t clock_ = 0;
    uint64_t samples_ = 0; // chip samples generated
    float velocitySensitivity_ = 0.5f;
    int bendRange_ = 2;
};

} // namespace opm86
