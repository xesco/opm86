#pragma once

#include "AppModel.h"
#include "ChipView.h"
#include "TrackerView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

// A transport button that draws its symbol.
class IconButton : public juce::TextButton
{
public:
    enum class Icon { Rewind, End, Play, Stop, Playhead, Follow, Loop, Order, Chips, Reset, Invert, Instruments, Drums, FitTracks, FitTime, FitBoth, ZoomHome, FewerKeys, MoreKeys, NoteNames };
    explicit IconButton(Icon icon) : icon_(icon) {}
    void paintButton(juce::Graphics&, bool over, bool down) override;

private:
    Icon icon_;
};

// A slider with a home value, marked on its groove: a drag near it snaps
// to it, and a double-click goes back to it.
class SnapSlider : public juce::Slider
{
public:
    void setHome(double home);
    double snapValue(double attempted, DragMode) override;
};

// A volume knob like a track's: dragged up or right, or scrolled, from
// silence to +6 dB; a double-click puts it back to 0 dB.
class VolumeKnob : public juce::Component, public juce::SettableTooltipClient
{
public:
    float volume() const { return volume_; }
    void setVolume(float dB);
    std::function<void(float dB)> onChange;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override { setVolume(0.0f); }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    float volume_ = 0.0f;
    float from_ = 0.0f; // the knob's position when a drag began
};

// The transport and song display along the top: title, rewind, play, stop
// and to the end, LED counters, the file, the master volume and Open.
class TopBar : public juce::Component
{
public:
    TopBar(AppModel& model, TrackerView& view);

    void rewind(); // back to the song start, or the selection's, playing on if it was
    void toEnd();  // to the selection's end, or the song's

    void paint(juce::Graphics&) override;
    void resized() override;
    void songChanged(); // a song opened or closed: CLOSE shows while one is open

    std::function<void()> onOpen;
    std::function<void()> onClose;

private:
    AppModel& model_;
    TrackerView& view_;
    IconButton rewind_ { IconButton::Icon::Rewind }, play_ { IconButton::Icon::Play }, stop_ { IconButton::Icon::Stop };
    IconButton end_ { IconButton::Icon::End };
    juce::TextButton open_ { "OPEN" };
    juce::TextButton close_ { "CLOSE" }; // beside OPEN, while a song is open
    VolumeKnob master_;
    juce::Rectangle<int> counters_, masterArea_;
};

// The tool bar under the top bar: the playhead, follow and loop toggles, the
// mix buttons, speed, the selection's start and end, the fit buttons and the
// zoom sliders.
class ToolBar : public juce::Component
{
public:
    ToolBar(AppModel& model, TrackerView& view);

    void songOpened(); // a new song: its first time zoom as the zoom's home
    void showTracks(); // the chips' view off, if it is on
    void syncZoom();   // the zoom sliders follow the view, however it was zoomed
    void syncSelection(); // the start and end fields follow the selection, unless being typed in
    void refresh();    // the M/S shortcuts light up as the tracks' solos stand

    void paint(juce::Graphics&) override;
    void resized() override;

    std::function<void(bool shown)> onChips; // the chips' view was turned on or off

private:
    enum class Only { None, Instruments, Drums };
    Only soloed() const; // every instrument soloed and no drum, or the other way round
    void soloOnly(Only only);

