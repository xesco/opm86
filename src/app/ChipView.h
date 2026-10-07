#pragma once

#include "AppModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace opm86
{

// The two chips at the playhead, each in its own section: the YM2151, its
// registers decoded by channel and operator, and the SegaPCM, its registers
// by channel. The values are the game's writes up to the playhead, before
// the player's mutes and volumes, and each one glows for a moment when it
// is written.
class ChipView : public juce::Component
{
public:
    explicit ChipView(AppModel& model);
    void paint(juce::Graphics&) override;

private:
    void catchUp(); // the registers as written up to the playhead
    void write(bool fm, uint16_t reg, uint8_t value, double time);
    float glow(bool fm, int reg) const; // 1 as it is written, down to 0 after kGlow
    void paintFm(juce::Graphics&, juce::Rectangle<float> area);
    void paintPcm(juce::Graphics&, juce::Rectangle<float> area);

    AppModel& model_;
    const VgmSong* song_ = nullptr; // the song the registers belong to
    size_t ymNext_ = 0, pcmNext_ = 0; // the next writes to apply
    double at_ = 0.0;                 // the song time they are applied up to
    std::array<uint8_t, 256> ym_ {}, pcm_ {};
    std::array<double, 256> ymWritten_ {}, pcmWritten_ {}; // song time of each register's last write
    std::array<uint8_t, 8> keyOn_ {}; // per FM channel, its operators keyed on: bit 0 M1, 1 C1, 2 M2, 3 C2
    std::array<double, 8> keyWritten_ {};
    uint8_t pmd_ = 0, amd_ = 0;       // the LFO's depths, which share register 0x19
};

} // namespace opm86
