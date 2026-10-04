#include "TrackerView.h"

#include "Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace opm86
{

namespace
{

constexpr float kEdge = 6.0f;      // pixels at a note's right end that resize it
constexpr double kMinLength = 441; // 10 ms, the shortest note

// A small editor for a note's pitch, shown in a callout.
class PitchEditor : public juce::Component
{
public:
    PitchEditor(const juce::String& current, std::function<void(int)> apply) : apply_(std::move(apply))
    {
        label_.setText("PITCH", juce::dontSendNotification);
        label_.setFont(theme::pixel(8.0f));
        label_.setColour(juce::Label::textColourId, theme::orange);
        addAndMakeVisible(label_);
        editor_.setFont(theme::mono(18.0f, true));
        editor_.setText(current, false);
        editor_.setJustification(juce::Justification::centred);
        editor_.setColour(juce::TextEditor::backgroundColourId, theme::background);
        editor_.setColour(juce::TextEditor::textColourId, theme::text);
        editor_.setColour(juce::TextEditor::outlineColourId, theme::gridBar);
        editor_.setColour(juce::TextEditor::focusedOutlineColourId, theme::cyan);
        editor_.onReturnKey = [this] { commit(); };
        addAndMakeVisible(editor_);
        setSize(150, 70);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        label_.setBounds(area.removeFromTop(16));
        editor_.setBounds(area.removeFromTop(32));
    }

    void visibilityChanged() override
    {
        if (isShowing())
        {
            editor_.grabKeyboardFocus();
            editor_.selectAll();
        }
    }

private:
    // Accepts "C#5", "c-4", "Bb3", "60".
    void commit()
    {
        const juce::String t = editor_.getText().trim().toUpperCase().removeCharacters("-");
        int note = -1;
        if (t.containsOnly("0123456789"))
            note = t.getIntValue();
        else if (t.isNotEmpty())
        {
            static const int kBase[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A..G
            const juce::juce_wchar letter = t[0];
            if (letter >= 'A' && letter <= 'G')
            {
                int pc = kBase[letter - 'A'];
                int i = 1;
                if (t[i] == '#')
                    ++pc, ++i;
                else if (t[i] == 'B')
                    --pc, ++i;
                const juce::String octave = t.substring(i);
                if (octave.isNotEmpty() && octave.trimCharactersAtStart("-").containsOnly("0123456789"))
                    note = (octave.getIntValue() + 1) * 12 + pc;
            }
        }
        if (note >= 0 && note <= 127)
        {
            apply_(note);
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }
        else
            editor_.setColour(juce::TextEditor::outlineColourId, theme::danger);
    }

    juce::Label label_;
    juce::TextEditor editor_;
    std::function<void(int)> apply_;
};

} // namespace

TrackerView::TrackerView(AppModel& model) : model_(model)
{
    model_.addChangeListener(this);
    setWantsKeyboardFocus(true);
}

TrackerView::~TrackerView()
{
    model_.removeChangeListener(this);
}

void TrackerView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Drop selected notes that no longer exist.
    for (auto it = selection_.begin(); it != selection_.end();)
        it = indexOf(*it) < 0 ? selection_.erase(it) : std::next(it);
    repaint();
}

const TrackerView::Layout& TrackerView::layout() const
{
    if (layoutRevision_ == model_.revision())
        return layout_;
    layoutRevision_ = model_.revision();
    layout_ = {};
    if (!model_.hasTune())
        return layout_;
    const auto& tracks = model_.tune().tracks;
    for (const TuneTrack& t : tracks)
    {
        // Notes that sound together take a row each: the lowest row free at
        // their start, a chord's notes from the highest down. Drum hits share
        // one row: a hit's tail under the next is no chord.
        std::vector<size_t> order(t.notes.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        if (t.kind == TuneTrack::Kind::Fm)
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const TuneNote &x = t.notes[a], &y = t.notes[b];
                return x.start != y.start ? x.start < y.start : model_.tune().pitch(t, x) > model_.tune().pitch(t, y);
            });
        std::vector<int64_t> rowEnd;
        std::vector<int> rows(t.notes.size(), 0);
        int64_t longest = 0;
        std::map<int, int> index;
        for (const size_t i : order)
        {
            const TuneNote& n = t.notes[i];
            size_t r = 0;
            while (t.kind == TuneTrack::Kind::Fm && r < rowEnd.size() && rowEnd[r] > n.start)
                ++r;
            if (r == rowEnd.size())
                rowEnd.push_back(0);
            rowEnd[r] = n.start + n.length;
            rows[i] = static_cast<int>(r);
            longest = std::max(longest, n.length);
            index[n.id] = static_cast<int>(i);
        }

        // A part the game keeps on fixed chip voices, no more of them than it
        // plays at once, gets a row per voice instead, each with its mute. A
        // part whose notes take whichever voice is free keeps the rows above.
        std::vector<int> voices = model_.tune().voices(t);
        const bool fixedVoices = voices.size() > 1 && voices.size() <= rowEnd.size();
        if (fixedVoices)
        {
            std::array<int, 8> voiceRow {};
            for (size_t k = 0; k < voices.size(); ++k)
                voiceRow[static_cast<size_t>(voices[k])] = static_cast<int>(k);
            for (size_t i = 0; i < t.notes.size(); ++i)
                rows[i] = voiceRow[static_cast<size_t>(model_.tune().originalChannel(t, t.notes[i]))];
        }
        else
            voices.clear();
        layout_.row.push_back(rows);
        layout_.rows.push_back(std::max<int>(1, static_cast<int>(fixedVoices ? voices.size() : rowEnd.size())));
        layout_.voices.push_back(std::move(voices));
        const bool collapsed = layout_.rows.back() > 1 && collapsed_.count(static_cast<int>(layout_.shown.size())) > 0;
        layout_.shown.push_back(collapsed ? 1 : layout_.rows.back());
        layout_.firstRow.push_back(layout_.totalRows);
        layout_.totalRows += layout_.shown.back();
        layout_.longest.push_back(longest);
        layout_.index.push_back(std::move(index));
    }
    return layout_;
}

int TrackerView::indexOf(NoteRef ref) const
{
    if (ref.track < 0 || ref.track >= static_cast<int>(layout().index.size()))
        return -1;
    const auto& index = layout().index[static_cast<size_t>(ref.track)];
    const auto it = index.find(ref.id);
    return it == index.end() ? -1 : it->second;
}

double TrackerView::visibleSamples() const
{
    return (getWidth() - kHeaderWidth) * samplesPerPixel_;
}

void TrackerView::setViewStart(double songTime)
{
    const double length = model_.hasTune() ? model_.tune().source().length + 4 * kVgmRate : 0;
    viewStart_ = std::clamp(songTime, -kVgmRate, std::max(0.0, length - visibleSamples() * 0.5));
    repaint();
    sendChangeMessage();
}

void TrackerView::zoomAround(double factor, float x)
{
    const double anchor = xToTime(x);
    samplesPerPixel_ = std::clamp(samplesPerPixel_ / factor, 4.0, 20000.0);
    setViewStart(anchor - (x - kHeaderWidth) * samplesPerPixel_);
}