    AppModel& model_;
    TrackerView& view_;
    IconButton keepPlayhead_ { IconButton::Icon::Playhead }; // on, the playhead never fades
    IconButton follow_ { IconButton::Icon::Follow };         // on, the view follows the playhead
    IconButton loop_ { IconButton::Icon::Loop };             // on, the selection or the song plays over and over
    IconButton order_ { IconButton::Icon::Order };           // on, the tracks go by patch, FM 1 first; off, by voice
    IconButton chips_ { IconButton::Icon::Chips };           // on, the chips' registers show in place of the tracks
    IconButton reset_ { IconButton::Icon::Reset }, invert_ { IconButton::Icon::Invert }, instruments_ { IconButton::Icon::Instruments }, drums_ { IconButton::Icon::Drums };
    IconButton fitTracks_ { IconButton::Icon::FitTracks }, fitTime_ { IconButton::Icon::FitTime }, fitBoth_ { IconButton::Icon::FitBoth };
    IconButton zoomHome_ { IconButton::Icon::ZoomHome }; // both zoom sliders back to their homes
    SnapSlider speed_, heightZoom_, timeZoom_;
    juce::Rectangle<int> speedCaption_, heightCaption_, timeCaption_; // labels over the sliders
    // The selection's start and end, typed as m:ss.mmm or seconds.
    juce::TextEditor selectionStart_, selectionEnd_;
    juce::Rectangle<int> startCaption_, endCaption_;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> groupCaptions_; // the button groups' names, over them
    std::vector<int> dividers_;                                                // x of the lines between groups
    void applySelection();
};

// A strip under the tracks with a chevron that slides it down to a thin bar
// and back up, and a grip in its top edge that drags it taller. Its content
// keeps the open height while it slides, so it moves down with the strip's
// top edge instead of squashing.
class SlidingStrip : public juce::Component
{
public:
    explicit SlidingStrip(const juce::String& name);

    static constexpr int kClosedHeight = 20; // also the band at the top with the name and chevron
    static constexpr int kEdge = 6;          // pixels at the top edge that drag the height
    bool isOpen() const { return open_; }
    void setOpen(bool open);
    void setHeaderWidth(int width); // the tracks' header column, which the name and chevron sit in
    int headerWidth() const { return headerWidth_; }
    int openHeight = kClosedHeight;

    std::function<void()> onSlide;    // the chevron was clicked
    std::function<void(int)> onHeight; // the top edge was dragged: the height asked for
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

protected:
    juce::Rectangle<int> content() const { return { 0, 0, getWidth(), openHeight }; }
    float contentAlpha() const; // 1 open, falling to 0 as the strip closes
    void paintBar(juce::Graphics&, juce::Colour labelColour); // ground, top line, name and grip

    // Mouse events below the top edge, while open.
    virtual void contentDown(const juce::MouseEvent&) {}
    virtual void contentDrag(const juce::MouseEvent&) {}
    virtual void contentUp(const juce::MouseEvent&) {}
    virtual juce::MouseCursor contentCursor() const { return juce::MouseCursor::NormalCursor; }

private:
    class Chevron : public juce::Button
    {
    public:
        Chevron() : juce::Button("slide") {}
        bool up = false;
        void paintButton(juce::Graphics&, bool over, bool down) override;
    };

    juce::String name_;
    Chevron chevron_;
    bool open_ = true;
    int headerWidth_ = 200;
    bool resizing_ = false;
    bool inContent_ = false; // the press began in the content, right of the header
    int resizeFromY_ = 0, resizeFromHeight_ = 0; // screen y and height where the edge drag began
};

// The whole song at a glance: note density per track and the visible window,
// which can be dragged.
class OverviewStrip : public SlidingStrip, private juce::ChangeListener
{
public:
    OverviewStrip(AppModel& model, TrackerView& view);
    ~OverviewStrip() override;

    void paint(juce::Graphics&) override;

private:
    void contentDown(const juce::MouseEvent&) override;
    void contentDrag(const juce::MouseEvent&) override;
    void contentUp(const juce::MouseEvent&) override;
    juce::MouseCursor contentCursor() const override { return juce::MouseCursor::DraggingHandCursor; }
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void centrePlayhead();
    double songLength() const;
    juce::Rectangle<float> lanes() const;

    AppModel& model_;
    TrackerView& view_;
    juce::Image density_; // drawn once per edit
    int densityRevision_ = -1;
    int colourRevision_ = -1; // the pitch colours the density was drawn with
    bool byPatch_ = false;    // and the track order
    double grabOffset_ = 0;
    bool held_ = false;   // the window carries the playhead: taken while following it
    bool resume_ = false; // playing when the window was taken: plays again on release
};

