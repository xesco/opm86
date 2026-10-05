#pragma once

#include "AppModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

// The settings every voice shares, as the chip has them, to edit while the
// song plays: the LFO's rate, wave and two depths, and the noise generator,
// which plays on voice V8's C2 operator instead of its sine. A setting left
// to the game shows the game's value at the playhead, dimmed, and follows
// playback; one edited here takes the place of the game's writes all
// through the song, in yellow. A dial turns by dragging up or down, a word or
// a switch changes by clicking, and a double-click or an Option-click puts
// back the game's; each section's REVERT puts back all of its settings.
// Each one's tooltip names it, with its datasheet name, register and raw
// value.
class ChipView : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    explicit ChipView(AppModel& model);
    juce::String getTooltip() override; // the setting under the pointer

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    void timerCallback() override { repaint(); }

    // The chip's settings, and how each is drawn.
    enum class Field { Rate, Wave, Pmd, Amd, Noise, NoiseFreq };
    enum class Look { Dial, Word, Toggle };
    struct Control
    {
        Field field;
        Look look;
        juce::Rectangle<int> bounds;
        const char* name;
    };
    static int get(const ChipState&, Field);
    static std::optional<int> edited(const ChipEdits&, Field); // the value set, or none for the game's
    static void set(ChipEdits&, Field, std::optional<int> value);
    static juce::Range<int> range(Field);
    static juce::String text(Field, int value);
    juce::String describe(Field) const; // its name in plain words, datasheet name, register and raw value, and whose it is
    void commit(Field, std::optional<int> value, bool newStep); // one "Edit chip" step, or part of the one before
    void paintControl(juce::Graphics&, const Control&);
    int controlAt(juce::Point<int>) const;

    AppModel& model_;
    juce::Rectangle<int> lfoPanel_, noisePanel_;
    std::vector<Control> controls_; // laid out in resized
    juce::TextButton lfoRevert_ { "REVERT" }, noiseRevert_ { "REVERT" };
    int dragging_ = -1;     // the control being turned
    int dragFrom_ = 0;      // its value when the turn began
    float dragY_ = 0;       // and the pointer's height
    bool dragStep_ = false; // the turn has not made its undo step yet
    juce::TooltipWindow tooltips_ { this, 400 }; // the main window's shows only its own components' tips
};

// The floating window that holds a ChipView. The app's command keys work in
// it too, as in the patch window.
class ChipWindow : public juce::DocumentWindow
{
public:
    ChipWindow(AppModel& model, juce::LookAndFeel* look, juce::ApplicationCommandManager& commands);
    ~ChipWindow() override;
    void closeButtonPressed() override;
    bool keyPressed(const juce::KeyPress&) override;
    std::function<void()> onClose;

private:
    juce::ApplicationCommandManager& commands_;
};

} // namespace opm86