void TrackerView::followPlayhead()
{
    if (!model_.playing())
        return;
    // The playhead runs to the middle, then the song moves under it.
    const double p = model_.displayPosition();
    if (p > viewStart_ + visibleSamples() * 0.5)
        setViewStart(p - visibleSamples() * 0.5);
    else if (p < viewStart_)
        setViewStart(p - visibleSamples() * 0.1);
}

double TrackerView::xToTime(float x) const
{
    return viewStart_ + (x - kHeaderWidth) * samplesPerPixel_;
}

float TrackerView::timeToX(double t) const
{
    return static_cast<float>(kHeaderWidth + (t - viewStart_) / samplesPerPixel_);
}

int TrackerView::yToTrack(float y) const
{
    if (y < kRulerHeight || !model_.hasTune())
        return -1;
    const int row = static_cast<int>((y - static_cast<float>(kRulerHeight - scrollY_)) / static_cast<float>(laneHeight_));
    const auto& first = layout().firstRow;
    if (row >= layout().totalRows)
        return -1;
    return static_cast<int>(std::upper_bound(first.begin(), first.end(), row) - first.begin()) - 1;
}

int TrackerView::trackTop(int track) const
{
    return kRulerHeight + layout().firstRow[static_cast<size_t>(track)] * laneHeight_ - scrollY_;
}

int TrackerView::trackHeight(int track) const
{
    return layout().shown[static_cast<size_t>(track)] * laneHeight_;
}

void TrackerView::setCollapsed(int track, bool collapsed)
{
    if (collapsed)
        collapsed_.insert(track);
    else
        collapsed_.erase(track);
    layoutRevision_ = -1;
    setScrollY(scrollY_);
}

bool TrackerView::expanded(int track) const
{
    return layout().shown[static_cast<size_t>(track)] > 1;
}

juce::Rectangle<int> TrackerView::disclosure(int track) const
{
    return { 10, nameBounds(track).getY(), 14, nameBounds(track).getHeight() };
}

double TrackerView::snapTime(double t) const
{
    if (snap_ <= 0 || !model_.hasTune())
        return t;
    const double origin = model_.tune().source().firstBeat;
    return origin + std::round((t - origin) / snap_) * snap_;
}

juce::Rectangle<float> TrackerView::noteBounds(int track, int note) const
{
    const TuneNote& n = model_.tune().tracks[static_cast<size_t>(track)].notes[static_cast<size_t>(note)];
    const int row = layout().row[static_cast<size_t>(track)][static_cast<size_t>(note)];
    float h = static_cast<float>(laneHeight_) - 6.0f;
    float y = static_cast<float>(trackTop(track) + row * laneHeight_) + 3.0f;
    if (!expanded(track) && layout().rows[static_cast<size_t>(track)] > 1)
    {
        // Collapsed: the voices stacked thin in one lane.
        const float slot = h / static_cast<float>(layout().rows[static_cast<size_t>(track)]);
        y = static_cast<float>(trackTop(track)) + 3.0f + slot * static_cast<float>(row);
        h = std::max(2.0f, slot - 1.0f);
    }
    const float x0 = timeToX(static_cast<double>(n.start));
    const float x1 = timeToX(static_cast<double>(n.start + n.length));
    return { x0, y, std::max(2.0f, x1 - x0), h };
}

TrackerView::NoteRef TrackerView::noteAt(juce::Point<float> p, bool& onRightEdge) const
{
    onRightEdge = false;
    const int t = yToTrack(p.y);
    if (t < 0 || p.x < kHeaderWidth)
        return {};
    const auto& notes = model_.tune().tracks[static_cast<size_t>(t)].notes;
    const double time = xToTime(p.x);
    // The note under the pointer that starts last, so the shorter one on top wins.
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
    {
        const TuneNote& n = notes[static_cast<size_t>(i)];
        if (static_cast<double>(n.start) > time + kEdge * samplesPerPixel_)
            continue;
        if (static_cast<double>(n.start + n.length + layout().longest[static_cast<size_t>(t)]) < time)
            break;
        const auto r = noteBounds(t, i);
        if (r.expanded(1.0f, 0.0f).contains(p) || (p.x >= r.getRight() - kEdge && p.x <= r.getRight() + 2 && r.getY() <= p.y && p.y <= r.getBottom()))
        {
            onRightEdge = p.x >= r.getRight() - kEdge;
            return { t, n.id };
        }
    }
    return {};
}

void TrackerView::updateMeters(double seconds)
{
    if (!model_.hasTune())
        return;
    const auto& tracks = model_.tune().tracks;
    // Who owns each chip channel, from the newest build's channel choices.
    if (ownersRevision_ != model_.reportRevision())
    {
        ownersRevision_ = model_.reportRevision();
        for (auto& o : owners_)
            o.clear();
        const auto& channels = model_.report().channels;
        for (size_t t = 0; t < tracks.size() && t < channels.size(); ++t)
        {
            const bool drum = tracks[t].kind == TuneTrack::Kind::Drum;
            const bool chord = t < layout().voices.size() && !layout().voices[t].empty();
            for (size_t i = 0; i < tracks[t].notes.size() && i < channels[t].size(); ++i)
            {
                int ch = channels[t][i];
                if (ch < 0)
                    ch = model_.tune().originalChannel(tracks[t], tracks[t].notes[i]);
                const size_t slot = static_cast<size_t>(ch + (drum ? 8 : 0));
                if (slot < owners_.size())
                    owners_[slot].push_back({ tracks[t].notes[i].start, static_cast<int>(t), chord ? layout().row[t][i] : 0 });
            }
        }
        for (auto& o : owners_)
            std::stable_sort(o.begin(), o.end());
    }

    // Peak-meter ballistics, in dB: a hit shows at once and the level falls
    // back at kMeterRelease; the peak hold waits a second, then falls too.
    const auto peaks = model_.takePeaks();
    for (size_t c = 0; c < peaks.size(); ++c)
        channelLevel_[c] = model_.playing() ? peaks[c] : 0.0f;
    level_.resize(tracks.size(), kMeterFloor);
    hold_.resize(tracks.size(), kMeterFloor);
    holdAge_.resize(tracks.size(), 0.0);
    const float fall = static_cast<float>(seconds) * kMeterRelease;
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const float db = juce::Decibels::gainToDecibels(trackLevel(static_cast<int>(t)), kMeterFloor);
        level_[t] = std::max(db, level_[t] - fall);
        if (level_[t] >= hold_[t])
            hold_[t] = level_[t], holdAge_[t] = 0.0;
        else if ((holdAge_[t] += seconds) > 1.0)
            hold_[t] = std::max(level_[t], hold_[t] - fall * 0.5f);
    }
}

float TrackerView::trackLevel(int track) const
{
    const double now = model_.position();
    float loudest = 0.0f;
    for (size_t c = 0; c < owners_.size(); ++c)
    {
        const auto& o = owners_[c];
        const auto it = std::upper_bound(o.begin(), o.end(), Owner { static_cast<int64_t>(now), 0, 0 });
        if (it != o.begin() && std::prev(it)->track == track)
            loudest = std::max(loudest, channelLevel_[c]);
    }
    return loudest;
}

