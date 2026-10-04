#pragma once

#include "AppModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <limits>
#include <map>
#include <set>

namespace opm86
{

// The tracks over time: headers on the left, a bar ruler on top, one lane
// per track with its notes as blocks. Zoom and scroll move the view; the
// overview strip and the top bar follow it.
class TrackerView : public juce::Component, public juce::SettableTooltipClient, public juce::ChangeBroadcaster,
                    private juce::ChangeListener
{
public:
    explicit TrackerView(AppModel& model);
    ~TrackerView() override;

    // The view: song time at the left edge of the lanes, and zoom.
    double viewStart() const { return viewStart_; }
    double samplesPerPixel() const { return samplesPerPixel_; }
    double visibleSamples() const;
    void setViewStart(double songTime);
    void zoomAround(double factor, float x);
    float middle() const { return static_cast<float>(kHeaderWidth + getWidth()) * 0.5f; } // centre of the lanes
    void followPlayhead(); // keeps the playhead in view while playing
    void updateMeters(double seconds); // reads the chips' levels; call each frame with the time since the last

    // Vertical zoom: the height of every lane, kept around the pointer.
    int laneHeight() const { return laneHeight_; }
    void setCollapsed(int track, bool collapsed); // a chord part in one lane, or a row per voice
    void zoomLanes(double factor, float y);

    // Snap grid in VGM samples; 0 is off.
    void setSnap(double samples) { snap_ = samples; }
    double snap() const { return snap_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int kHeaderWidth = 300;
    static constexpr int kRulerHeight = 30;
    static constexpr int kMinLaneHeight = 46;
    static constexpr int kMaxLaneHeight = 220;

private:
    // A note by its track and its id, which edits keep.
    struct NoteRef
    {
        int track = -1, id = -1;
        bool valid() const { return track >= 0; }
        bool operator<(const NoteRef& o) const { return track != o.track ? track < o.track : id < o.id; }
        bool operator==(const NoteRef& o) const { return track == o.track && id == o.id; }
    };

    // Chords: each note's row inside its lane, and each lane's row count.
    struct Layout
    {
        std::vector<std::vector<int>> row;
        std::vector<int> rows;
        std::vector<int64_t> longest; // per track, to find notes reaching into view
        std::vector<std::map<int, int>> index; // per track: note id to index
        std::vector<std::vector<int>> voices;  // per track: chip channels, one row each, when more than one
        std::vector<int> shown;                // per track: rows high, 1 for a collapsed chord part
        std::vector<int> firstRow;             // per track: rows above it; each row is one lane high
        int totalRows = 0;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    const Layout& layout() const; // rebuilt when the tune has changed

    double xToTime(float x) const;
    float timeToX(double t) const;
    int yToTrack(float y) const;
    juce::Rectangle<float> noteBounds(int track, int note) const; // note by index
    int indexOf(NoteRef ref) const;
    NoteRef noteAt(juce::Point<float> p, bool& onRightEdge) const;
    double snapTime(double t) const;
    void setScrollY(int y);
    int trackTop(int track) const;    // a track is as many lanes high as it has rows
    int trackHeight(int track) const;
    juce::Rectangle<int> nameBounds(int track) const;
    juce::Rectangle<int> pill(int track, int which) const; // 0 mute, 1 solo, after the name
    juce::Rectangle<int> voicePill(int track, int k) const; // the k-th voice's mute, beside its row
    juce::Rectangle<int> meterBounds(int track) const;      // the level meter under the name
    juce::Rectangle<int> knobBounds(int track) const;       // the volume knob, right of the meter
    juce::Rectangle<int> disclosure(int track) const;       // a chord part's expand and collapse arrow
    bool expanded(int track) const;                          // a chord part showing a row per voice
    static float knobPosition(float volume);                // dB to 0..1 round the knob
    static float knobVolume(float position);                // and back
    void setVolume(int track, float volume);

    // The buttons after the track count: 0 clears every mute and solo, 1
    // solos the melody (the FM tracks), 2 the drums. Again, it clears them.
    enum Corner { kReset, kMelody, kDrums, kCorners };
    juce::Rectangle<int> cornerButton(int which) const;
    int cornerAt(juce::Point<float> p) const; // -1 when on none
    bool cornerOn(int which) const;           // melody or drums alone are soloed; reset: anything to clear
    void pressCorner(int which);
    void paintCorner(juce::Graphics&, int which);

    void paintRuler(juce::Graphics&);
    void paintLanes(juce::Graphics&);
    void paintHeaders(juce::Graphics&);
    void paintNote(juce::Graphics&, int track, int note, juce::Rectangle<float> r);
    bool isSounding(const TuneNote& n) const; // under the playhead while playing

    // Editing.
    enum class Drag { None, Move, Resize, Box, Ruler, Scroll, Knob };
    void transposeSelection(int semitones);
    void deleteSelection();
    void duplicateSelection();
    void editPitch(NoteRef ref);
    void addNoteAt(int track, double time);

    AppModel& model_;
    mutable Layout layout_;
    mutable int layoutRevision_ = -1;
    double viewStart_ = 0;
    double samplesPerPixel_ = 120;
    int scrollY_ = 0;
    int laneHeight_ = 54;
    double laneHeightExact_ = 54; // the zoom, unrounded, so small pinch steps add up
    double snap_ = 0;

    std::set<NoteRef> selection_;
    std::set<int> collapsed_; // chord parts drawn in one lane
    Drag drag_ = Drag::None;
    juce::Point<float> dragFrom_;
    NoteRef dragNote_;
    int dragTrack_ = -1;                   // the track whose knob is turned
    float dragKnob_ = 0;                   // the knob's position when the turn began
    bool dragStarted_ = false;            // the knob turn has made its undo step
    double dragDelta_ = 0;                 // move or resize amount, in samples, while dragging
    juce::Rectangle<float> box_;
    NoteRef hover_;
    int hoverCorner_ = -1;

    // Meters: each chip channel belongs to the track of the note last started
    // on it, so a release still counts for its track.
    float trackLevel(int track) const; // loudest owned channel, linear, 1 at full scale
    struct Owner
    {
        int64_t start;
        int track, row; // row: the voice's row in a chord part, else 0
        bool operator<(const Owner& o) const { return start < o.start; }
    };
    std::array<std::vector<Owner>, SongPlayer::kMeterChannels> owners_; // by start
    int ownersRevision_ = -1;
    std::array<float, SongPlayer::kMeterChannels> channelLevel_ {};
    std::vector<float> level_, hold_; // per track, 0..1 along the meter
    std::vector<double> holdAge_;     // seconds since the hold was set
    bool hoverEdge_ = false;
};

} // namespace opm86
