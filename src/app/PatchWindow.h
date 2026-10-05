#pragma once

#include "AppModel.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

// The patch window's look, which the chip window shares. A read-only dial:
// its name over it, the ring lit up to the value in its colour, the value
// under it in its ink. A setting that is a word, not a number: its name over
// a box holding it, faint when off. A section's rounded panel.
void drawDial(juce::Graphics&, juce::Rectangle<int>, const juce::String& name, float fraction, const juce::String& value, juce::Colour colour,
              juce::Colour ink = theme::text);
void drawWord(juce::Graphics&, juce::Rectangle<int>, const juce::String& name, const juce::String& word, bool on = true, juce::Colour ink = theme::text);
void drawPanel(juce::Graphics&, juce::Rectangle<int>);

// The app's command keys pressed in a floating window, for the main window
// to handle: Space plays, Cmd+Z undoes, Cmd+S saves. The note edits are not
// taken. Returns false for a key no command has.
bool forwardCommandKey(juce::ApplicationCommandManager& commands, const juce::KeyPress& key);

// Two octaves of keys to play a patch with the song stopped: a key sounds
// while the mouse holds it down, and a drag goes from key to key. The
// arrows at its left move it an octave down or up.
class TestKeyboard : public juce::Component
{
public:
    TestKeyboard();
    void setLowest(int key); // the C at its left
    int held() const { return held_; } // the key down, or -1
    std::function<bool(int key, bool on)> onKey; // false: a key down cannot sound, and stays up
    void letGo() { hold(-1); }                   // the key down goes up, as when the song starts

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    static constexpr int kKeys = 25; // two octaves and the C over them
    juce::Rectangle<float> keysArea() const;
    juce::Rectangle<float> keyBounds(int key) const; // by MIDI key, from lowest_
    int keyAt(juce::Point<float>) const;             // a black key over the white ones; -1 off the keys
    void hold(int key); // lets go of the key down, then holds this one; with -1 only lets go
    juce::TextButton down_ { "<" }, up_ { ">" };
    juce::Rectangle<int> octave_; // the lowest key's name, between the arrows
    int lowest_ = 48;
    int held_ = -1;
};