void TrackerView::resized()
{
    setViewStart(viewStart_);
    setScrollY(scrollY_);
}

void TrackerView::setScrollY(int y)
{
    const int rows = model_.hasTune() ? layout().totalRows : 0;
    scrollY_ = std::clamp(y, 0, std::max(0, rows * laneHeight_ - (getHeight() - kRulerHeight)));
    repaint();
}

void TrackerView::zoomLanes(double factor, float y)
{
    // The point under the pointer stays where it is.
    const double anchor = std::max(0.0f, y - static_cast<float>(kRulerHeight));
    const double content = (anchor + scrollY_) / laneHeight_;
    laneHeightExact_ = std::clamp(laneHeightExact_ * factor, static_cast<double>(kMinLaneHeight), static_cast<double>(kMaxLaneHeight));
    laneHeight_ = static_cast<int>(std::lround(laneHeightExact_));
    setScrollY(static_cast<int>(std::lround(content * laneHeight_ - anchor)));
}

// Header layout. In a track's first row: the name with mute and solo on its
// right, and under them the level meter; right of both, the volume knob. At
// the right edge, a chord part's voice toggles, one beside each row.
namespace
{
constexpr int kVoiceColumn = 44; // width of the voice toggles' column
constexpr int kKnobColumn = 40;  // width of the volume knob's column
constexpr int kBlock = 44;       // height of the name and meter block
constexpr float kKnobArc = juce::MathConstants<float>::pi * 0.75f; // the knob turns 135 degrees each way
}

juce::Rectangle<int> TrackerView::voicePill(int track, int k) const
{
    return { kHeaderWidth - kVoiceColumn + 4, trackTop(track) + k * laneHeight_ + (laneHeight_ - 18) / 2, 30, 18 };
}

juce::Rectangle<int> TrackerView::nameBounds(int track) const
{
    // After the arrow of a chord part; as wide as the name, up to a limit.
    const int left = layout().rows[static_cast<size_t>(track)] > 1 ? 28 : 14;
    const juce::String name = juce::String(model_.tune().tracks[static_cast<size_t>(track)].name).toUpperCase();
    const int width = std::min(juce::GlyphArrangement::getStringWidthInt(theme::mono(13.0f, true), name) + 2, 96);
    return { left, trackTop(track) + (laneHeight_ - kBlock) / 2, width, 17 };
}

juce::Rectangle<int> TrackerView::pill(int track, int which) const
{
    const auto name = nameBounds(track);
    return { name.getRight() + 8 + which * 26, name.getY(), 23, 17 };
}

juce::Rectangle<int> TrackerView::knobBounds(int track) const
{
    // Beside the block, before the voice column when a chord part shows its toggles.
    const int top = trackTop(track) + (laneHeight_ - kBlock) / 2;
    const bool toggles = !layout().voices[static_cast<size_t>(track)].empty() && expanded(track);
    const int right = kHeaderWidth - (toggles ? kVoiceColumn : 0) - 4;
    return { right - kKnobColumn + 4, top + (kBlock - 34) / 2, 34, 34 };
}

juce::Rectangle<int> TrackerView::meterBounds(int track) const
{
    // The width up to the knob.
    const int top = trackTop(track) + (laneHeight_ - kBlock) / 2;
    return { 14, top + 25, knobBounds(track).getX() - 8 - 14, 16 };
}

// The knob law: amplitude goes with the cube of the position, so full right
// is +6 dB, 0 dB sits at four fifths, -12 dB near the middle, and the first
// twelfth is silence.
float TrackerView::knobPosition(float volume)
{
    if (volume <= TuneTrack::kSilent)
        return 0.0f;
    return std::pow(10.0f, (std::min(volume, TuneTrack::kMaxVolume) - TuneTrack::kMaxVolume) / 60.0f);
}

float TrackerView::knobVolume(float position)
{
    if (position <= knobPosition(TuneTrack::kSilent + 0.01f))
        return TuneTrack::kSilent;
    return std::min(TuneTrack::kMaxVolume, TuneTrack::kMaxVolume + 60.0f * std::log10(position));
}

// Painting --------------------------------------------------------------------

void TrackerView::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    if (!model_.hasTune())
    {
        g.setColour(theme::dim);
        g.setFont(theme::pixel(12.0f));
        g.drawText("OPEN A VGM RIP TO START", getLocalBounds(), juce::Justification::centred);
        g.setFont(theme::mono(14.0f));
        g.drawText("File > Open, or drop a .vgz file here", getLocalBounds().withTrimmedTop(60), juce::Justification::centred);
        return;
    }
    paintLanes(g);
    paintRuler(g);
    paintHeaders(g);

    // The playhead, over lanes and ruler, with a soft glow.
    const float x = timeToX(model_.displayPosition());
    if (x >= kHeaderWidth && x <= static_cast<float>(getWidth()))
    {
        for (int w = 6; w >= 2; w -= 2)
        {
            g.setColour(theme::cyan.withAlpha(0.07f));
            g.fillRect(x - static_cast<float>(w) / 2.0f, 0.0f, static_cast<float>(w), static_cast<float>(getHeight()));
        }
        g.setColour(theme::cyan);
        g.fillRect(x - 0.75f, 0.0f, 1.5f, static_cast<float>(getHeight()));
        juce::Path head;
        head.addTriangle(x - 6, 0, x + 6, 0, x, 9);
        g.fillPath(head);
    }
}

void TrackerView::paintRuler(juce::Graphics& g)
{
    const auto area = juce::Rectangle<int>(kHeaderWidth, 0, getWidth() - kHeaderWidth, kRulerHeight);
    g.setColour(theme::panel);
    g.fillRect(area);
    g.setColour(theme::gridBar);
    g.fillRect(area.getX(), area.getBottom() - 1, area.getWidth(), 1);

    const VgmSong& song = model_.tune().source();
    const double beat = song.beatLength;
    const double beatPixels = beat / samplesPerPixel_;
    // Label every bar, or every 2nd, 4th... when bars are close together.
    int every = 1;
    while (beatPixels * 4 * every < 44)
        every *= 2;
    const long first = static_cast<long>(std::floor((viewStart_ - song.firstBeat) / beat)) - 1;
    const long last = static_cast<long>(std::ceil((xToTime(static_cast<float>(getWidth())) - song.firstBeat) / beat));
    g.setFont(theme::pixel(10.0f));
    for (long k = first; k <= last; ++k)
    {
        const float x = timeToX(song.firstBeat + static_cast<double>(k) * beat);
        if (x < kHeaderWidth)
            continue;
        const bool bar = ((k % 4) + 4) % 4 == 0;
        const long barNumber = (k >= 0 ? k / 4 : (k - 3) / 4) + 1;
        if (bar && ((barNumber - 1) % every + every) % every == 0)
        {
            g.setColour(theme::faint);
            g.fillRect(x, 6.0f, 1.0f, kRulerHeight - 6.0f);
            g.setColour(theme::orange);
            g.drawText(juce::String(barNumber), juce::Rectangle<float>(x + 4, 4, 70, 16),
                       juce::Justification::centredLeft);
        }
        else if (beatPixels > 10)
        {
            g.setColour(theme::faint);
            g.fillRect(x, static_cast<float>(kRulerHeight - 7), 1.0f, 6.0f);
        }
    }
}

