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
    enum class Icon { Rewind, Play, Stop };
    explicit IconButton(Icon icon) : icon_(icon) {}
    void paintButton(juce::Graphics&, bool over, bool down) override;

private:
    Icon icon_;
};

// The transport and song display along the top: title, rewind, play and
// stop, LED counters, tempo, snap, zoom and Open.
class TopBar : public juce::Component, public juce::TooltipClient
{
public:
    TopBar(AppModel& model, TrackerView& view);

    void rewind();   // back to the song start, playing on if it was
    void syncZoom(); // the zoom sliders follow the view, however it was zoomed

    void paint(juce::Graphics&) override;
    void resized() override;
    juce::String getTooltip() override; // the file's full path, over its name

    std::function<void()> onOpen;

private:
    AppModel& model_;
    TrackerView& view_;
    IconButton rewind_ { IconButton::Icon::Rewind }, play_ { IconButton::Icon::Play }, stop_ { IconButton::Icon::Stop };
    juce::TextButton open_ { "OPEN" };
    juce::Slider timeZoom_, heightZoom_;
    juce::Rectangle<int> timeCaption_, heightCaption_; // labels over the zoom sliders
    juce::ComboBox snap_;
    juce::Rectangle<int> counters_, fileArea_;
};

// The whole song at a glance: note density per track and the visible window,
// which can be dragged.
class OverviewStrip : public juce::Component, private juce::ChangeListener
{
public:
    OverviewStrip(AppModel& model, TrackerView& view);
    ~OverviewStrip() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

    // Dragging the top edge asks for a new height.
    std::function<void(int)> onHeight;
    static constexpr int kEdge = 6;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    double songLength() const;
    juce::Rectangle<float> lanes() const;

    AppModel& model_;
    TrackerView& view_;
    juce::Image density_; // drawn once per edit
    int densityRevision_ = -1;
    double grabOffset_ = 0;
    bool resizing_ = false;
    int resizeFromY_ = 0, resizeFromHeight_ = 0; // screen y and height where the edge drag began
};

// A keyboard over the song's range, lighting the keys of the notes sounding
// at the playhead in their pitch colours.
class PianoStrip : public juce::Component
{
public:
    explicit PianoStrip(AppModel& model) : model_(model) {}
    void paint(juce::Graphics&) override;

private:
    void updateRange();
    AppModel& model_;
    int revision_ = -1;
    int low_ = 36, high_ = 96; // MIDI keys shown, whole octaves C to B
};

class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    bool open(const juce::File& file);
    void openDialog();
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

    std::unique_ptr<juce::LookAndFeel> look_;
    AppModel model_;
    TrackerView view_ { model_ };
    TopBar top_ { model_, view_ };
    OverviewStrip overview_ { model_, view_ };
    PianoStrip piano_ { model_ };
    juce::TooltipWindow tooltips_ { this, 400 };
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String status_;
    bool dragOver_ = false;
    int overviewHeight_ = 54; // dragged between its first size and twice that
    double lastFrame_ = 0;
    juce::VBlankAttachment vblank_ { this, [this](double timestamp) { frame(timestamp); } };
};

} // namespace opm86
