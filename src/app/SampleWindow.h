#pragma once

#include "AppModel.h"
#include "PatchWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

// Loads a drum sample from an audio file: asks for a WAV or AIFF file, then
// how to encode it for the SegaPCM (its rate, and whether its silence is
// trimmed and it is normalised), reads it and hands the sample to done
// (see encodeSample). A file it cannot read, or one with no sound, it says
// so of. One at a time; the dialogs go with it.
class AudioSampleChooser
{
public:
    void choose(juce::Component* parent, std::function<void(const PadSample&)> done);

private:
    void askEncoding(juce::Component* parent, const juce::File& file);

    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::AlertWindow> options_;
    std::function<void(const PadSample&)> done_;
};

// A drum track's sample, in the patch window's look: its waveform as the
// chip reads it from the song's ROM, with
// markers for where it starts, where it ends (a whole page, as the chip
// ends one) and where a loop starts again; the loop switched on or off; its
// pitch at no transposition, as the chip's step, in Hz and as a note; its
// left and right volumes; and two octaves of keys to play it with, middle C
// at its pitch. A marker drags, a dial turns by dragging up or down, a
// switch changes by clicking, and a double-click or an Option-click puts
// back the game's value; REVERT puts back all of them. REPLACE... loads
// the sample's sound from an audio file, for every track that plays it,
// in the ROM's free space. Every hit of the track plays with the edit, so
// it needs a new build, which playback switches to at the playhead. A
// setting left to the game shows the sample's usual value, dimmed; one
// edited here is yellow. Each one's tooltip names it, with its register
// and raw value.
class SampleView : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    SampleView(AppModel& model, int track);
    ~SampleView() override;
    juce::String getTooltip() override; // the setting under the pointer

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    // The sample's settings, each a marker on the waveform or a control.
    enum class Field { Start, End, LoopStart, Loop, Step, Left, Right };
    struct Control
    {
        Field field;
        juce::Rectangle<int> bounds;
        const char* name;
        bool asNote = false; // the pitch shown as a note, not in Hz
    };
    const PadSample* sample() const;      // the track's sample; nullptr once the track is gone
    SampleEdit edit() const;              // the track's edit, inside the sample's range
    static bool edited(const SampleEdit&, Field);
    int value(Field) const;               // as heard: edited, else the sample's own
    static void set(SampleEdit&, Field, std::optional<int> value);
    juce::Range<int> range(Field) const;
    juce::String describe(Field) const;   // its name, register and raw value, and whose it is
    void commit(Field, std::optional<int> value, bool newStep); // one "Edit sample" step, or part of the one before
    void replace(const PadSample& sound); // one "Replace sample" step

    // The waveform: the sample's whole range in the ROM across the box.
    int first() const;                    // the first address of the sample's range
    int end() const;                      // and the first address past it
    float addressToX(int address) const;
    int xToAddress(float x) const;
    int markerAt(juce::Point<float>) const; // the marker under the pointer, as a Field, or -1
    void paintWave(juce::Graphics&);
    void paintControl(juce::Graphics&, const Control&);
    int controlAt(juce::Point<int>) const;

    AppModel& model_;
    int track_;
    juce::Rectangle<int> header_, wave_, controlsPanel_, waveBox_;
    std::vector<Control> controls_; // laid out in resized
    juce::TextButton revert_ { "REVERT" };
    juce::TextButton replace_ { "REPLACE..." };
    AudioSampleChooser chooser_;
    TestKeyboard keyboard_;
    int dragging_ = -1;     // the marker or control being moved, as a Field
    int dragFrom_ = 0;      // a control's value when the turn began
    float dragY_ = 0;       // and the pointer's height
    bool dragStep_ = false; // the drag has not made its undo step yet
    static constexpr int kKeyboardHeight = 70;
    juce::TooltipWindow tooltips_ { this, 400 }; // the main window's shows only its own components' tips
};

// The floating window that holds a SampleView. The app's command keys work
// in it too, as in the patch window.
class SampleWindow : public juce::DocumentWindow
{
public:
    SampleWindow(AppModel& model, int track, juce::LookAndFeel* look, juce::ApplicationCommandManager& commands);
    ~SampleWindow() override;
    void closeButtonPressed() override;
    bool keyPressed(const juce::KeyPress&) override;
    std::function<void()> onClose;

private:
    juce::ApplicationCommandManager& commands_;
};

} // namespace opm86
