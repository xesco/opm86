#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

class OpmLookAndFeel : public juce::LookAndFeel_V4
{
public:
    OpmLookAndFeel();
};

// A rotary control bound to one parameter, with its name above.
class Knob : public juce::Component
{
public:
    // A compact knob has no value box; it shows the value while dragged.
    Knob(juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& label,
         bool compact = false);
    void resized() override;

private:
    juce::Slider slider_;
    juce::Label label_;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment_;
};

class ChoiceBox : public juce::Component
{
public:
    ChoiceBox(juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& label);
    void resized() override;

private:
    juce::ComboBox box_;
    juce::Label label_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment_;
};

class OperatorPanel : public juce::Component
{
public:
    OperatorPanel(juce::AudioProcessorValueTreeState& state, int op);
    void setCarrier(bool carrier);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    int op_;
    bool carrier_ = false;
    juce::OwnedArray<Knob> knobs_;
    juce::ToggleButton am_ { "LFO AM" };
    juce::AudioProcessorValueTreeState::ButtonAttachment amAttachment_;
};

class PadView : public juce::Component
{
public:
    PadView(OpmProcessor& processor, int pad);
    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void showMenu();

    OpmProcessor& processor_;
    int pad_;
    juce::String name_;
    juce::TextButton menu_ { "Sample" };
    Knob tune_, level_, pan_, choke_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

// Song mode: which imported song plays, which of its channels, and how
// far loading has got.
class SongPanel : public juce::Component
{
public:
    explicit SongPanel(OpmProcessor& processor);
    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    OpmProcessor& processor_;
    juce::ComboBox song_, stem_;
    juce::Label songLabel_, stemLabel_, status_, help_;
};

class OpmEditor : public juce::AudioProcessorEditor, private juce::ChangeListener, private juce::Timer
{
public:
    explicit OpmEditor(OpmProcessor&);
    ~OpmEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshPresets();
    void showImportMenu();
    void importVgm();

    OpmProcessor& processor_;
    OpmLookAndFeel lookAndFeel_;

    juce::ComboBox mode_;
    juce::ComboBox presets_;
    juce::TextButton import_ { "Import VGM" };
    juce::TextButton kit_ { "Default Kit" };
    juce::Label status_;
    Knob fmLevel_, pcmLevel_, velSens_, bendRange_, drumChannel_;

    Knob alg_, fb_, lfoFreq_, pmd_, amd_, pms_, ams_;
    ChoiceBox lfoWave_, pan_;
    juce::Label algDiagram_;

    juce::OwnedArray<OperatorPanel> operators_;
    juce::OwnedArray<PadView> pads_;
    juce::MidiKeyboardComponent keyboard_;
    SongPanel songPanel_;
    void showMode();
    std::unique_ptr<juce::FileChooser> chooser_;

    int shownAlg_ = -1;
    juce::Rectangle<int> voiceArea_, drumArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpmEditor)
};

} // namespace opm86
