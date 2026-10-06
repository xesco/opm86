#pragma once

#include "AppModel.h"
#include "TrackerView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

// A transport button that draws its symbol.
class IconButton : public juce::TextButton
{
public:
    enum class Icon { Rewind, Play, Stop, Playhead, Follow, Loop, Reset, Invert, Instruments, Drums, FitTracks, FitTime, FitBoth, ZoomHome, FewerKeys, MoreKeys };
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

// The transport and song display along the top: title, rewind, play and
// stop, LED counters, the file and Open.
class TopBar : public juce::Component, public juce::TooltipClient
{
public:
    TopBar(AppModel& model, TrackerView& view);

    void rewind(); // back to the song start, playing on if it was

    void paint(juce::Graphics&) override;
    void resized() override;
    juce::String getTooltip() override; // the file's full path, over its name

    std::function<void()> onOpen;

private:
    AppModel& model_;
    TrackerView& view_;
    IconButton rewind_ { IconButton::Icon::Rewind }, play_ { IconButton::Icon::Play }, stop_ { IconButton::Icon::Stop };
    juce::TextButton open_ { "OPEN" };
    juce::Rectangle<int> counters_, fileArea_;
};

// The tool bar under the top bar: the playhead and follow toggles, the tracks'
// M/S shortcuts, speed, the fit buttons and the zoom sliders.
class ToolBar : public juce::Component
{
public:
    ToolBar(AppModel& model, TrackerView& view);

    void songOpened(); // a new song: its first time zoom as the zoom's home
    void syncZoom();   // the zoom sliders follow the view, however it was zoomed
    void refresh();    // the M/S shortcuts light up as the tracks' solos stand

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    enum class Only { None, Instruments, Drums };
    Only soloed() const; // every instrument soloed and no drum, or the other way round
    void soloOnly(Only only);

    AppModel& model_;
    TrackerView& view_;
    IconButton keepPlayhead_ { IconButton::Icon::Playhead }; // on, the playhead never fades
    IconButton follow_ { IconButton::Icon::Follow };         // on, the view follows the playhead
    IconButton loop_ { IconButton::Icon::Loop };             // on, the selection or the song plays over and over
    IconButton reset_ { IconButton::Icon::Reset }, invert_ { IconButton::Icon::Invert }, instruments_ { IconButton::Icon::Instruments }, drums_ { IconButton::Icon::Drums };
    IconButton fitTracks_ { IconButton::Icon::FitTracks }, fitTime_ { IconButton::Icon::FitTime }, fitBoth_ { IconButton::Icon::FitBoth };
    IconButton zoomHome_ { IconButton::Icon::ZoomHome }; // both zoom sliders back to their homes
    SnapSlider speed_, heightZoom_, timeZoom_;
    juce::Rectangle<int> speedCaption_, heightCaption_, timeCaption_; // labels over the sliders
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
    double grabOffset_ = 0;
    bool held_ = false;   // the window carries the playhead: taken while following it
    bool resume_ = false; // playing when the window was taken: plays again on release
};

// A keyboard over the song's range, lighting the keys of the notes sounding
// at the playhead in their pitch colours.
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
    AppModel& model() { return model_; }
    TrackerView& view() { return view_; }

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
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
        void mouseDown(const juce::MouseEvent&) override { view.wake(); }
        void mouseDrag(const juce::MouseEvent&) override { view.wake(); }
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override { view.wake(); }
        void mouseMagnify(const juce::MouseEvent&, float) override { view.wake(); }
        TrackerView& view;
    };

    std::unique_ptr<juce::LookAndFeel> look_;
    AppModel model_;
    TrackerView view_ { model_ };
    TopBar top_ { model_, view_ };
    ToolBar tools_ { model_, view_ };
    OverviewStrip overview_ { model_, view_ };
    PianoStrip piano_ { model_ };
    juce::TooltipWindow tooltips_ { this, 400 };
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String status_;
    bool dragOver_ = false;
    int pianoHeight_ = 66, overviewHeight_ = 54; // dragged from their first size up to twice the keys' first size
    double pianoShown_ = 0, overviewShown_ = 0; // heights now, easing to open or closed
    Activity activity_ { view_ };
    double lastFrame_ = 0;
    juce::VBlankAttachment vblank_ { this, [this](double timestamp) { frame(timestamp); } };
};

} // namespace opm86