void TrackerView::paintLanes(juce::Graphics& g)
{
    const auto& tracks = model_.tune().tracks;
    const VgmSong& song = model_.tune().source();
    const double beat = song.beatLength;
    const double beatPixels = beat / samplesPerPixel_;
    const auto lanes = juce::Rectangle<int>(kHeaderWidth, kRulerHeight, getWidth() - kHeaderWidth, getHeight() - kRulerHeight);
    g.saveState();
    g.reduceClipRegion(lanes);

    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const int y = trackTop(static_cast<int>(t)), h = trackHeight(static_cast<int>(t));
        if (y > getHeight() || y + h < kRulerHeight)
            continue;
        g.setColour(t % 2 ? theme::background : theme::background.brighter(0.04f));
        g.fillRect(kHeaderWidth, y, getWidth() - kHeaderWidth, h);
        // Faint lines between a chord part's voices.
        g.setColour(theme::grid.withAlpha(0.3f));
        for (size_t k = 1; expanded(static_cast<int>(t)) && k < layout().voices[t].size(); ++k)
            g.fillRect(kHeaderWidth, y + static_cast<int>(k) * laneHeight_, getWidth() - kHeaderWidth, 1);
        g.setColour(theme::grid.withAlpha(0.6f));
        g.fillRect(kHeaderWidth, y + h - 1, getWidth() - kHeaderWidth, 1);
    }

    // Grid: bars bright, beats dim, sixteenths when there is room.
    const long first = static_cast<long>(std::floor((viewStart_ - song.firstBeat) / beat)) - 1;
    const long last = static_cast<long>(std::ceil((xToTime(static_cast<float>(getWidth())) - song.firstBeat) / beat));
    for (long k = first; k <= last; ++k)
    {
        const double t0 = song.firstBeat + static_cast<double>(k) * beat;
        const bool bar = ((k % 4) + 4) % 4 == 0;
        if (bar || beatPixels > 10)
        {
            g.setColour(bar ? theme::gridBar : theme::grid);
            g.fillRect(timeToX(t0), static_cast<float>(kRulerHeight), 1.0f, static_cast<float>(getHeight()));
        }
        if (beatPixels > 64)
            for (int s = 1; s < 4; ++s)
            {
                g.setColour(theme::grid.withAlpha(0.45f));
                g.fillRect(timeToX(t0 + s * beat / 4), static_cast<float>(kRulerHeight), 1.0f, static_cast<float>(getHeight()));
            }
    }

    // Notes in view.
    const double from = viewStart_, to = xToTime(static_cast<float>(getWidth()));
    std::vector<std::tuple<int, int, juce::Rectangle<float>>> sounding;
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const int y = trackTop(static_cast<int>(t));
        if (y > getHeight() || y + trackHeight(static_cast<int>(t)) < kRulerHeight)
            continue;
        const auto& notes = tracks[t].notes;
        const double earliest = from - static_cast<double>(layout().longest[t]);
        auto it = std::lower_bound(notes.begin(), notes.end(), earliest,
                                   [](const TuneNote& n, double time) { return static_cast<double>(n.start) < time; });
        for (; it != notes.end() && static_cast<double>(it->start) <= to; ++it)
        {
            if (static_cast<double>(it->start + it->length) < from)
                continue;
            const int i = static_cast<int>(it - notes.begin());
            auto r = noteBounds(static_cast<int>(t), i);
            // While dragging, selected notes show where they will land.
            if ((drag_ == Drag::Move || drag_ == Drag::Resize) && selection_.count({ static_cast<int>(t), it->id }))
            {
                const float dx = static_cast<float>(dragDelta_ / samplesPerPixel_);
                if (drag_ == Drag::Move)
                    r.translate(dx, 0);
                else
                    r.setWidth(std::max(2.0f, r.getWidth() + dx));
            }
            if (isSounding(*it))
                sounding.push_back({ static_cast<int>(t), i, r });
            else
                paintNote(g, static_cast<int>(t), i, r);
        }
    }
    // The notes sounding now go last, so their glow lies over their neighbours.
    for (const auto& [t, i, r] : sounding)
        paintNote(g, t, i, r);

    if (drag_ == Drag::Box)
    {
        g.setColour(theme::cyan.withAlpha(0.12f));
        g.fillRect(box_);
        g.setColour(theme::cyan.withAlpha(0.8f));
        g.drawRect(box_, 1.0f);
    }
    g.restoreState();
}

bool TrackerView::isSounding(const TuneNote& n) const
{
    const double pos = model_.displayPosition();
    return model_.playing() && pos >= static_cast<double>(n.start) && pos < static_cast<double>(n.start + n.length);
}

void TrackerView::paintNote(juce::Graphics& g, int track, int note, juce::Rectangle<float> r)
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track)];
    const TuneNote& n = tr.notes[static_cast<size_t>(note)];
    const bool drum = tr.kind == TuneTrack::Kind::Drum;
    const int pitch = model_.tune().pitch(tr, n);
    const bool audible = model_.tune().audible(tr, n);
    const bool selected = selection_.count({ track, n.id }) > 0;
    const bool sounding = isSounding(n);
    const auto& channels = model_.report().channels;
    const bool conflict = static_cast<size_t>(track) < channels.size()
                          && static_cast<size_t>(note) < channels[static_cast<size_t>(track)].size()
                          && channels[static_cast<size_t>(track)][static_cast<size_t>(note)] < 0;

    // Three states far apart: silent notes nearly grey and faint, the rest
    // calm, and the ones sounding now at full colour with a glow and a white rim.
    const juce::Colour base = drum ? theme::instrumentColour(track).withMultipliedSaturation(0.8f) : theme::pitchColour(pitch);
    juce::Colour fill = !audible ? base.withMultipliedSaturation(0.15f).withMultipliedBrightness(0.6f).withAlpha(0.35f)
                        : sounding ? base.brighter(0.08f)
                                   : base.withMultipliedSaturation(0.7f).withMultipliedBrightness(0.5f);
    if (sounding && audible)
    {
        for (const auto [grow, alpha] : { std::pair { 7.0f, 0.06f }, std::pair { 4.0f, 0.12f }, std::pair { 2.0f, 0.25f } })
        {
            g.setColour(base.withAlpha(alpha));
            g.fillRoundedRectangle(r.expanded(grow), 3.0f + grow);
        }
    }
    // A small gap and a dark outline keep touching notes apart.
    if (r.getWidth() > 3.0f)
        r = r.withTrimmedRight(1.0f);
    g.setColour(fill);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(juce::Colours::white.withAlpha(audible ? (sounding ? 0.5f : 0.18f) : 0.05f));
    g.fillRect(r.withHeight(1.0f).reduced(2.0f, 0.0f));
    if (sounding && audible)
    {
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.drawRoundedRectangle(r.reduced(0.25f), 3.0f, 1.5f);
    }
    else
    {
        g.setColour(theme::background.withAlpha(audible ? 0.9f : 0.5f));
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
    }

    if (conflict)
    {
        g.setColour(theme::danger);
        g.drawRoundedRectangle(r, 3.0f, 2.0f);
    }
    else if (selected)
    {
        g.setColour(juce::Colours::white);
        g.drawRoundedRectangle(r.expanded(0.5f), 3.0f, 1.5f);
    }

    // The label, centred and as large as the note allows: white with a dark
    // outline, so it reads on every colour. Notes too small for it go without.
    const juce::String label = drum ? (n.transpose == 0 ? juce::String() : (n.transpose > 0 ? "+" : "") + juce::String(n.transpose))
                                    : theme::noteName(pitch).removeCharacters("-");
    const float height = std::min({ 26.0f, r.getHeight() * 0.62f, (r.getWidth() - 6.0f) / (0.64f * static_cast<float>(label.length())) });
    if (label.isNotEmpty() && height >= 9.0f)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText(theme::mono(height, true), label, r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                             juce::Justification::centred, 1, 1.0f);
        juce::Path text;
        glyphs.createPath(text);
        if (audible)
        {
            g.setColour(theme::background.withAlpha(0.7f));
            g.strokePath(text, juce::PathStrokeType(std::max(2.0f, height * 0.16f), juce::PathStrokeType::curved));
        }
        g.setColour(juce::Colours::white.withAlpha(audible ? (sounding ? 1.0f : 0.7f) : 0.3f));
        g.fillPath(text);
    }
}