// A keyboard of 3 to 7 octaves round the song's notes, lighting the keys of
// the notes sounding at the playhead in their pitch colours.
class PianoStrip : public SlidingStrip
{
public:
    explicit PianoStrip(AppModel& model);
    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kMinOctaves = 3, kMaxOctaves = 7;

private:
    void updateRange();
    void setOctaves(int octaves);
    AppModel& model_;
    int revision_ = -1;
    int octaves_ = 5;                // octaves shown, round the song's notes; fewer make bigger keys
    IconButton fewer_ { IconButton::Icon::FewerKeys }, more_ { IconButton::Icon::MoreKeys };
    IconButton names_ { IconButton::Icon::NoteNames }; // on, the notes sounding are named under their keys
    int low_ = 36, high_ = 95; // MIDI keys shown, whole octaves C to B
};

class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    bool open(const juce::File& file);
    void openDialog();
    std::function<void(const juce::File&)> onOpened; // a file was opened
    std::function<void()> onClosed;                  // the song was closed
    void close();                                    // closes the song, back to the songs to start with
    std::function<void()> onColoursChanged;            // a pitch colour was chosen or reset
    AppModel& model() { return model_; }
    TrackerView& view() { return view_; }

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;
    void fileDragExit(const juce::StringArray&) override;

private:
    void frame(double timestamp); // each display refresh
    void updateTitle();           // the window's title: the app and the file open
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    // Any mouse event in the window wakes the tracks' playhead.
    struct Activity : juce::MouseListener
    {
        explicit Activity(TrackerView& v) : view(v) {}
        void mouseMove(const juce::MouseEvent&) override { view.wake(); }
        void mouseDown(const juce::MouseEvent& e) override
        {
            view.wake();
            // A click outside a field being typed in hands the keys back to the view.
            auto* typing = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent());
            if (typing != nullptr && e.originalComponent != typing && !typing->isParentOf(e.originalComponent))
                view.grabKeyboardFocus();
        }
        void mouseDrag(const juce::MouseEvent&) override { view.wake(); }
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override { view.wake(); }
        void mouseMagnify(const juce::MouseEvent&, float) override { view.wake(); }
        TrackerView& view;
    };

    std::unique_ptr<juce::LookAndFeel> look_;
    AppModel model_;
    TrackerView view_ { model_ };
    ChipView chipView_ { model_, view_ }; // in the tracks' place while the tool bar's Chips is on
    bool pianoWasOpen_ = true, overviewWasOpen_ = true; // the strips as they were before the chips showed
    TopBar top_ { model_, view_ };
    ToolBar tools_ { model_, view_ };
    OverviewStrip overview_ { model_, view_ };
    PianoStrip piano_ { model_ };
    juce::TooltipWindow tooltips_ { this, 400 };
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String status_;
    bool dragOver_ = false;
    // The pitch colours' legend in the status bar: a swatch per pitch class,
    // which opens the colours to choose from, and Reset; empty when hidden.
    std::array<juce::Rectangle<int>, 12> swatches_ {};
    juce::Rectangle<int> resetColours_;
    void coloursChanged();
    bool cursorHidden_ = false; // the pointer is hidden, playing without activity
    int pianoHeight_ = 92, overviewHeight_ = 92; // 70% of the greatest height, dragged between their least and greatest
    double pianoShown_ = 0, overviewShown_ = 0; // heights now, easing to open or closed
    Activity activity_ { view_ };
    double lastFrame_ = 0;
    double endedAt_ = -1.0, endedPosition_ = 0.0; // when and where playing stopped at the song's end, until the rewind
    juce::VBlankAttachment vblank_ { this, [this](double timestamp) { frame(timestamp); } };
};

} // namespace opm86
