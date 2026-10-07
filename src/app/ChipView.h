#pragma once

#include "AppModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <tuple>
#include <vector>

namespace opm86
{

// The two chips at the playhead, each in its own section: the YM2151, its
// registers decoded by channel and operator, and the SegaPCM, its registers
// by channel. The values are the game's writes up to the playhead, before
// the player's mutes and volumes, and each one glows for a moment when it
// is written.
class ChipView : public juce::Component, public juce::TooltipClient
{
public:
    explicit ChipView(AppModel& model);
    void paint(juce::Graphics&) override;
    juce::String getTooltip() override; // what the abbreviation under the pointer means

private:
    void tip(juce::Rectangle<float> area, const juce::String& label); // a label drawn there, explained on hover
    std::vector<std::pair<juce::Rectangle<float>, juce::String>> tips_; // the labels drawn, refilled each paint
    void catchUp(); // the registers as written up to the playhead
    void write(bool fm, uint16_t reg, uint8_t value, double time);
    float glow(bool fm, int reg) const; // 1 as a new value is written, a third for the same value again, down to 0 after kGlow
    void paintFm(juce::Graphics&, juce::Rectangle<float> area);
    void paintOperatorTable(juce::Graphics&, juce::Rectangle<float> area, int ch, juce::Colour accent); // the values, a row per operator
    void paintOperatorGraph(juce::Graphics&, juce::Rectangle<float> area, int ch, juce::Colour accent); // the algorithm and the envelopes, drawn
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void paintPcm(juce::Graphics&, juce::Rectangle<float> area);
    void paintSampleMap(juce::Graphics&, juce::Rectangle<float> area, juce::Colour accent); // the song's samples, by bank, lit as they play
    void paintPcmTable(juce::Graphics&, juce::Rectangle<float> area, juce::Colour accent, float valueFont); // a row per channel

    AppModel& model_;
    const VgmSong* song_ = nullptr; // the song the registers belong to
    size_t ymNext_ = 0, pcmNext_ = 0; // the next writes to apply
    double at_ = 0.0;                 // the song time they are applied up to
    std::array<uint8_t, 256> ym_ {}, pcm_ {};
    std::array<double, 256> ymWritten_ {}, pcmWritten_ {}; // song time of each register's last write
    std::array<bool, 256> ymChanged_ {}, pcmChanged_ {};   // and whether that write changed its value
    std::array<uint8_t, 8> keyOn_ {}; // per FM channel, its operators keyed on: bit 0 M1, 1 C1, 2 M2, 3 C2
    std::array<double, 8> keyWritten_ {};
    uint8_t pmd_ = 0, amd_ = 0;       // the LFO's depths, which share register 0x19
    ChipState live_;                  // the chips as they play, read each paint
    float fmFont_ = 13.0f;            // the YM2151 values' size, which the SegaPCM's follow
    // A sample the song plays: its bank, and its start and end in the bank.
    struct Sample
    {
        uint32_t bank = 0, start = 0, end = 0;
        bool operator<(const Sample& o) const { return std::tie(bank, start, end) < std::tie(o.bank, o.start, o.end); }
        bool operator==(const Sample& o) const { return bank == o.bank && start == o.start && end == o.end; }
    };
    std::vector<Sample> samples_; // every one the song triggers, by bank and start
    float liveLevel(int slot) const;  // an FM operator's envelope level now, 0 when the chips have not played
    std::array<bool, 8> drawn_ {};    // per FM channel: its operators drawn, not listed
    std::array<juce::Rectangle<float>, 8> drawToggle_ {}; // the switch between the two in each channel's header
};

} // namespace opm86