void TrackerView::paintHeaders(juce::Graphics& g)
{
    const auto& tracks = model_.tune().tracks;
    g.setColour(theme::panel);
    g.fillRect(0, 0, kHeaderWidth, getHeight());
    g.setColour(theme::gridBar);
    g.fillRect(kHeaderWidth - 1, 0, 1, getHeight());

    g.saveState();
    g.reduceClipRegion(0, kRulerHeight, kHeaderWidth, getHeight() - kRulerHeight);
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const TuneTrack& tr = tracks[t];
        const int y = trackTop(static_cast<int>(t));
        if (y > getHeight() || y + trackHeight(static_cast<int>(t)) < kRulerHeight)
            continue;
        const auto row = juce::Rectangle<int>(0, y, kHeaderWidth - 1, trackHeight(static_cast<int>(t)));
        g.setColour(t % 2 ? theme::panel : theme::panelLight);
        g.fillRect(row);
        g.setColour(theme::instrumentColour(static_cast<int>(t)));
        g.fillRect(row.getX(), row.getY() + 3, 4, row.getHeight() - 6);

        const int ti = static_cast<int>(t);
        const auto& voices = layout().voices[t];
        auto drawToggle = [&](juce::Rectangle<int> r, const juce::String& label, bool on, juce::Colour colour) {
            g.setColour(on ? colour : theme::background.withAlpha(0.7f));
            g.fillRoundedRectangle(r.toFloat(), 4.0f);
            g.setColour(on ? colour : theme::gridBar.brighter(0.3f));
            g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 4.0f, 1.0f);
            g.setColour(on ? theme::background : theme::text.withAlpha(0.85f));
            g.setFont(theme::mono(12.0f, true));
            g.drawText(label, r, juce::Justification::centred);
        };

        // Name, with mute and solo on its right, over the meter.
        const bool audible = model_.tune().audible(tr);
        const auto mute = pill(ti, 0);
        g.setColour(audible ? theme::text : theme::faint);
        g.setFont(theme::mono(13.0f, true));
        if (layout().rows[t] > 1)
        {
            // A chord part's arrow: down when its notes have a row each.
            const auto arrow = disclosure(ti).toFloat().withSizeKeepingCentre(8.0f, 8.0f);
            juce::Path p;
            if (expanded(ti))
                p.addTriangle(arrow.getX(), arrow.getY() + 1.5f, arrow.getRight(), arrow.getY() + 1.5f, arrow.getCentreX(), arrow.getBottom() - 0.5f);
            else
                p.addTriangle(arrow.getX() + 1.5f, arrow.getY(), arrow.getX() + 1.5f, arrow.getBottom(), arrow.getRight() - 0.5f, arrow.getCentreY());
            g.setColour(theme::text.withAlpha(0.8f));
            g.fillPath(p);
        }
        g.setColour(audible ? theme::text : theme::faint);
        g.drawText(juce::String(tr.name).toUpperCase(), nameBounds(ti),
                   juce::Justification::centredLeft, true);
        drawToggle(mute, "M", tr.mute, theme::orange);
        drawToggle(pill(ti, 1), "S", tr.solo, theme::cyan);
        if (tr.kind == TuneTrack::Kind::Fm)
        {
            // Which instrument the voice plays.
            g.setColour(audible ? theme::dim : theme::faint);
            g.setFont(theme::mono(12.0f, true));
            g.drawText("FM " + juce::String(tr.instrument + 1), juce::Rectangle<int>(pill(ti, 1).getRight() + 8, mute.getY(), 50, mute.getHeight()),
                       juce::Justification::centredLeft, false);
        }

        // The level in dB, always shown at the end of the name line.
        const bool dragging = drag_ == Drag::Knob && dragTrack_ == ti;
        const bool silent = !audible || tr.volume <= TuneTrack::kSilent;
        {
            const juce::String db = tr.volume <= TuneTrack::kSilent ? juce::String("-inf dB")
                                    : (tr.volume > 0.05f ? "+" : "") + juce::String(tr.volume, 1) + " dB";
            g.setColour(dragging ? theme::cyan : silent ? theme::faint
                        : std::abs(tr.volume) < 0.05f ? theme::dim : theme::text);
            g.setFont(theme::mono(12.0f, true));
            const auto meter = meterBounds(ti);
            g.drawText(db, juce::Rectangle<int>(meter.getRight() - 80, mute.getY(), 80, mute.getHeight()), juce::Justification::centredRight);
        }

        // The meter, as DAWs draw a peak meter: a dark groove with a bar
        // along a dB scale, white for most of it, then yellow, orange and
        // red over the last few dB, and a peak-hold line.
        {
            const auto groove = meterBounds(ti).toFloat();
            g.setColour(juce::Colour(0xff10121a));
            g.fillRoundedRectangle(groove, 2.0f);
            g.setColour(silent ? juce::Colour(0xff2c2d33) : theme::grid);
            g.drawRoundedRectangle(groove, 2.0f, 1.0f);
            const auto bar = groove.reduced(2.0f, 2.0f);
            auto xAt = [&](float db) {
                const float frac = juce::jlimit(0.0f, 1.0f, (db - kMeterFloor) / (kMeterTop - kMeterFloor));
                return bar.getX() + frac * bar.getWidth();
            };
            // -12 and 0 dB marks under the bar.
            g.setColour(theme::faint.withAlpha(0.6f));
            for (const float db : { -12.0f, 0.0f })
                g.fillRect(xAt(db), bar.getY(), 1.0f, bar.getHeight());

            juce::ColourGradient zones(silent ? juce::Colour(0xff7a7b86) : juce::Colour(0xffeceaf4), bar.getX(), 0.0f,
                                       silent ? juce::Colour(0xff7a7b86) : theme::danger, bar.getRight(), 0.0f, false);
            if (!silent)
            {
                auto stop = [&](float db, juce::Colour c) { zones.addColour((xAt(db) - bar.getX()) / bar.getWidth(), c); };
                stop(-15.0f, juce::Colour(0xffeceaf4));
                stop(-9.0f, theme::yellow);
                stop(-4.0f, theme::orange);
                stop(0.0f, theme::danger);
            }
            const float level = t < level_.size() ? level_[t] : kMeterFloor;
            const float hold = t < hold_.size() ? hold_[t] : kMeterFloor;
            g.setGradientFill(zones);
            if (level > kMeterFloor)
                g.fillRect(bar.withRight(xAt(level)));
            if (hold > kMeterFloor + 1.0f)
                g.fillRect(juce::Rectangle<float>(xAt(hold) - 2.0f, bar.getY(), 2.0f, bar.getHeight()));
        }

        // The volume knob: a ring from silence to +6 dB lit up to the level,
        // a tick at 0 dB, and a raised cap with a pointer.
        {
            const auto bounds = knobBounds(ti).toFloat();
            const auto centre = bounds.getCentre();
            const float radius = bounds.getWidth() * 0.5f - 2.0f;
            const float angle = -kKnobArc + 2.0f * kKnobArc * knobPosition(tr.volume);
            const juce::Colour accent = dragging ? theme::cyan.brighter(0.3f) : silent ? juce::Colour(0xff6a6c80) : theme::cyan;

            juce::Path ring;
            ring.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, -kKnobArc, kKnobArc, true);
            g.setColour(silent ? juce::Colour(0xff2c2d33) : juce::Colour(0xff2a2f4a));
            g.strokePath(ring, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            if (tr.volume > TuneTrack::kSilent)
            {
                juce::Path value;
                value.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, -kKnobArc, angle, true);
                g.setColour(accent);
                g.strokePath(value, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            // 0 dB, as the game played it.
            const float zero = -kKnobArc + 2.0f * kKnobArc * knobPosition(0.0f);
            const auto tickIn = centre.getPointOnCircumference(radius - 2.5f, zero);
            const auto tickOut = centre.getPointOnCircumference(radius + 2.5f, zero);
            g.setColour(theme::text.withAlpha(0.8f));
            g.drawLine({ tickIn, tickOut }, 1.2f);

            const auto cap = juce::Rectangle<float>(0, 0, (radius - 4.0f) * 2.0f, (radius - 4.0f) * 2.0f).withCentre(centre);
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillEllipse(cap.translated(0.0f, 1.5f));
            const juce::Colour light = silent ? juce::Colour(0xff6a6c84) : juce::Colour(0xffc9cbe8);
            const juce::Colour dark = silent ? juce::Colour(0xff34364a) : juce::Colour(0xff4f5378);
            g.setGradientFill(juce::ColourGradient(light, cap.getX(), cap.getY(), dark, cap.getX(), cap.getBottom(), false));
            g.fillEllipse(cap);
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.drawEllipse(cap.reduced(0.5f), 1.0f);
            const auto tip = centre.getPointOnCircumference(cap.getWidth() * 0.5f - 2.5f, angle);
            const auto base = centre.getPointOnCircumference(cap.getWidth() * 0.12f, angle);
            g.setColour(dragging ? theme::cyan : theme::background);
            g.drawLine({ base, tip }, 2.2f);
        }

        if (!voices.empty() && expanded(ti))
        {
            // The voice column: a bracket joining one toggle per row, with row lines.
            const int x = kHeaderWidth - kVoiceColumn;
            g.setColour(theme::background.withAlpha(0.35f));
            g.fillRect(x, row.getY(), kHeaderWidth - 1 - x, row.getHeight());
            g.setColour(theme::grid.withAlpha(0.5f));
            for (size_t k = 1; k < voices.size(); ++k)
                g.fillRect(x, row.getY() + static_cast<int>(k) * laneHeight_, kHeaderWidth - 1 - x, 1);
            const auto top = voicePill(ti, 0), bottom = voicePill(ti, static_cast<int>(voices.size()) - 1);
            g.setColour(theme::instrumentColour(ti).withAlpha(0.45f));
            g.fillRect(x + 1, top.getCentreY(), 1, bottom.getCentreY() - top.getCentreY());
            for (size_t k = 0; k < voices.size(); ++k)
            {
                const auto r = voicePill(ti, static_cast<int>(k));
                g.setColour(theme::instrumentColour(ti).withAlpha(0.45f));
                g.fillRect(x + 1, r.getCentreY(), r.getX() - x - 1, 1);
                drawToggle(r, "V" + juce::String(voices[k] + 1), (tr.mutedVoices >> voices[k]) & 1, theme::orange);
            }
        }
    }
    g.restoreState();

    // Corner over the headers.
    g.setColour(theme::panel);
    g.fillRect(0, 0, kHeaderWidth, kRulerHeight);
    g.setColour(theme::gridBar);
    g.fillRect(0, kRulerHeight - 1, kHeaderWidth, 1);
    g.setColour(theme::magenta);
    g.setFont(theme::pixel(8.0f));
    g.drawText("TRACKS " + juce::String(tracks.size()).paddedLeft('0', 2), juce::Rectangle<int>(14, 0, kHeaderWidth, kRulerHeight),
               juce::Justification::centredLeft);
}

