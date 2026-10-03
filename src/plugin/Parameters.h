#pragma once

#include "Engine.h"
#include "FmPatch.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace opm86
{

// Field names of operator parameters; the IDs are e.g. "m1_tl", "c2_ar".
inline constexpr const char* kOperatorFields[] = { "tl", "ar", "d1r", "d2r", "rr", "d1l", "mul", "dt1", "dt2", "ks", "am" };
inline constexpr int kOperatorFieldCount = 11;

juce::String operatorParamId(int op, const char* field);
juce::String padParamId(int pad, const char* field);

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// Raw parameter values, looked up once so the audio thread reads atomics only.
class ParameterRefs
{
public:
    explicit ParameterRefs(juce::AudioProcessorValueTreeState& state);

    FmPatch patch() const;
    PadParams pad(int index) const;
    float fmLevel() const { return fmLevel_->load(); }
    float pcmLevel() const { return pcmLevel_->load(); }
    float velocitySensitivity() const { return velSens_->load(); }
    int bendRange() const { return static_cast<int>(bendRange_->load()); }
    int drumChannel() const { return static_cast<int>(drumChannel_->load()); }

private:
    using Value = std::atomic<float>*;

    Value alg_, fb_, lfoFreq_, lfoWave_, pmd_, amd_, pms_, ams_, pan_;
    std::array<std::array<Value, kOperatorFieldCount>, 4> ops_ {};
    std::array<std::array<Value, 4>, kPads> pads_ {};
    Value fmLevel_, pcmLevel_, velSens_, bendRange_, drumChannel_;
};

// Set every patch parameter, notifying the host. Call on the message thread.
void applyPatchToParameters(juce::AudioProcessorValueTreeState& state, const FmPatch& patch);

} // namespace opm86
