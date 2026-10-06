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
    float middle() const { return static_cast<float>(headerWidth() + getWidth()) * 0.5f; } // centre of the lanes
    int headerWidth() const { return layout().headerWidth; } // as wide as the tracks' names, knobs and voice toggles need
    float lanesX() const { return static_cast<float>(headerWidth()); }      // where the lanes begin
    void followPlayhead(); // keeps the playhead in view while playing
    void centrePlayhead(); // following playback, the playhead to the middle of the view at once
    void reveal(double songTime); // the view follows a playhead there as it does while playing: up to the middle, then scrolling
    void fitTime();   // the whole song across, as far as the zoom goes
    void fitTracks(); // every track's rows down, as far as the lane height goes
    void updateMeters(double seconds); // reads the chips' levels; call each frame with the time since the last
    void wake(); // a mouse or key event anywhere: shows the playhead, which hides after a second without one
    void setKeepPlayhead(bool keep) { keepPlayhead_ = keep; repaint(); } // or always shows it
    double idleSeconds() const { return (juce::Time::getMillisecondCounterHiRes() - lastActivity_) / 1000.0; } // since the last wake()
    void setFollow(bool follow) { follow_ = follow; } // off, the view stays where it is put while playing
    // On, the FM tracks go in patch order, FM 1 first, and each header names
    // the patch first and the voice after; off, in voice order, voice first.
    void setOrderByPatch(bool byPatch);
    bool orderByPatch() const { return byPatch_; }
    int position(int track) const { return layout().position[static_cast<size_t>(track)]; } // its place top down
    bool follow() const { return follow_; }

    // Vertical zoom: the height of every lane, kept around the pointer.
    int laneHeight() const { return laneHeight_; }
    void setCollapsed(int track, bool collapsed); // a chord part in one lane, or a row per voice
    void zoomLanes(double factor, float y);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;
    bool keyPressed(const juce::KeyPress&) override;
    // A note's name and length, but none while playing.
    juce::String getTooltip() override { return model_.playing() ? juce::String() : SettableTooltipClient::getTooltip(); }

    static constexpr int kRulerHeight = 30;
    // Lanes under kCompactHeight drop the meter and knob for one row with the
    // name, mute, solo and instrument; under kMinimalHeight, only the name
    // and small mute and solo.
    static constexpr int kMinLaneHeight = 20;
    static constexpr int kMinimalHeight = 28;
    static constexpr int kCompactHeight = 46;
    static constexpr int kMaxLaneHeight = 80;
    static constexpr double kMinSamplesPerPixel = 50, kMaxSamplesPerPixel = 10000; // time zoom, in and out

    // A volume knob's turn: dB from silence to +6 dB, to 0..1 round the knob, 0 dB at its middle; and back.
    static float knobPosition(float volume);
    static float knobVolume(float position);

private:
    // A note by its track and its id.
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
        std::vector<int> position;             // per track: its place top down, in the order shown
        int totalRows = 0;
        int nameEnd = 0;        // the names' shared column ends here
        int knobX = 0;          // the knobs' column, right after the widest name line
        int headerWidth = 200;  // up to the knobs' column, and the voice toggles' when a chord part shows them
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    const Layout& layout() const; // rebuilt when the tune has changed

    double xToTime(float x) const;
    float timeToX(double t) const;
    int yToTrack(float y) const;
    juce::Rectangle<float> noteBounds(int track, int note) const; // note by index
    int indexOf(NoteRef ref) const;
    NoteRef noteAt(juce::Point<float> p) const;
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
    bool compact() const { return laneHeight_ < kCompactHeight; }
    bool minimal() const { return laneHeight_ < kMinimalHeight; }
    int pillWidth() const { return minimal() ? 18 : 23; } // mute and solo
    void setVolume(int track, float volume);

    void paintRuler(juce::Graphics&);
    void paintLanes(juce::Graphics&);
    void paintHeaders(juce::Graphics&);
    void paintNote(juce::Graphics&, int track, int note, juce::Rectangle<float> r);
    bool isSounding(const TuneNote& n) const; // under the playhead while playing
    double playhead() const { return model_.playing() ? head_ : model_.displayPosition(); } // this frame's, while playing

    enum class Drag { None, Lanes, Pan, Select, SelectEdge, SelectMove, Ruler, Knob };

    AppModel& model_;
    mutable Layout layout_;
    mutable int layoutRevision_ = -1;
    double viewStart_ = 0;
    double samplesPerPixel_ = 120;
    int scrollY_ = 0;
    int laneHeight_ = kCompactHeight; // the smallest lanes that keep the meters
    double laneHeightExact_ = kCompactHeight; // the zoom, unrounded, so small pinch steps add up
    double lastActivity_ = 0; // milliseconds, when wake() was last called
    bool keepPlayhead_ = false;
    bool follow_ = true;
    bool byPatch_ = false;
    double head_ = 0; // the playhead when this frame began

    std::set<int> collapsed_; // chord parts drawn in one lane
    Drag drag_ = Drag::None;
    juce::Point<float> dragFrom_;
    double panFrom_ = 0;                   // the view's start when the hand took it
    double selectFrom_ = 0;                // song time where a selection drag began, or the edge held still, or the grab in a moved one
    bool overSelectionBar(juce::Point<float> p) const; // stopped, on the ruler inside the selection, where a drag moves it
    int selectionEdgeAt(juce::Point<float> p) const; // 0 start, 1 end, -1 neither: within a few pixels of one
    static constexpr double kGlideFrom = 60;  // pixels: a follow jump longer than this glides
    static constexpr double kGlideMs = 350;   // and takes this long
    static constexpr double kHandsOffMs = 600; // following waits this long after the view is moved by hand
    void movedByHand() { movedByHand_ = juce::Time::getMillisecondCounterHiRes(); pulledAway_ = true; }
    double movedByHand_ = 0;  // ms, when the view was last dragged or scrolled
    bool pulledAway_ = false; // moved by hand since following last caught up: the way back glides
    bool gliding_ = false;
    double glideFrom_ = 0, glideStart_ = 0;   // the view's start and the time, in ms, when the glide began
    int dragTrack_ = -1;                   // the track whose knob is turned
    float dragKnob_ = 0;                   // the knob's position when the turn began
    bool dragStarted_ = false;            // the knob turn has made its undo step
    NoteRef hover_;

    // Meters: each chip channel belongs to the track of the note last started
    // on it, so a release still counts for its track.
    float trackLevel(int track) const; // loudest owned channel, linear, 1 at full scale
    static constexpr float kMeterFloor = -36.0f; // dB at the meter's left end; its right end is full scale
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
};

} // namespace opm86