// Mouse -----------------------------------------------------------------------

void TrackerView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (!model_.hasTune())
        return;
    const auto p = e.position;
    dragFrom_ = p;
    dragDelta_ = 0;

    if (p.y < kRulerHeight && p.x >= kHeaderWidth)
    {
        drag_ = Drag::Ruler;
        model_.setPosition(std::max(0.0, xToTime(p.x)));
        return;
    }

    const int t = yToTrack(p.y);
    if (p.x < kHeaderWidth)
    {
        if (t < 0)
            return;
        // The knob turns as the pointer goes up or right; Option-click puts it back to 0 dB.
        if (knobBounds(t).expanded(3).contains(p.toInt()))
        {
            drag_ = Drag::Knob;
            dragTrack_ = t;
            dragStarted_ = false;
            dragKnob_ = knobPosition(model_.tune().tracks[static_cast<size_t>(t)].volume);
            if (e.mods.isAltDown())
                setVolume(t, 0.0f);
            else
                repaint();
            return;
        }
        // Voice mutes, mute or solo.
        const auto& voices = layout().voices[static_cast<size_t>(t)];
        if (layout().rows[static_cast<size_t>(t)] > 1 && disclosure(t).expanded(4, 4).contains(p.toInt()))
        {
            setCollapsed(t, expanded(t));
            return;
        }
        for (size_t k = 0; expanded(t) && k < voices.size(); ++k)
            if (voicePill(t, static_cast<int>(k)).contains(p.toInt()))
            {
                const int voice = voices[k];
                model_.edit("Mute voice", [t, voice](Tune& tune) {
                    tune.tracks[static_cast<size_t>(t)].mutedVoices ^= static_cast<uint16_t>(1 << voice);
                });
                return;
            }
        const bool mute = pill(t, 0).contains(p.toInt());
        const bool solo = pill(t, 1).contains(p.toInt());
        if (mute || solo)
            model_.edit(mute ? "Mute" : "Solo", [t, mute](Tune& tune) {
                auto& tr = tune.tracks[static_cast<size_t>(t)];
                (mute ? tr.mute : tr.solo) = !(mute ? tr.mute : tr.solo);
            });
        return;
    }

    bool edge = false;
    const NoteRef hit = noteAt(p, edge);
    if (hit.valid())
    {
        if (e.mods.isShiftDown())
        {
            if (!selection_.erase(hit))
                selection_.insert(hit);
        }
        else if (!selection_.count(hit))
            selection_ = { hit };
        dragNote_ = hit;
        drag_ = edge ? Drag::Resize : Drag::Move;
    }
    else
    {
        if (!e.mods.isShiftDown())
            selection_.clear();
        drag_ = Drag::Box;
        box_ = { p, p };
    }
    repaint();
}