// A track's FM patch, as a synth panel shows it, to edit while the song
// plays: the algorithm, as a diagram of how the operators connect, feedback
// and LFO along the top, the four operators with their settings and
// envelopes, under them a keyboard to play the patch with the song stopped,
// and the patch's sound, all its voices together or one of them, or the
// keyboard's and the note previews' while stopped, as a scope and a
// spectrum. A dial or value box turns by dragging up or down, a quarter as
// fast with Shift, or a step a notch of the mouse wheel; a word or the
// diagram changes by clicking. A right-click or a Ctrl-click types a value,
// and a double-click, but on the diagram, or an Option-click puts back the
// game's value. Each operator's M mutes it to hear the others alone, for as
// long as the window shows the track, without changing the instrument. The
// LFO, which every instrument shares, is shown as the chip has it now, and
// edited in the chip window, which its CHIP button opens. While the chip's
// noise is on, voice V8's C2 operator plays it, and an instrument on V8
// marks its C2 so. Each one's tooltip names it, with its datasheet name,
// register and raw value.
class PatchView : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    PatchView(AppModel& model, int track);
    ~PatchView() override;
    juce::String getTooltip() override; // the setting under the pointer
    std::function<void()> onChip;        // the LFO's CHIP button was clicked

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    void choose(int index); // the voice the live views follow, by index, or -1 for all
    void paintHeader(juce::Graphics&, juce::Rectangle<int>);
    // The algorithm's connection: the operators as boxes, carriers and
    // modulators in their colours, wired to each other and to the output,
    // with M1's feedback loop and its value.
    void paintDiagram(juce::Graphics&, juce::Rectangle<int>, const FmPatch&);
    void paintOperator(juce::Graphics&, juce::Rectangle<int>, int op, float level, int stage);

    // The patch's settings that can be edited, and how each is drawn.
    enum class Field { Alg, Fb, Pms, Ams, Tl, Mul, Dt1, Dt2, Ks, AmEn, Ar, D1r, D1l, D2r, Rr, On };
    enum class Look { Dial, Word, Box, Toggle, Lfo, Diagram };
    struct Control
    {
        Field field;
        int op; // 0-3 in patch order for an operator's setting, else -1
        Look look;
        juce::Rectangle<int> bounds;
        const char* name;
    };
    static int get(const FmPatch&, Field, int op);
    static void set(FmPatch&, Field, int op, int value);
    static juce::Range<int> range(Field);
    static juce::String text(Field, int value);
    static juce::String describe(Field, int value); // its name in plain words, datasheet name, register and raw value
    // A value typed in the units shown: dB for a level, the ratio or its
    // index for a coarse, ON or OFF for a switch, else the raw number. None
    // for text that is not one; a number out of range is clamped.
    static std::optional<int> parse(Field, const juce::String& typed);
    const FmPatch& original() const; // the patch as imported
    FmPatch patch() const;           // and as edited, or as imported
    void commit(const FmPatch& patch, bool newStep);
    void reset(int control);                     // back to the game's value, as one undo step
    void resetHandle(int op, int which);         // a handle's settings back to the game's, as one undo step
    bool step(int control, int by, bool newStep); // up or down by steps, within its range; false when it stays
    void paintControl(juce::Graphics&, const Control&);
    int controlAt(juce::Point<int>) const;
    // Typing a value: a box over the control's value; Return sets it as one
    // undo step, Escape or a click elsewhere leaves it as it was.
    void openEntry(int control);
    void closeEntry(bool apply);
    juce::TextEditor entry_;
    int entering_ = -1; // the control being typed, or -1

    // An operator's envelope as the chip runs it for a middle C, the key held
    // kHold after the first decay ends: in attenuation steps (0 loudest, 1023
    // silent, about 96 dB) against seconds from the key on.
    struct Envelope
    {
        bool silent = false;          // an attack rate of 0 never rises
        double attackK = 0;           // the attack's exponential speed, a second
        double attack = 0, decay = 0; // seconds the attack and the first decay last
        double sustain = 0;           // the level the first decay stops at
        double decaySpeed = 0, sustainSpeed = 0, releaseSpeed = 0; // steps a second
        double keyOff() const { return attack + decay + kHold; }
        double release() const;       // seconds the release lasts
        double at(double seconds) const;
    };
    static constexpr double kHold = 0.5;          // seconds the key is held after the first decay
    static constexpr float kEnvelopeRange = 60.0f; // dB from the curve's top to its bottom
    Envelope envelopeOf(const FmOperator&) const;
    static float xOf(double seconds);              // the time axis: 0-1, log-like from 0 to 4 s
    static double secondsAt(float x);
    static float heightOf(double attenuation);     // 0-1 on the curve, by its dB
    juce::Point<float> handle(int op, int which) const; // 0 attack, 1 first decay, 2 key off, 3 release end, in the view
    std::pair<int, int> handleAt(juce::Point<float>) const; // the handle under the pointer: operator and which, or -1s
    int handleOp_ = -1, handleWhich_ = -1;            // the handle being dragged
    void paintScope(juce::Graphics&, juce::Rectangle<int>, const std::vector<float>& samples);
    void paintSpectrum(juce::Graphics&, juce::Rectangle<int>, const std::vector<float>& samples);
    int soundingPitch() const; // the lowest note the followed voices play now, or the preview's; -1 for none
    bool live() const;         // the song plays or a preview sounds: the live views have a sound to show
    int liveExtra() const;     // pixels the live strip grows by for an instrument on many voices

    AppModel& model_;
    int track_;
    std::vector<int> voices_; // chip voices the track plays on
    int voice_ = -1;          // the one the live views follow, by index; -1 all
    std::array<std::pair<int, int>, 4> levels_ {}; // per operator, the levels the notes set: loudest and quietest
    int pitch_ = 60;          // the instrument's usual note, its median, for the envelope's timing
    int keycode_ = 15;        // and that note's key code, which key scaling reads
    juce::OwnedArray<juce::TextButton> voiceButtons_; // ALL, then a button per voice
    juce::OwnedArray<juce::TextButton> soloButtons_;  // beside each: solo all the voices, then each voice
    juce::OwnedArray<juce::TextButton> sideButtons_;  // and the output sides: of all the voices, then each voice's
    uint8_t sides(int index) const;                   // a voice's sides as heard: as edited, or as the game has them
    void stepSides(int index);                        // a voice's sides, or with -1 every voice's, on to the next
    void solo(int index); // a voice's solo, by index, or with -1 every voice's
    float scopePeak_ = 0.05f; // the scope's level, following the wave slowly
    std::vector<float> spectrum_; // per pixel column, in dB, falling back slowly
    double lastPaint_ = 0;    // milliseconds, for the spectrum's fall
    juce::Rectangle<int> header_, operators_, live_;
    std::vector<Control> controls_;         // laid out in resized
    std::array<juce::Rectangle<int>, 4> panels_, envelopes_; // per operator, its panel and its curve's box
    juce::Rectangle<int> lfoBox_;           // the LFO's group: its frame
    juce::Rectangle<int> lfoSens_, lfoNote_; // and in it this instrument's sensitivities, and the note on the chip's LFO
    juce::Rectangle<int> noiseMark_;        // the NOISE mark on C2, while it shows
    std::array<juce::Rectangle<int>, 4> mutes_; // per operator, its audition mute's button
    juce::TextButton revert_ { "REVERT" }, save_ { "SAVE" }, load_ { "LOAD" }, chip_ { "CHIP" };
    std::unique_ptr<juce::AlertWindow> naming_; // asks a saved instrument's name
    void saveInstrument();                      // under a name the user gives
    void loadInstrument();                      // one saved before, chosen from a menu, and its LFO on the chip if asked
    // An instrument's own settings onto the track as one step, and the LFO
    // it has, when lfo says it has one, on the chip if the user agrees.
    void applyInstrument(const FmPatch& from, bool lfo, const juce::String& name);
    void importOpm();                           // a voice of a .opm bank, picked by number and name
    void exportOpm();                           // the instrument as a .opm bank of one voice
    std::unique_ptr<juce::FileChooser> chooser_;
    int dragging_ = -1;                     // the control being turned
    int dragFrom_ = 0;                      // its value when the turn began
    float dragY_ = 0;                       // the pointer's height at its last move
    float dragSteps_ = 0;                   // and the steps turned since the turn began, a quarter as many with Shift
    bool dragStep_ = false;                 // the turn has not made its undo step yet
    int wheelControl_ = -1;                 // the control the wheel turned last
    double wheelAt_ = 0;                    // when, in milliseconds; a pause ends the wheel's undo step
    float wheelSum_ = 0;                    // a trackpad's scroll not yet a whole step
    bool wheelStep_ = false;                // the wheel's turn has not made its undo step yet
    static constexpr int kKeyboardHeight = 70;
    TestKeyboard keyboard_;
    FmPatch sent_;                          // the patch a key held plays, sent again when it changes
    juce::TooltipWindow tooltips_ { this, 400 }; // the main window's shows only its own components' tips
};

// The floating window that holds a PatchView. The app's command keys work
// in it too: Space plays, Cmd+Z undoes, Cmd+S saves. The note edits do not.
class PatchWindow : public juce::DocumentWindow
{
public:
    PatchWindow(AppModel& model, int track, juce::LookAndFeel* look, juce::ApplicationCommandManager& commands);
    ~PatchWindow() override;
    void closeButtonPressed() override;
    bool keyPressed(const juce::KeyPress&) override;
    std::function<void()> onClose;
    std::function<void()> onChip; // the chip window is asked for

private:
    juce::ApplicationCommandManager& commands_;
};

} // namespace opm86
