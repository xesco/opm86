#pragma once

#include "AppModel.h"
#include "ChipWindow.h"
#include "PatchWindow.h"
#include "SampleWindow.h"
#include "TrackerView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace opm86
{

// A transport button that draws its symbol.
class IconButton : public juce::TextButton
{
public:
    enum class Icon { Rewind, Play, Stop, Playhead, Loop, Reset, Instruments, Drums, Chip, FitTracks, FitTime, FitBoth, Home };
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
// stop, LED counters, the file and Open. A click on the BPM counter opens
// the song's tempo and meter in a callout.
class TopBar : public juce::Component, public juce::TooltipClient
{
public:
    TopBar(AppModel& model, TrackerView& view);
    ~TopBar() override;

    void rewind();    // back to the song start, playing on if it was
    void showTempo(); // the tempo callout, under the BPM counter

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    juce::String getTooltip() override; // the file's full path, over its name; what the BPM counter does

    std::function<void()> onNew, onOpen, onSave, onJson;
    juce::Component& jsonButton() { return json_; } // where the JSON menu opens

private:
    AppModel& model_;
    TrackerView& view_;
    IconButton rewind_ { IconButton::Icon::Rewind }, play_ { IconButton::Icon::Play }, stop_ { IconButton::Icon::Stop };
    juce::TextButton new_ { "NEW" }, open_ { "OPEN" }, save_ { "SAVE" }, json_ { "JSON" };
    juce::Rectangle<int> counters_, fileArea_, bpmArea_;
    juce::Component::SafePointer<juce::CallOutBox> tempo_; // the tempo callout, while open
};

// A jog wheel: turning it, by dragging round or with the scroll wheel,
// reports how far in turns, clockwise forward; with Shift, a quarter as far.
class JogWheel : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void(double turns)> onTurn;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void turn(double turns, bool fine);
    float angleAt(juce::Point<float> p) const;
    float angle_ = 0, lastAngle_ = 0; // radians: the wheel's, and the pointer's at the last drag
};

// Choices in rows of a few, one lit: the snap grid's values.
class GridPicker : public juce::Component, public juce::SettableTooltipClient
{
public:
    GridPicker(juce::StringArray labels, int columns) : labels_(std::move(labels)), columns_(columns) {}
    int selected() const { return selected_; }
    void setSelected(int index) { selected_ = index, repaint(); }
    std::function<void()> onChange;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

private:
    juce::Rectangle<float> segment(int index) const;
    juce::StringArray labels_;
    int columns_;
    int selected_ = 0;
};

// The tool bar under the top bar: snap, the playhead and loop toggles, the tracks'
// M/S shortcuts, the chip window's button, speed and the jog wheel, the fit
// buttons and the zoom sliders.
class ToolBar : public juce::Component
{
public:
    ToolBar(AppModel& model, TrackerView& view);

    void songOpened(); // a new song: its snap grid, and its first time zoom as the zoom's home
    void syncZoom();   // the zoom sliders follow the view, however it was zoomed
    void refresh();    // the M/S shortcuts light up as the tracks' solos stand, the loop as it stands, the snap on the grid
    void defaultZoom(); // both zoom sliders back to their homes
    bool keepsPlayhead() const { return keepPlayhead_.getToggleState(); }
    void setKeepPlayhead(bool keep); // the toggle and the view's playhead
    std::function<void()> onChip;    // the chip button was clicked
    std::function<void()> onLoop;    // the loop button was clicked
    void setChipOpen(bool open) { chip_.setToggleState(open, juce::dontSendNotification); } // lit while the chip window is open

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    enum class Only { None, Instruments, Drums };
    Only soloed() const; // every instrument soloed and no drum, or the other way round
    void soloOnly(Only only);

    AppModel& model_;
    TrackerView& view_;
    GridPicker snap_ { { "OFF", "1", "1/2", "1/4", "1/8", "1/16" }, 3 };
    IconButton keepPlayhead_ { IconButton::Icon::Playhead }; // on, the playhead never fades
    IconButton loop_ { IconButton::Icon::Loop };             // lit while the loop is on
    IconButton reset_ { IconButton::Icon::Reset }, instruments_ { IconButton::Icon::Instruments }, drums_ { IconButton::Icon::Drums };
    IconButton chip_ { IconButton::Icon::Chip }; // the chip window, open or closed
    IconButton fitTracks_ { IconButton::Icon::FitTracks }, fitTime_ { IconButton::Icon::FitTime }, fitBoth_ { IconButton::Icon::FitBoth };
    IconButton zoomHome_ { IconButton::Icon::Home }; // both zoom sliders back to their homes
    SnapSlider speed_, heightZoom_, timeZoom_;
    JogWheel jog_;
    juce::Rectangle<int> speedCaption_, heightCaption_, timeCaption_; // labels over the sliders
    bool rowZoom_ = false; // the height slider sets the piano roll's rows, not the tracks' lanes
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

    // Mouse events below the top edge, while open; the mouse up of any press
    // that did not drag the height, open or not.
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
    juce::MouseCursor contentCursor() const override { return juce::MouseCursor::DraggingHandCursor; }
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    double songLength() const;
    juce::Rectangle<float> lanes() const;

    AppModel& model_;
    TrackerView& view_;
    juce::Image density_; // drawn once per edit
    int densityRevision_ = -1;
    double grabOffset_ = 0;
};

// A keyboard over the song's range, lighting the keys of the notes sounding
// at the playhead in their pitch colours. With the song stopped, a key
// sounds while the mouse holds it down, with the instrument of the track
// previewTrack gives, and a drag goes from key to key.
class PianoStrip : public SlidingStrip
{
public:
    explicit PianoStrip(AppModel& model) : SlidingStrip("KEYS"), model_(model) {}
    void paint(juce::Graphics&) override;
    std::function<int()> previewTrack; // the FM track whose instrument the keys play, or -1 for none
    void letGo() { hold(-1); }         // the key down goes up, as when the song starts

private:
    void contentDown(const juce::MouseEvent&) override;
    void contentDrag(const juce::MouseEvent&) override;
    void contentUp(const juce::MouseEvent&) override;
    juce::MouseCursor contentCursor() const override { return juce::MouseCursor::PointingHandCursor; }
    void updateRange();
    juce::Rectangle<float> keysArea() const;
    int keyAt(juce::Point<float>) const; // a black key over the white ones; -1 off the keys
    void hold(int key);                  // lets go of the key down, then holds this one; with -1 only lets go
    AppModel& model_;
    int revision_ = -1;
    int low_ = 36, high_ = 96; // MIDI keys shown, whole octaves C to B
    int held_ = -1;            // the key the mouse holds down, or -1
};

// The app's commands: every action the menus, the keys and the shortcut sheet offer.
namespace command
{
enum : juce::CommandID
{
    newSong = 1, open, save, saveAs, exportJson, importJson, exportVgm, exportWav, exportMidi, cancelExport,
    undo, redo, cut, copy, paste, deleteNotes, duplicate, selectAll, up, down, octaveUp, octaveDown,
    fineUp, fineDown, fineUpEighth, fineDownEighth, louder, quieter, deselect, moveConflicts,
    setTempo, barOne,
    play, rewind, back, forward, loopSelection, loop, clearLoop,
    zoomIn, zoomOut, taller, shorter, fitTracks, fitTime, fitBoth, defaultZoom, keepPlayhead, showKeys, showOverview, showExpression, chip,
    shortcuts, pianoRoll, ghostNotes
};
} // namespace command

// Every command and its keys, by category, as the command manager has them,
// so it never goes out of date.
class ShortcutSheet : public juce::Component
{
public:
    explicit ShortcutSheet(juce::ApplicationCommandManager& commands);
    void paint(juce::Graphics&) override;

private:
    struct Column
    {
        juce::StringArray categories;
        int height = 0;
    };
    static juce::String describe(const juce::KeyPress&); // as the tooltips write keys: "Cmd+Shift+S"
    int categoryHeight(const juce::String& category) const;
    juce::ApplicationCommandManager& commands_;
    std::vector<Column> columns_;
};

class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel, private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    bool open(const juce::File& file);
    void openDialog();
    // Saves the project: where it was, or asked the first time; then does what follows.
    void save(std::function<void()> then = {});
    void saveAs(std::function<void()> then = {});
    // Does something that loses the changes not saved, after asking whether to save them first.
    void whenSaved(std::function<void()> then);
    void newSong();               // an empty song to write (see AppModel::newProject)
    void exportJson();            // asks where, then writes the project as JSON
    void importJson();            // asks which, then opens a project's JSON
    // Exports the song as it sounds here, each asking where (see AppModel):
    // a VGM; a WAV, asked first how to render it, rendered in the background
    // with its progress in the status line; or the notes as MIDI.
    void exportVgm();
    void exportWav();
    void exportMidi();
    juce::PropertiesFile& settings() { return *settings_.getUserSettings(); } // the app's settings, kept between runs
    AppModel& model() { return model_; }
    TrackerView& view() { return view_; }
    void matchHeaderWidths(); // the strips under the tracks line up with the view's header column, as each frame does
    juce::ApplicationCommandManager& commands() { return commands_; }
    void showShortcuts(); // the shortcut sheet, in a window of its own

    // The commands, all handled here, whichever window has the focus.
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>&) override;
    void getCommandInfo(juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform(const InvocationInfo&) override;

    // The menu bar: File, Edit, Song, Transport, View and Help.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int, int) override {}

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
    void updateTitle();           // the window's title: the app and the song open
    void opened();                // sets the view for a song just opened or made
    LoopRegion toggledLoop() const; // the loop on or off; with none, a new one on
    juce::File exportTarget(const juce::String& extension); // beside the project, else in the last folder, named after the song
    void closePatch();            // closes the patch window, keeping where it was
    void closeSample();           // closes the sample window, keeping where it was
    void openChip();              // opens the chip window where it was last, or brings it to the front
    void closeChip();             // closes the chip window, keeping where it was
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
    juce::ApplicationCommandManager commands_;
    AppModel model_;
    TrackerView view_ { model_ };
    TopBar top_ { model_, view_ };
    ToolBar tools_ { model_, view_ };
    OverviewStrip overview_ { model_, view_ };
    PianoStrip piano_ { model_ };
    juce::TooltipWindow tooltips_ { this, 400 };
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::ApplicationProperties settings_; // remembers the last folder opened
    std::unique_ptr<PatchWindow> patch_;   // the patch window, while open
    std::unique_ptr<ChipWindow> chip_;     // the chip window, while open
    std::unique_ptr<juce::DialogWindow> shortcuts_; // the shortcut sheet, once shown
    int patchTrack_ = -1;                  // the track it shows
    std::string patchName_;                // and that track's name, to tell when a removal moved it
    std::unique_ptr<SampleWindow> sample_; // the sample window, while open, beside the patch window
    int sampleTrack_ = -1;                 // the drum track it shows
    std::string sampleName_;               // and that track's name
    juce::String status_;
    int exportPercent_ = -1; // the WAV render's progress the status line shows
    bool dragOver_ = false;
    int pianoHeight_ = 66, overviewHeight_ = 54; // dragged from their first size up to twice the keys' first size
    bool keysByRoll_ = false; // the piano roll closed the KEYS strip, to open again when the roll closes
    double pianoShown_ = 0, overviewShown_ = 0; // heights now, easing to open or closed
    Activity activity_ { view_ };
    double lastFrame_ = 0;
    juce::VBlankAttachment vblank_ { this, [this](double timestamp) { frame(timestamp); } };
};

} // namespace opm86