void TrackerView::mouseDrag(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
        return;
    const auto p = e.position;
    if (drag_ == Drag::Knob)
    {
        // 200 pixels turn it end to end.
        const float moved = (p.x - dragFrom_.x) - (p.y - dragFrom_.y);
        const float position = juce::jlimit(0.0f, 1.0f, dragKnob_ + moved / 200.0f);
        setVolume(dragTrack_, knobVolume(position));
        return;
    }
    if (drag_ == Drag::Ruler)
    {
        model_.setPosition(std::max(0.0, xToTime(p.x)));
        return;
    }
    if (drag_ == Drag::Move || drag_ == Drag::Resize)
    {
        const int index = indexOf(dragNote_);
        if (index < 0)
            return;
        const TuneNote& n = model_.tune().tracks[static_cast<size_t>(dragNote_.track)].notes[static_cast<size_t>(index)];
        const double raw = (p.x - dragFrom_.x) * samplesPerPixel_;
        if (drag_ == Drag::Move)
            dragDelta_ = snapTime(static_cast<double>(n.start) + raw) - static_cast<double>(n.start);
        else
        {
            const double end = static_cast<double>(n.start + n.length);
            dragDelta_ = std::max(snapTime(end + raw), static_cast<double>(n.start) + kMinLength) - end;
        }
        repaint();
        return;
    }
    if (drag_ == Drag::Box)
    {
        box_ = juce::Rectangle<float>(dragFrom_, p);
        // Select every note the box touches.
        if (!e.mods.isShiftDown())
            selection_.clear();
        const auto& tracks = model_.tune().tracks;
        const int t0 = std::max(0, yToTrack(std::max<float>(box_.getY(), kRulerHeight)));
        const int t1 = yToTrack(box_.getBottom());
        for (int t = t0; t <= (t1 < 0 ? static_cast<int>(tracks.size()) - 1 : t1); ++t)
            for (size_t i = 0; i < tracks[static_cast<size_t>(t)].notes.size(); ++i)
                if (noteBounds(t, static_cast<int>(i)).intersects(box_))
                    selection_.insert({ t, tracks[static_cast<size_t>(t)].notes[i].id });
        repaint();
    }
}

void TrackerView::mouseUp(const juce::MouseEvent&)
{
    const Drag finished = drag_;
    drag_ = Drag::None;
    if ((finished == Drag::Move || finished == Drag::Resize) && std::abs(dragDelta_) >= 1.0)
    {
        const auto chosen = selection_;
        const auto delta = static_cast<int64_t>(std::llround(dragDelta_));
        const bool move = finished == Drag::Move;
        model_.edit(move ? "Move notes" : "Resize notes", [chosen, delta, move](Tune& tune) {
            for (size_t t = 0; t < tune.tracks.size(); ++t)
            {
                auto& notes = tune.tracks[t].notes;
                for (TuneNote& n : notes)
                    if (chosen.count({ static_cast<int>(t), n.id }))
                    {
                        if (move)
                            n.start = std::max<int64_t>(0, n.start + delta);
                        else
                            n.length = std::max<int64_t>(static_cast<int64_t>(kMinLength), n.length + delta);
                    }
                std::stable_sort(notes.begin(), notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
            }
        });
    }
    dragDelta_ = 0;
    repaint();
}

void TrackerView::mouseMove(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
        return;
    bool edge = false;
    hover_ = noteAt(e.position, edge);
    hoverEdge_ = edge;
    setMouseCursor(hover_.valid() && edge ? juce::MouseCursor::LeftRightResizeCursor
                   : hover_.valid()       ? juce::MouseCursor::DraggingHandCursor
                   : e.position.y < kRulerHeight && e.position.x > kHeaderWidth ? juce::MouseCursor::PointingHandCursor
                                                                                 : juce::MouseCursor::NormalCursor);
    if (hover_.valid())
    {
        const int i = indexOf(hover_);
        const auto& tr = model_.tune().tracks[static_cast<size_t>(hover_.track)];
        const auto& n = tr.notes[static_cast<size_t>(i)];
        setTooltip(tr.kind == TuneTrack::Kind::Fm ? theme::noteName(model_.tune().pitch(tr, n)) + theme::dot()
                                                        + juce::String(static_cast<double>(n.length) / kVgmRate * 1000.0, 0) + " ms"
                                                  : juce::String(tr.name));
    }
    else
        setTooltip({});
}

void TrackerView::setVolume(int track, float volume)
{
    // In tenths of a dB: a drag edits only when the level changes, as one undo step.
    volume = volume <= TuneTrack::kSilent ? TuneTrack::kSilent : std::round(juce::jlimit(TuneTrack::kSilent, TuneTrack::kMaxVolume, volume) * 10.0f) / 10.0f;
    if (std::abs(model_.tune().tracks[static_cast<size_t>(track)].volume - volume) < 0.01f)
    {
        repaint();
        return;
    }
    model_.edit("Volume", [track, volume](Tune& tune) { tune.tracks[static_cast<size_t>(track)].volume = volume; }, !dragStarted_);
    dragStarted_ = true;
}

void TrackerView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (model_.hasTune() && e.position.x < kHeaderWidth)
        if (const int t = yToTrack(e.position.y); t >= 0 && knobBounds(t).expanded(3).contains(e.position.toInt()))
        {
            dragStarted_ = false;
            setVolume(t, 0.0f);
            return;
        }
    if (!model_.hasTune() || e.position.x < kHeaderWidth || e.position.y < kRulerHeight)
        return;
    bool edge = false;
    const NoteRef hit = noteAt(e.position, edge);
    if (hit.valid())
        editPitch(hit);
    else if (const int t = yToTrack(e.position.y); t >= 0)
        addNoteAt(t, snapTime(xToTime(e.position.x)));
}

