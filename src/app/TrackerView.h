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
    void reveal(double songTime); // the view follows a playhead there as it does while playing: up to the middle, then scrolling
    void fitTime();   // the whole song across, as far as the zoom goes
    void fitTracks(); // every track's rows down, as far as the lane height goes
    void updateMeters(double seconds); // reads the chips' levels; call each frame with the time since the last
    std::function<void(int track)> onOpenPatch;  // an FM track's gear was clicked
    std::function<void(int track)> onOpenSample; // a drum track's gear was clicked
    void wake(); // a mouse or key event anywhere: shows the playhead, which hides after a second without one
    void setKeepPlayhead(bool keep) { keepPlayhead_ = keep; repaint(); } // or always shows it
    // Each ripped note's own pitch and level moves, drawn inside its block (see Tune::expression).
    void setShowExpression(bool show) { showExpression_ = show; repaint(); }
    bool showsExpression() const { return showExpression_; }

    // Vertical zoom: the height of every lane, kept around the pointer.
    int laneHeight() const { return laneHeight_; }
    void zoomLanes(double factor, float y);

    // Snap grid in VGM samples; 0 is off.
    void setSnap(double samples) { snap_ = samples; }
    double snap() const { return snap_; }

    // Editing the selected notes, as the app's commands do.
    bool hasSelection() const { return !selection_.empty(); }
    int selectedTrack() const; // the first selected FM note's, else the first selected note's; -1 for none
    void selectAll();
    void deselect();
    void transposeSelection(int semitones);
    void fineTuneSelection(int sixtyFourths); // the selected notes' fine tune, in 64ths of a semitone
    void changeVelocity(int steps);           // the selected notes' velocity, in 0.75 dB steps
    void deleteSelection();
    void duplicateSelection();
    // The clipboard, in the app only: the selected notes with their tracks
    // and voices, their starts from the earliest's. A paste goes at the
    // playhead, on the snap grid, into the notes' own tracks and voices; or,
    // when they all come from one FM track, into the row clicked since the
    // copy, a ripped note going into another track as a written one.
    void copySelection();
    void cutSelection(); // a copy, then the notes deleted, as one step of undo
    bool canPaste() const; // notes copied from the song open now
    void paste();
    // Every note whose voice is busy to the free voice it plays on (see
    // Tune::moveConflicts), as one step of undo, saying how many moved and
    // how many could not.
    void moveConflicts();
    // The selected notes' span, out to the snap grid either side, as a loop
    // that is on; none with no note selected.
    LoopRegion selectionLoop() const;
    void step(int direction); // the selected notes one snap step later, or earlier with -1; with none selected, the playhead

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;

    static constexpr int kRulerHeight = 30;
    // Lanes under kCompactHeight drop the meter and knob for one row with the
    // name, mute, solo and instrument; under kMinimalHeight, only the name
    // and small mute and solo.
    static constexpr int kMinLaneHeight = 20;
    static constexpr int kMinimalHeight = 28;
    static constexpr int kCompactHeight = 46;
    static constexpr int kLabelHeight = 18; // the strip over a track's voices, with its name and gear
    static constexpr int kAddRow = 28;      // the room under the last track for the button that adds one
    static constexpr int kMaxLaneHeight = 80;
    static constexpr double kMinSamplesPerPixel = 50, kMaxSamplesPerPixel = 10000; // time zoom, in and out

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
        int nameEnd = 0;        // the names' shared column ends here
        int knobX = 0;          // the knobs' column, right after the widest name line
        int headerWidth = 200;  // up to the knobs' column and the dB under them
        std::vector<int> labelsBefore; // per track: label strips above it
        int labels = 0;                // label strips in all
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
    int headerRow(int track, float y) const; // the row: a track's only row, or its k-th voice's; -1 over a label strip
    int rowTop(int track, int row) const;    // a track's row, or with voices the k-th voice's row under the label strip
    bool labelled(int track) const;          // has voices: a label strip, then a row per voice
    int contentHeight() const;               // every track's rows and label strips
    juce::Rectangle<int> label(int track) const; // the name in a label strip
    int trackHeight(int track) const;
    // Header rows: a track's only row, 0, or with voices the k-th voice's.
    juce::Rectangle<int> nameBounds(int track, int row = 0) const;
    juce::Rectangle<int> pill(int track, int which, int row = 0) const; // 0 mute, 1 solo, after the name
    juce::Rectangle<int> gear(int track) const;            // an FM track's patch button, or a drum track's sample button, after solo
    juce::Rectangle<int> addVoiceButton(int track) const;  // after the gear in a label strip; empty with every voice taken
    juce::Rectangle<int> addTrackButton() const;           // under the last track
    juce::Rectangle<int> removeTrackButton(int track) const;        // after the plus in a label strip; else after solo, or the gear
    juce::Rectangle<int> removeVoiceButton(int track, int row) const; // after a voice's solo; empty for a track's only voice
    static void paintMeter(juce::Graphics&, juce::Rectangle<float> groove, float level, float hold, bool silent);
    juce::Rectangle<int> meterBounds(int track, int row = 0) const; // the level meter under the name
    juce::Rectangle<int> knobBounds(int track, int row = 0) const;  // the volume knob, right of the meter
    bool compact() const { return laneHeight_ < kCompactHeight; }
    bool minimal() const { return laneHeight_ < kMinimalHeight; }
    int pillWidth() const { return minimal() ? 18 : 23; } // mute and solo
    static float knobPosition(float volume);                // dB to 0..1 round the knob
    static float knobVolume(float position);                // and back
    float rowVolume(int track, int row) const; // the track's volume, or with voices the row's voice's
    void setVolume(int track, int row, float volume);

    void paintRuler(juce::Graphics&);
    void paintLanes(juce::Graphics&);
    void paintHeaders(juce::Graphics&);
    void paintNote(juce::Graphics&, int track, int note, juce::Rectangle<float> r);
    // A note's expression, worked out once for each version of the tune.
    const NoteExpression& expressionOf(int track, const TuneNote& n) const;
    // Inside a note's block: shade darker where it is quieter, and a line for its pitch.
    void paintExpression(juce::Graphics&, const NoteExpression& e, juce::Rectangle<float> r, juce::Colour line,
                         float shadeAlpha) const;
    bool isSounding(const TuneNote& n) const; // under the playhead while playing

    // Editing.
    enum class Drag { None, Move, Resize, Box, Ruler, Scroll, Knob, Loop, LoopStart, LoopEnd };
    void selectionSpan(int64_t& first, int64_t& last) const; // the selected notes' first start and last end
    int loopEdgeAt(juce::Point<float> p) const; // -1 over the loop's start handle on the ruler, 1 over its end, else 0
    // A move's vertical part: the row the pointer is over, of the dragged
    // note's track or of another FM track; none over its own row.
    void updateDrop(juce::Point<float> p);
    // Where a selected note lands as the selection moves, from its track and
    // row: another row of its track, or with the dragged note into another track.
    std::pair<int, int> landing(int track, int row) const;
    void removeSelection(const juce::String& name); // the selected notes deleted, as one step of undo by that name
    // The note's properties in a callout: pitch, fine tune, velocity and pan,
    // for every selected note when it is one of them.
    void editNote(NoteRef ref);
    void preview(NoteRef ref); // plays the note with its track's instrument, with the song stopped (see AppModel::previewNote)
    // Plays a note of the track as it would sound with these values: an FM
    // note with the instrument, a drum hit with the sample.
    void preview(NoteRef ref, const TuneNote& note);
    void previewSelection(); // the first selected FM note, else the first drum hit
    bool fmTrack(int track) const; // an FM track of the tune, not a drum track
    // A note on a track's header row at a time: a copy of the nearest note
    // on that voice, else a written note (see TuneNote); on a drum track
    // with no hits, a written hit of its sample.
    void addNoteAt(int track, int row, double time);
    // Asks which track to add: an FM track with one voice, on the default
    // instrument, or a drum track of one of the rip's samples, with no hits.
    void addTrack();
    void addFmTrack();
    void addDrumTrack(int sample);
    void addVoice(int track); // the next voice for an FM track
    void removeTrack(int track);
    void removeVoice(int track, int row); // and the notes on it
    // Asks before doing something to the tracks: the question, what it does, and the button that does it.
    void confirm(const juce::String& title, const juce::String& message, const juce::String& button, std::function<void()> then);
    bool refusedWhilePlaying(); // while playing, says why the song cannot change, as the model does, and returns true
    int freeVoice(int track) const; // the lowest chip channel no track has, else the lowest the track has not; -1 for none

    AppModel& model_;
    mutable Layout layout_;
    mutable int layoutRevision_ = -1;
    double viewStart_ = 0;
    double samplesPerPixel_ = 120;
    int scrollY_ = 0;
    int laneHeight_ = 54;
    double laneHeightExact_ = 54; // the zoom, unrounded, so small pinch steps add up
    double snap_ = 0;
    double lastActivity_ = 0; // milliseconds, when wake() was last called
    bool keepPlayhead_ = false;
    bool showExpression_ = true;
    mutable std::map<int, NoteExpression> expressions_; // by note id
    mutable int expressionsRevision_ = -1;

    std::set<NoteRef> selection_;
    Drag drag_ = Drag::None;
    juce::Point<float> dragFrom_;
    NoteRef dragNote_;
    int dragTrack_ = -1;                   // the track whose knob is turned
    int dragRow_ = 0;                      // and the row: the track's, or with voices a voice's
    float dragKnob_ = 0;                   // the knob's position when the turn began
    bool dragStarted_ = false;            // the knob turn has made its undo step
    double dragDelta_ = 0;                 // move or resize amount, in samples, while dragging
    int fromRow_ = 0;                      // the dragged note's row when the move began
    int dropTrack_ = -1, dropRow_ = -1;    // the row the pointer is over while moving, -1 for the notes' own
    int dropShift_ = 0;                    // rows each selected note moves within its track, kept inside the tracks
    double loopAnchor_ = 0;                // where a Shift-drag on the ruler began, on the snap grid
    juce::Rectangle<float> box_;
    NoteRef hover_;

    // The clipboard: each note with its start from the earliest's, its
    // track, and its row when copied.
    struct ClipNote
    {
        int track = -1, row = 0;
        TuneTrack::Kind kind = TuneTrack::Kind::Fm;
        std::string name; // the track's, to find it again if tracks moved
        TuneNote note;
    };
    std::vector<ClipNote> clipboard_;
    std::weak_ptr<const VgmSong> clipSong_; // the song the notes were copied from
    int pasteTrack_ = -1, pasteVoice_ = -1; // the FM row clicked since the copy, by its chip channel; -1 for none
    int pressTrack_ = -1, pressVoice_ = -1; // the FM row an empty spot was pressed on, the paste's if the press is a click
    float wheelPitch_ = 0; // the wheel's travel toward the next semitone over a selected note

    // Meters: each chip channel belongs to the track of the note last started
    // on it, so a release still counts for its track.
    float trackLevel(int track, int row = -1) const; // loudest owned channel, linear, 1 at full scale; or of one voice row
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
    std::vector<std::vector<float>> voiceLevel_, voiceHold_; // per track, per voice row
    std::vector<std::vector<double>> voiceHoldAge_;
    std::vector<double> holdAge_;     // seconds since the hold was set
    bool hoverEdge_ = false;
};

} // namespace opm86