void TrackerView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (!model_.hasTune())
        return;
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomAround(std::pow(1.6, static_cast<double>(w.deltaY) * 2.0), e.position.x);
        return;
    }
    if (e.mods.isAltDown())
    {
        zoomLanes(std::pow(1.6, static_cast<double>(w.deltaY) * 2.0), e.position.y);
        return;
    }
    // Over a selected note, the wheel changes its pitch: one semitone a notch.
    bool edge = false;
    const NoteRef hit = noteAt(e.position, edge);
    if (hit.valid() && selection_.count(hit) && std::abs(w.deltaY) > std::abs(w.deltaX))
    {
        static float accumulated = 0;
        accumulated += w.deltaY;
        if (std::abs(accumulated) >= 0.12f)
        {
            transposeSelection((accumulated > 0 ? 1 : -1) * (e.mods.isShiftDown() ? 12 : 1));
            accumulated = 0;
        }
        return;
    }
    // Otherwise it scrolls: sideways through time, up and down through tracks.
    const float dx = std::abs(w.deltaX) > 0 ? w.deltaX : (e.mods.isShiftDown() ? w.deltaY : 0.0f);
    const float dy = e.mods.isShiftDown() ? 0.0f : w.deltaY;
    if (std::abs(dx) > 0)
        setViewStart(viewStart_ - dx * 400.0 * samplesPerPixel_);
    if (std::abs(dy) > 0)
        setScrollY(scrollY_ - static_cast<int>(dy * 200.0f));
}

void TrackerView::mouseMagnify(const juce::MouseEvent& e, float scale)
{
    // Pinch zooms time; with Shift, the tracks' height.
    if (e.mods.isShiftDown())
        zoomLanes(scale, e.position.y);
    else
        zoomAround(scale, e.position.x);
}

// Editing ---------------------------------------------------------------------

bool TrackerView::keyPressed(const juce::KeyPress& key)
{
    if (!model_.hasTune())
        return false;
    const auto mods = key.getModifiers();
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return deleteSelection(), true;
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0))
        return model_.undo().undo(), true;
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return model_.undo().redo(), true;
    if (key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0))
        return duplicateSelection(), true;
    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0))
    {
        selection_.clear();
        const auto& tracks = model_.tune().tracks;
        for (size_t t = 0; t < tracks.size(); ++t)
            for (const TuneNote& n : tracks[t].notes)
                selection_.insert({ static_cast<int>(t), n.id });
        repaint();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
    {
        const int step = mods.isShiftDown() ? 12 : 1;
        transposeSelection(key.getKeyCode() == juce::KeyPress::upKey ? step : -step);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)
    {
        if (selection_.empty())
            return false;
        const double step = snap_ > 0 ? snap_ : model_.tune().source().beatLength / 4;
        const auto delta = static_cast<int64_t>(std::llround(key.getKeyCode() == juce::KeyPress::rightKey ? step : -step));
        const auto chosen = selection_;
        model_.edit("Move notes", [chosen, delta](Tune& tune) {
            for (size_t t = 0; t < tune.tracks.size(); ++t)
            {
                auto& notes = tune.tracks[t].notes;
                for (TuneNote& n : notes)
                    if (chosen.count({ static_cast<int>(t), n.id }))
                        n.start = std::max<int64_t>(0, n.start + delta);
                std::stable_sort(notes.begin(), notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
            }
        });
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        selection_.clear();
        repaint();
        return true;
    }
    return false;
}

void TrackerView::transposeSelection(int semitones)
{
    if (selection_.empty())
        return;
    const auto chosen = selection_;
    model_.edit("Transpose", [chosen, semitones](Tune& tune) {
        for (size_t t = 0; t < tune.tracks.size(); ++t)
            for (TuneNote& n : tune.tracks[t].notes)
                if (chosen.count({ static_cast<int>(t), n.id }))
                    n.transpose = std::clamp(n.transpose + semitones, -48, 48);
    });
}

void TrackerView::deleteSelection()
{
    if (selection_.empty())
        return;
    const auto chosen = selection_;
    model_.edit("Delete notes", [chosen](Tune& tune) {
        for (size_t t = 0; t < tune.tracks.size(); ++t)
        {
            auto& notes = tune.tracks[t].notes;
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [&](const TuneNote& n) { return chosen.count({ static_cast<int>(t), n.id }) > 0; }),
                        notes.end());
        }
    });
    selection_.clear();
}

void TrackerView::duplicateSelection()
{
    if (selection_.empty())
        return;
    // The copy goes right after the selection, rounded up to a whole bar.
    const auto& tracks = model_.tune().tracks;
    int64_t first = std::numeric_limits<int64_t>::max(), last = 0;
    for (const NoteRef& r : selection_)
        if (const int i = indexOf(r); i >= 0)
        {
            const TuneNote& n = tracks[static_cast<size_t>(r.track)].notes[static_cast<size_t>(i)];
            first = std::min(first, n.start);
            last = std::max(last, n.start + n.length);
        }
    const double bar = model_.tune().source().beatLength * 4;
    const auto shift = static_cast<int64_t>(std::ceil(static_cast<double>(last - first) / bar) * bar);
    const auto chosen = selection_;
    std::set<NoteRef> copies;
    model_.edit("Duplicate notes", [chosen, shift, &copies](Tune& tune) {
        int id = tune.newNoteId();
        for (size_t t = 0; t < tune.tracks.size(); ++t)
        {
            auto& notes = tune.tracks[t].notes;
            std::vector<TuneNote> added;
            for (const TuneNote& n : notes)
                if (chosen.count({ static_cast<int>(t), n.id }))
                {
                    TuneNote copy = n;
                    copy.id = id++;
                    copy.start += shift;
                    added.push_back(copy);
                    copies.insert({ static_cast<int>(t), copy.id });
                }
            notes.insert(notes.end(), added.begin(), added.end());
            std::stable_sort(notes.begin(), notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
        }
    });
    selection_ = copies;
    repaint();
}

void TrackerView::editPitch(NoteRef ref)
{
    const int i = indexOf(ref);
    if (i < 0)
        return;
    const auto& tr = model_.tune().tracks[static_cast<size_t>(ref.track)];
    const auto& n = tr.notes[static_cast<size_t>(i)];
    const int current = model_.tune().pitch(tr, n);
    const int base = current - n.transpose;
    auto editor = std::make_unique<PitchEditor>(theme::noteName(current), [this, ref, base](int note) {
        model_.edit("Set pitch", [ref, note, base](Tune& tune) {
            for (TuneNote& x : tune.tracks[static_cast<size_t>(ref.track)].notes)
                if (x.id == ref.id)
                    x.transpose = std::clamp(note - base, -48, 48);
        });
    });
    juce::CallOutBox::launchAsynchronously(std::move(editor), noteBounds(ref.track, i).toNearestInt() + getScreenPosition(), nullptr);
}

void TrackerView::addNoteAt(int track, double time)
{
    // A copy of the track's nearest note: the instrument's own sound.
    const auto& notes = model_.tune().tracks[static_cast<size_t>(track)].notes;
    if (notes.empty())
        return;
    const auto nearest = std::min_element(notes.begin(), notes.end(), [time](const TuneNote& a, const TuneNote& b) {
        return std::abs(static_cast<double>(a.start) - time) < std::abs(static_cast<double>(b.start) - time);
    });
    TuneNote copy = *nearest;
    copy.start = static_cast<int64_t>(std::max(0.0, time));
    NoteRef added;
    model_.edit("Add note", [track, copy, &added](Tune& tune) mutable {
        copy.id = tune.newNoteId();
        added = { track, copy.id };
        auto& list = tune.tracks[static_cast<size_t>(track)].notes;
        list.push_back(copy);
        std::stable_sort(list.begin(), list.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
    });
    selection_ = { added };
    repaint();
}

} // namespace opm86
