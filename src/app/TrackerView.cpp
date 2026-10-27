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

// Songs to start with, shown while no song is open: each opens its VGM file's
// download in the browser, from vgmrips.net. The app ships no music itself.
struct SongLink
{
    const char* title;
    const char* url;
};
// A chip as it sits on the board, for the empty view: its body, its legs
// and its markings in accent. The YM2151 is a long DIP, legs down its two
// sides and a notch at its top; the SegaPCM a square flat pack, legs on all
// four sides and a dot by pin 1. Under it, at nameY, the name it goes by.
void drawChip(juce::Graphics& g, juce::Rectangle<float> body, bool square, int legsPerSide, juce::Colour accent,
              const juce::String& maker, const juce::String& name, const juce::String& role, const juce::String& nickname, float nameY)
{
    const juce::Colour leg(150, 160, 200);
    const float pitch = (square ? body.getWidth() : body.getHeight()) / static_cast<float>(legsPerSide);
    const float legLength = square ? 12.0f : 16.0f, legWidth = pitch * 0.45f;
    g.setColour(leg);
    for (int i = 0; i < legsPerSide; ++i)
    {
        const float along = (static_cast<float>(i) + 0.5f) * pitch;
        // Down the left and right sides; a square pack has them along its top and bottom too.
        g.fillRect(body.getX() - legLength, body.getY() + along - legWidth * 0.5f, legLength, legWidth);
        g.fillRect(body.getRight(), body.getY() + along - legWidth * 0.5f, legLength, legWidth);
        if (square)
        {
            g.fillRect(body.getX() + along - legWidth * 0.5f, body.getY() - legLength, legWidth, legLength);
            g.fillRect(body.getX() + along - legWidth * 0.5f, body.getBottom(), legWidth, legLength);
        }
    }
    // The body, with a soft glow of its accent round it.
    g.setColour(accent.withAlpha(0.08f));
    g.fillRoundedRectangle(body.expanded(10.0f), 14.0f);
    g.setColour(juce::Colour(16, 20, 38));
    g.fillRoundedRectangle(body, 8.0f);
    g.setColour(theme::gridBar);
    g.drawRoundedRectangle(body.reduced(0.5f), 8.0f, 2.0f);
    if (square)
        g.fillEllipse(body.getX() + 16.0f, body.getY() + 16.0f, 12.0f, 12.0f); // pin 1
    else
    {
        // The notch, a half circle cut into the top, its edge drawn.
        const auto notch = juce::Rectangle<float>(28.0f, 28.0f).withCentre(body.getCentre().withY(body.getY()));
        g.setColour(theme::background);
        g.fillEllipse(notch);
        g.setColour(theme::gridBar);
        juce::Path edge;
        edge.addCentredArc(notch.getCentreX(), notch.getCentreY(), 14.0f, 14.0f, 0.0f, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi * 3.0f, true);
        g.strokePath(edge, juce::PathStrokeType(2.0f));
    }
    // The markings, centred.
    auto text = body.reduced(10.0f).withSizeKeepingCentre(body.getWidth() - 20.0f, 102.0f);
    g.setColour(accent.withAlpha(0.7f));
    g.setFont(theme::mono(22.0f, true));
    g.drawText(maker, text.removeFromTop(34.0f), juce::Justification::centred, false);
    g.setColour(accent);
    g.setFont(theme::pixel(std::min(24.0f, (body.getWidth() - 24.0f) / static_cast<float>(name.length()))));
    g.drawText(name, text.removeFromTop(42.0f), juce::Justification::centred, false);
    g.setColour(theme::dim);
    g.setFont(theme::mono(15.0f));
    g.drawText(role, text, juce::Justification::centred, false);
    g.setColour(accent);
    g.setFont(theme::pixel(18.0f));
    g.drawText(nickname, juce::Rectangle<float>(body.getX() - 60.0f, nameY, body.getWidth() + 120.0f, 24.0f),
               juce::Justification::centred, false);
}

const SongLink kSongLinks[] = {
    { "Magical Sound Shower (Out Run)", "https://vgmrips.net/packs/vgm/Arcade/SegaSys/Out_Run_%28Arcade%29/01%20Magical%20Sound%20Shower.vgz" },
    { "Passing Breeze, extended (Out Run)", "https://vgmrips.net/packs/vgm/Arcade/SegaSys/Out_Run_%28Arcade%29/06%20Passing%20Breeze%20%28Extended%29.vgz" },
    { "Rush a Difficulty (Turbo Out Run)", "https://vgmrips.net/packs/vgm/Arcade/Turbo_Out_Run_%28Arcade%29/04%20Rush%20a%20Difficulty.vgz" },
    { "After Burner (After Burner)", "https://vgmrips.net/packs/vgm/Arcade/SegaSys/After_Burner_%28Sega_X%29/05%20After%20Burner.vgz" },
    { "Like the Wind (Power Drift)", "https://vgmrips.net/packs/vgm/Arcade/Power_Drift_%28Sega_Y%29/04%20Like%20the%20Wind%20%28B%20Course%29.vgz" },
    { "Beyond the Galaxy (Galaxy Force II)", "https://vgmrips.net/packs/vgm/Arcade/Galaxy_Force_II_%28Sega_Y%29/04%20Beyond%20the%20Galaxy%20%28Scene%20A%20-%20Level%201%29.vgz" },
};

// A header's name and the tag right after it: the voice, V1 2, and the patch,
// FM 6; in patch order the other way round, the voice without its number,
// V1. Drums have their name alone.
juce::String headName(const TuneTrack& t, bool byPatch)
{
    return byPatch && t.kind == TuneTrack::Kind::Fm ? "FM " + juce::String(t.instrument + 1) : juce::String(t.name).toUpperCase();
}

juce::String tagName(const TuneTrack& t, bool byPatch)
{
    return t.kind != TuneTrack::Kind::Fm ? juce::String()
           : byPatch                     ? juce::String(t.name).upToFirstOccurrenceOf(" ", false, false).toUpperCase()
                                         : "FM " + juce::String(t.instrument + 1);
}

// The width an FM part's tag takes after the name, gap included; 0 for drums.
int instrumentWidth(const TuneTrack& t, bool byPatch)
{
    return t.kind == TuneTrack::Kind::Fm ? 8 + juce::GlyphArrangement::getStringWidthInt(theme::mono(12.0f, true), tagName(t, byPatch)) : 0;
}

// Header layout; see the comment before voicePill.
constexpr int kVoiceColumn = 44; // width of the voice toggles' column
constexpr int kChipColumn = 24;  // width of the chip's column, at the left edge
constexpr int kKnobColumn = 48;  // width of the dB under the knob
constexpr int kKnob = 32;        // the knob's size
constexpr int kBlock = 44;       // height of the name and meter block
constexpr int kMinHeaderWidth = 150; // room for TRACKS and the strips' names and chevrons
constexpr float kKnobArc = juce::MathConstants<float>::pi * 0.75f; // the knob turns 135 degrees each way

} // namespace

const TrackerView::LabelShapes& TrackerView::labelShapes(const juce::String& label, float height) const
{
    // Sizes to half a pixel, so a zoom makes only a few new ones.
    const int size = static_cast<int>(std::lround(height * 2.0f));
    const auto key = std::make_pair(label, size);
    if (auto it = labels_.find(key); it != labels_.end())
        return it->second;
    if (labels_.size() > 2000)
        labels_.clear();
    const float h = static_cast<float>(size) * 0.5f;
    LabelShapes shapes;
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(theme::mono(h, true), label, 0.0f, 0.0f);
    glyphs.createPath(shapes.text);
    juce::PathStrokeType(std::max(2.0f, h * 0.16f), juce::PathStrokeType::curved).createStrokedPath(shapes.outline, shapes.text);
    return labels_.emplace(key, std::move(shapes)).first->second;
}

TrackerView::TrackerView(AppModel& model) : model_(model)
{
    setOpaque(true); // it paints every pixel, so nothing behind it repaints
    model_.addChangeListener(this);
    setWantsKeyboardFocus(true);
}

TrackerView::~TrackerView()
{
    model_.removeChangeListener(this);
}

void TrackerView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // When a song closes, the tooltip of the note under the mouse goes with it.
    if (!model_.hasTune() && hover_.valid())
    {
        hover_ = {};
        setTooltip({});
    }
    // Selected notes belong to the song they were selected in, and a new
    // section drops them in the model (AppModel::setSelection).
    if (!selectedNotes_.empty() && (!model_.hasTune() || model_.file() != selectedFile_ || model_.selectedNotes().empty()))
        selectedNotes_.clear(), ++selectedRevision_;
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
        layout_.totalRows += layout_.shown.back();
        layout_.longest.push_back(longest);
        layout_.index.push_back(std::move(index));

        // The names, each with its tag, share a column as wide as the widest,
        // so mute and solo line up; the knobs go after them.
        const int left = kChipColumn + (layout_.rows.back() > 1 ? 28 : 14);
        // The name and, right after it, the tag: the patch or the voice. The
        // wider of the two orders, so the header keeps its width when the
        // order changes.
        auto width = [&](bool byPatch) {
            return std::min(juce::GlyphArrangement::getStringWidthInt(theme::mono(13.0f, true), headName(t, byPatch)) + 2, 96)
                   + instrumentWidth(t, byPatch);
        };
        const int name = std::max(width(false), width(true));
        layout_.nameEnd = std::max(layout_.nameEnd, left + name);
    }
    // The order shown: voice order as the tune has it, or with the FM tracks
    // by patch, then by their voice and number; drums stay last.
    std::vector<int> order(tracks.size());
    for (size_t t = 0; t < order.size(); ++t)
        order[t] = static_cast<int>(t);
    if (byPatch_)
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const TuneTrack &x = tracks[static_cast<size_t>(a)], &y = tracks[static_cast<size_t>(b)];
            const bool xd = x.kind == TuneTrack::Kind::Drum, yd = y.kind == TuneTrack::Kind::Drum;
            if (xd || yd)
                return !xd && yd;
            return x.instrument < y.instrument;
        });
    layout_.firstRow.assign(tracks.size(), 0);
    layout_.position.assign(tracks.size(), 0);
    int rowsAbove = 0;
    for (size_t k = 0; k < order.size(); ++k)
    {
        const auto t = static_cast<size_t>(order[k]);
        layout_.position[t] = static_cast<int>(k);
        layout_.firstRow[t] = rowsAbove;
        rowsAbove += layout_.shown[t];
    }
    layout_.knobX = layout_.nameEnd + 8 + 2 * 23 + 3 + 8;
    // The header ends past the knobs' column and the dB under them, with
    // room for the voice toggles when a chord part shows them.
    bool toggles = false;
    for (size_t t = 0; t < tracks.size(); ++t)
        toggles = toggles || (!layout_.voices[t].empty() && layout_.shown[t] > 1);
    layout_.headerWidth = std::max(kMinHeaderWidth, layout_.knobX + (kKnob + kKnobColumn) / 2 + 6 + (toggles ? kVoiceColumn : 0));
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
    return (getWidth() - headerWidth()) * samplesPerPixel_;
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
    samplesPerPixel_ = std::clamp(samplesPerPixel_ / factor, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    setViewStart(anchor - (x - lanesX()) * samplesPerPixel_);
    centrePlayhead();
}

void TrackerView::centrePlayhead()
{
    // Following playback, a zoom puts the playhead in the middle at once,
    // short of the song start, unless the whole song is in view.
    if (!follow_ || !model_.playing() || !model_.hasTune())
        return;
    gliding_ = false;
    if (viewStart_ <= 0 && viewStart_ + visibleSamples() >= static_cast<double>(model_.tune().source().length))
        return;
    setViewStart(std::max(0.0, playhead() - visibleSamples() * 0.5));
}

void TrackerView::followPlayhead()
{
    // One time for the frame: the view, the line and the notes lit all use it.
    head_ = model_.displayPosition();
    const double now = juce::Time::getMillisecondCounterHiRes();
    // While the view is dragged or scrolled by hand, and for a moment after,
    // it is left alone.
    if (!follow_ || !model_.playing() || drag_ == Drag::Pan || !model_.hasTune() || now - movedByHand_ < kHandsOffMs)
    {
        gliding_ = false;
        return;
    }
    // Where reveal would put the view; a jump further than kGlideFrom pixels,
    // after a click ahead, or any way back after the view was moved by hand,
    // glides there instead of cutting.
    const double p = head_;
    const double before = viewStart_;
    reveal(p);
    const double target = viewStart_;
    viewStart_ = before;
    const bool moves = std::abs(target - before) > 0.5;
    if (!moves)
        pulledAway_ = false;
    if (!gliding_ && moves && (pulledAway_ || std::abs(target - before) > kGlideFrom * samplesPerPixel_))
    {
        gliding_ = true;
        glideFrom_ = before;
        glideStart_ = now;
    }
    if (!gliding_ || !moves)
    {
        gliding_ = false;
        if (moves)
            setViewStart(target);
        return;
    }
    // Ease in and out towards the target, which moves on with the playhead.
    const double t = (now - glideStart_) / kGlideMs;
    if (t >= 1.0)
    {
        gliding_ = pulledAway_ = false;
        setViewStart(target);
        return;
    }
    const double eased = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
    setViewStart(glideFrom_ + (target - glideFrom_) * eased);
}

void TrackerView::reveal(double p)
{
    // With the whole song in view, the view stays still.
    if (!model_.hasTune() || (viewStart_ <= 0 && viewStart_ + visibleSamples() >= static_cast<double>(model_.tune().source().length)))
        return;
    // The playhead runs to the middle, then the song moves under it; behind
    // the view, the view goes back to put it in the middle, short of the start.
    if (p > viewStart_ + visibleSamples() * 0.5)
        setViewStart(p - visibleSamples() * 0.5);
    else if (p < viewStart_)
        setViewStart(std::max(0.0, p - visibleSamples() * 0.5));
}

void TrackerView::fitTime()
{
    // The whole rip across, from its start to its end, with a little room either side.
    if (!model_.hasTune())
        return;
    const double length = static_cast<double>(model_.tune().source().length);
    const double width = std::max(1.0, static_cast<double>(getWidth() - headerWidth()));
    samplesPerPixel_ = std::clamp(length * 1.04 / width, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    setViewStart(-length * 0.02);
    // Too long to fit: on the playhead, rather than at the start and gliding back.
    centrePlayhead();
}

void TrackerView::fitTracks()
{
    if (!model_.hasTune() || layout().totalRows == 0)
        return;
    // The rows from the first track that can be heard to the last, muted
    // tracks between them included; with none to hear, every row.
    int first = layout().totalRows, last = 0;
    const auto& tracks = model_.tune().tracks;
    for (size_t t = 0; t < tracks.size(); ++t)
        if (model_.tune().audible(tracks[t]))
        {
            first = std::min(first, layout().firstRow[t]);
            last = std::max(last, layout().firstRow[t] + layout().shown[t]);
        }
    if (first >= last)
        first = 0, last = layout().totalRows;
    const double room = getHeight() - kRulerHeight;
    laneHeightExact_ = std::clamp(std::floor(room / (last - first)), static_cast<double>(kMinLaneHeight), static_cast<double>(kMaxLaneHeight));
    laneHeight_ = static_cast<int>(laneHeightExact_);
    setScrollY(first * laneHeight_);
}

double TrackerView::xToTime(float x) const
{
    return viewStart_ + (x - lanesX()) * samplesPerPixel_;
}

float TrackerView::timeToX(double t) const
{
    return static_cast<float>(headerWidth() + (t - viewStart_) / samplesPerPixel_);
}

int TrackerView::yToTrack(float y) const
{
    if (y < kRulerHeight || !model_.hasTune())
        return -1;
    const int row = static_cast<int>((y - static_cast<float>(kRulerHeight - scrollY_)) / static_cast<float>(laneHeight_));
    for (size_t t = 0; t < layout().firstRow.size(); ++t)
        if (row >= layout().firstRow[t] && row < layout().firstRow[t] + layout().shown[t])
            return static_cast<int>(t);
    return -1;
}

int TrackerView::trackTop(int track) const
{
    return kRulerHeight + layout().firstRow[static_cast<size_t>(track)] * laneHeight_ - scrollY_;
}

int TrackerView::trackHeight(int track) const
{
    return layout().shown[static_cast<size_t>(track)] * laneHeight_;
}

void TrackerView::setOrderByPatch(bool byPatch)
{
    byPatch_ = byPatch;
    layoutRevision_ = -1;
    setScrollY(scrollY_);
    sendChangeMessage(); // the overview follows the order
}

void TrackerView::setShowBends(bool show)
{
    showBends_ = show;
    repaint();
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
    return { kChipColumn + 10, nameBounds(track).getY(), 14, nameBounds(track).getHeight() };
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

TrackerView::NoteRef TrackerView::noteAt(juce::Point<float> p) const
{
    const int t = yToTrack(p.y);
    if (t < 0 || p.x < lanesX())
        return {};
    const auto& notes = model_.tune().tracks[static_cast<size_t>(t)].notes;
    const double time = xToTime(p.x);
    // The note under the pointer that starts last, so the shorter one on top wins.
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
    {
        const TuneNote& n = notes[static_cast<size_t>(i)];
        if (static_cast<double>(n.start) > time)
            continue;
        if (static_cast<double>(n.start + n.length + layout().longest[static_cast<size_t>(t)]) < time)
            break;
        const auto r = noteBounds(t, i);
        if (r.expanded(1.0f, 0.0f).contains(p))
            return { t, n.id };
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

    // Channel levels fall back over about a third of a second; a track shows its loudest channel.
    const auto peaks = model_.takePeaks();
    const float fall = static_cast<float>(std::pow(0.82, seconds * 60.0));
    for (size_t c = 0; c < peaks.size(); ++c)
        channelLevel_[c] = model_.playing() ? std::max(peaks[c], channelLevel_[c] * fall) : channelLevel_[c] * fall;
    level_.resize(tracks.size(), 0.0f);
    hold_.resize(tracks.size(), 0.0f);
    holdAge_.resize(tracks.size(), 0.0);
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        // A dB scale from kMeterFloor to full scale, so a volume change moves
        // the meter by as much as it changes the sound.
        const float db = juce::Decibels::gainToDecibels(trackLevel(static_cast<int>(t)), kMeterFloor);
        level_[t] = (db - kMeterFloor) / -kMeterFloor;
        if (level_[t] >= hold_[t])
            hold_[t] = level_[t], holdAge_[t] = 0.0;
        else if ((holdAge_[t] += seconds) > 1.0)
            hold_[t] = std::max(level_[t], hold_[t] - static_cast<float>(seconds) * 0.6f);
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

void TrackerView::wake()
{
    lastActivity_ = juce::Time::getMillisecondCounterHiRes();
    repaint();
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
// right, and under them the level meter; right next to both, the volume knob
// with the level in dB under it. Compact lanes keep only the name line. At
// the right edge, a chord part's voice toggles, one beside each row.

juce::Rectangle<int> TrackerView::voicePill(int track, int k) const
{
    const int h = std::min(18, laneHeight_ - 4);
    return { headerWidth() - kVoiceColumn + 4, trackTop(track) + k * laneHeight_ + (laneHeight_ - h) / 2, 30, h };
}

juce::Rectangle<int> TrackerView::nameBounds(int track) const
{
    // After the arrow of a chord part, up to the names' shared column, but
    // leaving mute and solo room before the knob, or before the voice
    // toggles in a compact lane. Centred in a compact lane.
    const int left = kChipColumn + (layout().rows[static_cast<size_t>(track)] > 1 ? 28 : 14);
    const bool toggles = !layout().voices[static_cast<size_t>(track)].empty() && expanded(track);
    const int right = compact() ? headerWidth() - (toggles ? kVoiceColumn : 0) - 4 : knobBounds(track).getX() - 8;
    const int room = right - left - 2 * (pillWidth() + 3) - 5;
    const int width = std::min(layout().nameEnd - left, std::max(24, room));
    const int height = minimal() ? laneHeight_ - 6 : 17;
    return { left, trackTop(track) + (laneHeight_ - (compact() ? height : kBlock)) / 2, width, height };
}

juce::Rectangle<int> TrackerView::pill(int track, int which) const
{
    const auto name = nameBounds(track);
    return { name.getRight() + 8 + which * (pillWidth() + 3), name.getY(), pillWidth(), name.getHeight() };
}

juce::Rectangle<int> TrackerView::knobBounds(int track) const
{
    // In one column for all tracks, right after the widest name line, but
    // never into the voice column when a chord part shows its toggles.
    const int top = trackTop(track) + (laneHeight_ - kBlock) / 2;
    const bool toggles = !layout().voices[static_cast<size_t>(track)].empty() && expanded(track);
    const int right = headerWidth() - (toggles ? kVoiceColumn : 0) - 4 - (kKnobColumn + kKnob) / 2;
    return { std::min(layout().knobX, right), top, kKnob, kKnob };
}

juce::Rectangle<int> TrackerView::meterBounds(int track) const
{
    // The width up to the knob and the level in dB under it.
    const int top = trackTop(track) + (laneHeight_ - kBlock) / 2;
    return { kChipColumn + 14, top + 25, knobBounds(track).getCentreX() - kKnobColumn / 2 - 4 - kChipColumn - 14, 16 };
}

// The knob law: 0 dB, as the game played it, sits at the middle, so the
// right half boosts, evenly in dB up to +6 at full right. In the left half
// amplitude goes with the cube of the position: -18 dB at a quarter, and the
// first twentieth is silence.
float TrackerView::knobPosition(float volume)
{
    if (volume <= TuneTrack::kSilent)
        return 0.0f;
    if (volume <= 0.0f)
        return 0.5f * std::pow(10.0f, volume / 60.0f);
    return 0.5f + 0.5f * std::min(volume, TuneTrack::kMaxVolume) / TuneTrack::kMaxVolume;
}

float TrackerView::knobVolume(float position)
{
    if (position <= knobPosition(TuneTrack::kSilent + 0.01f))
        return TuneTrack::kSilent;
    if (position <= 0.5f)
        return 60.0f * std::log10(position * 2.0f);
    return std::min(TuneTrack::kMaxVolume, (position - 0.5f) * 2.0f * TuneTrack::kMaxVolume);
}

// Painting --------------------------------------------------------------------

void TrackerView::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    if (!model_.hasTune())
    {
        // A block of lines, centred in the view as a whole.
        constexpr float kTitle = 22.0f, kText = 18.0f, kLink = 34.0f;
        const float width = static_cast<float>(getWidth());
        const float height = kTitle + 30.0f + kText + 44.0f + kText + 22.0f + kLink * static_cast<float>(std::size(kSongLinks));
        float y = (static_cast<float>(getHeight()) - height) * 0.5f;
        auto line = [&](float h) { const auto r = juce::Rectangle<float>(0.0f, y, width, h); y += h; return r; };
        // The two chips beside the block, the YM2151 left and the SegaPCM
        // right, when there is room for them beside its widest line.
        {
            const float block = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(theme::mono(kText), "No songs yet? Download one from vgmrips.net, then open it:"));
            const float side = (width - block) * 0.5f - 60.0f; // the room each side leaves, short of a gap
            const float size = std::min({ 260.0f, side - 40.0f, static_cast<float>(getHeight()) - 150.0f }); // with room for the name under it
            if (size >= 150.0f)
            {
                const float cy = static_cast<float>(getHeight()) * 0.5f - 24.0f; // the chip and its name, centred together
                const float leftX = (width - block) * 0.25f - 30.0f, rightX = width - leftX;
                const float nameY = cy + size * 0.5f + 34.0f; // both names on one line, under the taller chip
                drawChip(g, juce::Rectangle<float>(size * 0.5f, size).withCentre({ leftX, cy }), false, 12, theme::cyan, "YAMAHA", "YM2151", "FM, 8 voices", "OPM", nameY);
                drawChip(g, juce::Rectangle<float>(size * 0.8f, size * 0.8f).withCentre({ rightX, cy }), true, 14, theme::orange, "SEGA", "315-5218", "PCM, 16 voices", "SEGA PCM", nameY);
            }
        }
        g.setColour(theme::dim);
        g.setFont(theme::pixel(18.0f));
        g.drawText("OPEN A VGM RIP TO START", line(kTitle), juce::Justification::centred, false);
        y += 30.0f;
        g.setFont(theme::mono(kText));
        g.drawText("File > Open, or drop a .vgz file here", line(kText), juce::Justification::centred, false);
        y += 44.0f;
        // Songs to download, a link each under a line that says where from,
        // with the site's name a link to it.
        links_.clear();
        const auto pointer = getMouseXYRelative().toFloat();
        const auto linkFont = theme::mono(kText, true);
        auto drawLink = [&](const juce::String& text, juce::Rectangle<float> at, const juce::String& url) {
            links_.emplace_back(at, url);
            g.setFont(linkFont);
            g.setColour(isMouseOver() && at.contains(pointer) ? theme::text : theme::cyan);
            g.drawText(text, at, juce::Justification::centred, false);
            g.fillRect(at.getX(), at.getBottom() - 1.0f, at.getWidth(), 1.0f);
        };
        {
            const juce::String before("No songs yet? Download one from "), site("vgmrips.net"), after(", then open it:");
            const auto plain = theme::mono(kText);
            const float wBefore = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(plain, before));
            const float wSite = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(linkFont, site));
            const float wAfter = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(plain, after));
            const auto row = line(kText);
            float x = row.getCentreX() - (wBefore + wSite + wAfter) * 0.5f;
            g.setFont(plain);
            g.setColour(theme::dim);
            g.drawText(before, juce::Rectangle<float>(x, row.getY(), wBefore, kText), juce::Justification::centredLeft, false);
            x += wBefore;
            drawLink(site, juce::Rectangle<float>(x, row.getY() - 3.0f, wSite, kText + 6.0f), "https://vgmrips.net");
            x += wSite;
            g.setFont(plain);
            g.setColour(theme::dim);
            g.drawText(after, juce::Rectangle<float>(x, row.getY(), wAfter, kText), juce::Justification::centredLeft, false);
        }
        y += 22.0f;
        for (const SongLink& song : kSongLinks)
        {
            const juce::String title(song.title);
            const float w = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(linkFont, title));
            drawLink(title, line(kLink).withSizeKeepingCentre(w, kText + 6.0f), song.url);
        }
        return;
    }
    paintLanes(g);
    paintRuler(g);
    paintHeaders(g);

    // The playhead, over lanes and ruler, with a soft glow. A second after
    // the last mouse or key event its line fades out, and comes back with
    // the next; the head on the ruler always shows.
    const double idle = (juce::Time::getMillisecondCounterHiRes() - lastActivity_) / 1000.0;
    const float shown = keepPlayhead_ ? 1.0f : juce::jlimit(0.0f, 1.0f, 1.0f - static_cast<float>(idle - 1.0) / 0.3f);
    const float x = timeToX(playhead());
    if (x >= lanesX() && x <= static_cast<float>(getWidth()))
    {
        if (shown > 0.0f)
        {
            for (int w = 6; w >= 2; w -= 2)
            {
                g.setColour(theme::cyan.withAlpha(0.07f * shown));
                g.fillRect(x - static_cast<float>(w) / 2.0f, 0.0f, static_cast<float>(w), static_cast<float>(getHeight()));
            }
            g.setColour(theme::cyan.withAlpha(shown));
            g.fillRect(x - 0.75f, 0.0f, 1.5f, static_cast<float>(getHeight()));
        }
        juce::Path head;
        head.addTriangle(x - 6, 0, x + 6, 0, x, 9);
        g.setColour(theme::cyan);
        g.fillPath(head);
    }
}

void TrackerView::paintRuler(juce::Graphics& g)
{
    const auto area = juce::Rectangle<int>(headerWidth(), 0, getWidth() - headerWidth(), kRulerHeight);
    g.setColour(theme::panel);
    g.fillRect(area);
    g.setColour(theme::gridBar);
    g.fillRect(area.getX(), area.getBottom() - 1, area.getWidth(), 1);
    // The selection: the ruler tinted over it, with a bar along its foot.
    if (model_.hasSelection())
    {
        const float x0 = std::max(static_cast<float>(area.getX()), timeToX(model_.selectionStart()));
        const float x1 = timeToX(model_.selectionEnd());
        if (x1 > x0)
        {
            g.setColour(theme::cyan.withAlpha(0.35f));
            g.fillRect(x0, 0.0f, x1 - x0, static_cast<float>(kRulerHeight));
            g.setColour(theme::cyan);
            g.fillRect(x0, static_cast<float>(kRulerHeight) - 6.0f, x1 - x0, 5.0f);
        }
    }

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
        if (x < lanesX())
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
    // The lanes at rest come from tiles_, drawn only when needed: drawing
    // every note each frame is most of a frame's cost. Over them go the notes
    // sounding now, which change from frame to frame.
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const float height = static_cast<float>(getHeight() - kRulerHeight);
    if (static_cast<float>(getWidth()) <= lanesX() || height <= 0.0f)
        return;
    // Where things are, and the selection, which follows a drag: all redrawn now.
    std::vector<double> key { model_.selectionStart(), model_.selectionEnd(), samplesPerPixel_, static_cast<double>(getHeight()),
                              lanesX(), scale };
    for (int t = 0; t < static_cast<int>(model_.tune().tracks.size()); ++t)
        key.push_back(trackTop(t)), key.push_back(trackHeight(t)); // collapse, order, lane height and vertical scroll
    if (key != lanesKey_)
        tiles_.clear(), lanesKey_ = std::move(key);
    // How the notes look: edits (mute, solo, volume), the build's channels,
    // the colours, the selected notes and the pitch lines. Redrawn over frames.
    std::vector<double> looks { static_cast<double>(model_.revision()), static_cast<double>(model_.reportRevision()),
                                static_cast<double>(theme::pitchColoursRevision()), static_cast<double>(selectedRevision_),
                                showBends_ ? 1.0 : 0.0 };
    if (looks != looksKey_)
        ++looksVersion_, looksKey_ = std::move(looks);

    // The tiles in view, and the one about to come into it.
    const double span = kTileWidth * samplesPerPixel_; // song time a tile covers
    const auto first = static_cast<long>(std::floor(viewStart_ / span));
    const auto last = static_cast<long>(std::floor(xToTime(static_cast<float>(getWidth())) / span)) + 1;
    for (auto it = tiles_.begin(); it != tiles_.end();)
        it = it->first < first - 2 || it->first > last + 2 ? tiles_.erase(it) : std::next(it);
    // Every tile starts a whole number of tiles from the next, so they share
    // one rounding to device pixels, and the sounding notes take it too:
    // tiles are copied, not resampled, and the notes stay on them.
    const float exact = timeToX(static_cast<double>(first) * span);
    const float shift = std::round(exact * scale) / scale - exact;
    g.saveState();
    g.reduceClipRegion(juce::Rectangle<int>(headerWidth(), kRulerHeight, getWidth() - headerWidth(), getHeight() - kRulerHeight));
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    // A tile not yet drawn is drawn now; one drawn before a change of looks is
    // redrawn while this frame's budget lasts, and shown as it was until then.
    const double started = juce::Time::getMillisecondCounterHiRes();
    bool behind = false;
    for (long k = first; k <= last; ++k)
    {
        auto& tile = tiles_[k];
        const bool stale = tile.image.isValid() && tile.looks != looksVersion_;
        if (stale && juce::Time::getMillisecondCounterHiRes() - started > kRedrawBudgetMs)
            behind = true;
        else if (!tile.image.isValid() || stale)
        {
            if (!tile.image.isValid())
                tile.image = juce::Image(juce::Image::RGB, juce::roundToInt(kTileWidth * scale), juce::roundToInt(height * scale), false);
            juce::Graphics layer(tile.image);
            layer.addTransform(juce::AffineTransform::translation(-lanesX(), -static_cast<float>(kRulerHeight)).scaled(scale));
            const double view = viewStart_;
            viewStart_ = static_cast<double>(k) * span; // the tile starts at the lanes' left edge
            paintLaneLayer(layer, lanesX() + static_cast<float>(kTileWidth));
            viewStart_ = view;
            tile.looks = looksVersion_;
        }
        const float x = timeToX(static_cast<double>(k) * span) + shift;
        g.drawImageTransformed(tile.image, juce::AffineTransform::scale(1.0f / scale).translated(x, static_cast<float>(kRulerHeight)));
    }
    if (behind)
        repaint(); // the next frame goes on with the rest

    // The notes sounding now, over the rest with their glow.
    if (model_.playing())
    {
        g.addTransform(juce::AffineTransform::translation(shift, 0.0f));
        const double now = playhead();
        const auto& tracks = model_.tune().tracks;
        for (size_t t = 0; t < tracks.size(); ++t)
        {
            const int y = trackTop(static_cast<int>(t));
            if (y > getHeight() || y + trackHeight(static_cast<int>(t)) < kRulerHeight)
                continue;
            const auto& notes = tracks[t].notes;
            auto it = std::lower_bound(notes.begin(), notes.end(), now - static_cast<double>(layout().longest[t]),
                                       [](const TuneNote& n, double time) { return static_cast<double>(n.start) < time; });
            for (; it != notes.end() && static_cast<double>(it->start) <= now; ++it)
                if (isSounding(static_cast<int>(t), *it))
                {
                    const int i = static_cast<int>(it - notes.begin());
                    paintNote(g, static_cast<int>(t), i, noteBounds(static_cast<int>(t), i));
                }
        }
    }
    g.restoreState();
}

void TrackerView::paintLaneLayer(juce::Graphics& g, float right)
{
    const auto& tracks = model_.tune().tracks;
    const VgmSong& song = model_.tune().source();
    const double beat = song.beatLength;
    const double beatPixels = beat / samplesPerPixel_;
    const auto lanes = juce::Rectangle<float>(lanesX(), static_cast<float>(kRulerHeight), right - lanesX(), static_cast<float>(getHeight() - kRulerHeight));
    g.saveState();
    g.reduceClipRegion(lanes.getSmallestIntegerContainer());
    g.setColour(theme::lane);
    g.fillRect(lanes);

    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const int y = trackTop(static_cast<int>(t)), h = trackHeight(static_cast<int>(t));
        if (y > getHeight() || y + h < kRulerHeight)
            continue;
        g.setColour(layout().position[t] % 2 ? theme::lane : theme::laneAlt);
        g.fillRect(lanesX(), static_cast<float>(y), right - lanesX(), static_cast<float>(h));
        // Faint lines between a chord part's voices.
        g.setColour(theme::grid.withAlpha(0.3f));
        for (size_t k = 1; expanded(static_cast<int>(t)) && k < layout().voices[t].size(); ++k)
            g.fillRect(lanesX(), static_cast<float>(y + static_cast<int>(k) * laneHeight_), right - lanesX(), 1.0f);
        g.setColour(theme::grid.withAlpha(0.6f));
        g.fillRect(lanesX(), static_cast<float>(y + h - 1), right - lanesX(), 1.0f);
    }

    // Grid: bars bright, beats dim, sixteenths when there is room.
    const long first = static_cast<long>(std::floor((viewStart_ - song.firstBeat) / beat)) - 1;
    const long last = static_cast<long>(std::ceil((xToTime(right) - song.firstBeat) / beat));
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

    // The selection, a tinted band across every lane, with the song outside
    // it darkened; its edges go over the notes, below.
    if (model_.hasSelection())
    {
        const float x0 = timeToX(model_.selectionStart()), x1 = timeToX(model_.selectionEnd());
        const float top = static_cast<float>(kRulerHeight), height = static_cast<float>(getHeight() - kRulerHeight);
        g.setColour(theme::background.withAlpha(0.45f));
        g.fillRect(lanesX(), top, x0 - lanesX(), height);
        g.fillRect(x1, top, right - x1, height);
        g.setColour(theme::cyan.withAlpha(0.14f));
        g.fillRect(x0, top, x1 - x0, height);
    }

    // Notes in view, at rest.
    const double from = viewStart_, to = xToTime(right);
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
            paintNote(g, static_cast<int>(t), i, noteBounds(static_cast<int>(t), i), true);
        }
    }

    // The selection's edges, over the notes.
    if (model_.hasSelection())
    {
        g.setColour(theme::cyan);
        for (const double edge : { model_.selectionStart(), model_.selectionEnd() })
            g.fillRect(timeToX(edge) - 1.0f, static_cast<float>(kRulerHeight), 2.0f, static_cast<float>(getHeight() - kRulerHeight));
    }
    g.restoreState();
}

bool TrackerView::isSounding(int track, const TuneNote& n) const
{
    const double pos = playhead();
    const auto& alone = model_.playingNotes();
    return model_.playing() && pos >= static_cast<double>(n.start) && pos < static_cast<double>(n.start + n.length)
           && (alone.empty() || alone.count({ track, n.id }) > 0);
}

void TrackerView::paintNote(juce::Graphics& g, int track, int note, juce::Rectangle<float> r, bool resting)
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track)];
    const TuneNote& n = tr.notes[static_cast<size_t>(note)];
    const bool drum = tr.kind == TuneTrack::Kind::Drum;
    const int pitch = model_.tune().pitch(tr, n);
    const bool audible = model_.tune().audible(tr, n);
    const bool sounding = !resting && isSounding(track, n);
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
        for (const auto& [grow, alpha] : { std::pair { 7.0f, 0.06f }, std::pair { 4.0f, 0.12f }, std::pair { 2.0f, 0.25f } })
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
    if (selectedNotes_.count({ track, n.id }) > 0)
    {
        g.setColour(theme::cyan);
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 2.0f);
    }

    // A note the game bends or moves to new keys while held gets a line of
    // its pitch over time, from its lowest pitch at the bottom of the note to
    // its highest at the top, over at least 2 semitones. A pitch that moves
    // less than a quarter of a semitone would draw nearly flat, so it gets none.
    const auto bends = showBends_ ? model_.tune().bends(tr, n) : std::vector<std::pair<int64_t, double>> {};
    const auto [low, high] = std::minmax_element(bends.begin(), bends.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (bends.size() > 1 && high->second - low->second >= 0.25 && r.getHeight() >= 10.0f)
    {
        const double span = std::max(2.0, high->second - low->second);
        const double middle = (low->second + high->second) * 0.5;
        const auto area = r.reduced(1.0f, 3.0f);
        auto y = [&](double p) { return area.getCentreY() - static_cast<float>((p - middle) / span) * area.getHeight(); };
        juce::Path line;
        line.startNewSubPath(area.getX(), y(bends.front().second));
        // A slide, in steps under a semitone less than 100 ms apart, is a ramp
        // from one change to the next. A jump of a semitone or more, or a
        // change after a held pitch, is a step.
        for (size_t i = 1; i < bends.size(); ++i)
        {
            const float x = juce::jlimit(area.getX(), area.getRight(), timeToX(static_cast<double>(bends[i].first)));
            const bool slide = std::abs(bends[i].second - bends[i - 1].second) < 1.0 && static_cast<double>(bends[i].first - bends[i - 1].first) < kVgmRate / 10;
            if (!slide)
                line.lineTo(x, y(bends[i - 1].second));
            line.lineTo(x, y(bends[i].second));
        }
        line.lineTo(area.getRight(), y(bends.back().second));
        g.setColour(juce::Colours::white.withAlpha(audible ? (sounding ? 0.6f : 0.35f) : 0.12f));
        g.strokePath(line, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The label, centred and as large as the note allows: white with a dark
    // outline, so it reads on every colour. Notes too small for it go without.
    const juce::String label = drum ? (n.transpose == 0 ? juce::String() : (n.transpose > 0 ? "+" : "") + juce::String(n.transpose))
                                    : theme::noteName(pitch).removeCharacters("-");
    const float height = std::min({ 26.0f, r.getHeight() * 0.62f, (r.getWidth() - 6.0f) / (0.64f * static_cast<float>(label.length())) });
    if (label.isNotEmpty() && height >= 9.0f)
    {
        // The text and its outline are made once per label and size, then
        // moved into place: building them is most of the cost of a frame.
        const auto& shapes = labelShapes(label, height);
        const auto at = juce::AffineTransform::translation(r.getCentreX() - shapes.text.getBounds().getCentreX(),
                                                           r.getCentreY() - shapes.text.getBounds().getCentreY());
        if (audible)
        {
            g.setColour(theme::background.withAlpha(0.7f));
            g.fillPath(shapes.outline, at);
        }
        g.setColour(juce::Colours::white.withAlpha(audible ? (sounding ? 1.0f : 0.7f) : 0.3f));
        g.fillPath(shapes.text, at);
    }
}

void TrackerView::paintHeaders(juce::Graphics& g)
{
    const auto& tracks = model_.tune().tracks;
    g.setColour(theme::panel);
    g.fillRect(0, 0, headerWidth(), getHeight());
    g.setColour(theme::gridBar);
    g.fillRect(headerWidth() - 1, 0, 1, getHeight());

    g.saveState();
    g.reduceClipRegion(0, kRulerHeight, headerWidth(), getHeight() - kRulerHeight);
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const TuneTrack& tr = tracks[t];
        const int y = trackTop(static_cast<int>(t));
        if (y > getHeight() || y + trackHeight(static_cast<int>(t)) < kRulerHeight)
            continue;
        const auto row = juce::Rectangle<int>(0, y, headerWidth() - 1, trackHeight(static_cast<int>(t)));
        g.setColour(layout().position[t] % 2 ? theme::panel : theme::panelLight);
        g.fillRect(row);

        // The chip that plays the track, in its own column, drawn as a chip
        // with pins down both sides and its name up the middle: OPM for the
        // YM2151's FM voices, PCM for the SegaPCM's samples.
        {
            const auto column = juce::Rectangle<int>(0, row.getY(), kChipColumn, row.getHeight());
            g.setColour(theme::background.withAlpha(0.35f));
            g.fillRect(column);
            g.setColour(theme::grid);
            g.fillRect(column.getRight() - 1, column.getY(), 1, column.getHeight());

            const bool fm = tr.kind == TuneTrack::Kind::Fm;
            const juce::Colour ink = (fm ? theme::cyan : theme::orange).withAlpha(model_.tune().audible(tr) ? 0.9f : 0.35f);
            const auto firstRow = column.withHeight(std::min(column.getHeight(), laneHeight_)).toFloat();
            const auto body = firstRow.withSizeKeepingCentre(11.0f, std::min(30.0f, firstRow.getHeight() - 8.0f));
            g.setColour(ink);
            if (body.getHeight() < 22.0f)
            {
                // Too short a lane for the chip: its name alone, across.
                g.setFont(theme::pixel(6.0f));
                g.drawText(fm ? "OPM" : "PCM", firstRow, juce::Justification::centred, false);
            }
            else
            {
                for (float pin = body.getY() + 3.0f; pin < body.getBottom() - 2.0f; pin += 3.5f)
                {
                    g.fillRect(body.getX() - 2.5f, pin, 2.5f, 1.2f);
                    g.fillRect(body.getRight(), pin, 2.5f, 1.2f);
                }
                g.setColour(theme::background);
                g.fillRoundedRectangle(body, 2.0f);
                g.setColour(ink);
                g.drawRoundedRectangle(body.reduced(0.5f), 2.0f, 1.0f);
                g.fillEllipse(body.getX() + 2.0f, body.getY() + 2.0f, 1.5f, 1.5f); // pin 1's mark

                // The name, reading upwards.
                juce::GlyphArrangement glyphs;
                glyphs.addLineOfText(theme::pixel(5.0f), fm ? "OPM" : "PCM", 0.0f, 0.0f);
                juce::Path text;
                glyphs.createPath(text);
                const auto bounds = text.getBounds();
                text.applyTransform(juce::AffineTransform::translation(-bounds.getCentreX(), -bounds.getCentreY())
                                        .rotated(-juce::MathConstants<float>::halfPi)
                                        .translated(body.getCentreX(), body.getCentreY() + 1.0f));
                g.fillPath(text);
            }
        }

        const int ti = static_cast<int>(t);
        const auto& voices = layout().voices[t];
        auto drawToggle = [&](juce::Rectangle<int> r, const juce::String& label, bool on, juce::Colour colour) {
            g.setColour(on ? colour : theme::background.withAlpha(0.7f));
            g.fillRoundedRectangle(r.toFloat(), 4.0f);
            g.setColour(on ? colour : theme::gridBar.brighter(0.3f));
            g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 4.0f, 1.0f);
            g.setColour(on ? theme::background : theme::text.withAlpha(0.85f));
            g.setFont(theme::mono(r.getHeight() < 17 ? 10.0f : 12.0f, true));
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
        g.setFont(theme::mono(minimal() ? 11.0f : 13.0f, true));
        // The name, then right after it the tag, dimmer: the patch the voice
        // plays, or in patch order the voice. Both stay in the smallest lanes.
        const auto nameArea = nameBounds(ti);
        const juce::String head = headName(tr, byPatch_);
        g.drawText(head, nameArea, juce::Justification::centredLeft, true);
        if (tr.kind == TuneTrack::Kind::Fm)
        {
            const int tagX = nameArea.getX() + juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), head) + 8;
            g.setColour(audible ? theme::dim : theme::faint);
            g.setFont(theme::mono(minimal() ? 11.0f : 12.0f, true));
            g.drawText(tagName(tr, byPatch_), juce::Rectangle<int>(tagX, nameArea.getY(), std::max(0, nameArea.getRight() - tagX), nameArea.getHeight()),
                       juce::Justification::centredLeft, true);
        }
        drawToggle(mute, "M", tr.mute, theme::orange);
        drawToggle(pill(ti, 1), "S", tr.solo, theme::cyan);

        // The level in dB, under the knob; compact lanes go without both and the meter.
        const bool dragging = drag_ == Drag::Knob && dragTrack_ == ti;
        const bool silent = !audible || tr.volume <= TuneTrack::kSilent;
        if (!compact())
        {
            const juce::String db = tr.volume <= TuneTrack::kSilent ? juce::String("-inf dB")
                                    : (tr.volume > 0.05f ? "+" : "") + juce::String(tr.volume, 1) + " dB";
            g.setColour(dragging ? theme::cyan : silent ? theme::faint
                        : std::abs(tr.volume) < 0.05f ? theme::dim : theme::text);
            g.setFont(theme::mono(10.0f, true));
            const auto knob = knobBounds(ti);
            g.drawText(db, juce::Rectangle<int>(knob.getCentreX() - kKnobColumn / 2, knob.getBottom() - 1, kKnobColumn, 13), juce::Justification::centred);
        }

        // The meter: a dark groove of cells, always showing dimmed and lit up
        // to the level, with a peak hold. As on a DAW's meter, cells are white
        // up to -12 dB, yellow to -6, orange to -3 and red above.
        if (!compact())
        {
            const auto groove = meterBounds(ti).toFloat();
            // A silent track's whole meter goes neutral grey.
            g.setColour(silent ? juce::Colour(0xff1d1e24) : juce::Colour(0xff1a1c24));
            g.fillRoundedRectangle(groove, 3.0f);
            g.setColour(silent ? juce::Colour(0xff3a3b44) : theme::gridBar);
            g.drawRoundedRectangle(groove, 3.0f, 1.0f);
            const auto cellsArea = groove.reduced(2.0f, 2.0f);
            const float level = t < level_.size() ? level_[t] : 0.0f;
            const float hold = t < hold_.size() ? hold_[t] : 0.0f;
            const int cells = static_cast<int>(cellsArea.getWidth() / 3.0f);
            const float cell = cellsArea.getWidth() / static_cast<float>(cells);
            const int held = static_cast<int>(hold * static_cast<float>(cells));
            auto zone = [](float db) { return (db - kMeterFloor) / -kMeterFloor; }; // where a level in dB sits along the meter
            for (int k = 0; k < cells; ++k)
            {
                const float frac = (static_cast<float>(k) + 0.5f) / static_cast<float>(cells);
                const juce::Colour c = silent ? juce::Colour(0xff7a7b86) : frac > zone(-3.0f) ? theme::danger : frac > zone(-6.0f) ? theme::orange
                                       : frac > zone(-12.0f) ? theme::yellow : juce::Colour(0xffeceaf4);
                const bool on = frac <= level || (k == held && hold > 0.02f);
                g.setColour(on ? c : silent ? juce::Colour(0xff2c2d33) : c.withMultipliedSaturation(0.8f).withMultipliedBrightness(0.32f));
                g.fillRect(cellsArea.getX() + static_cast<float>(k) * cell, cellsArea.getY(), cell - 1.0f, cellsArea.getHeight());
            }
        }

        // The volume knob: a ring from silence to +6 dB lit up to the level,
        // a tick at 0 dB, and a raised cap with a pointer.
        if (!compact())
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
            const int x = headerWidth() - kVoiceColumn;
            g.setColour(theme::background.withAlpha(0.35f));
            g.fillRect(x, row.getY(), headerWidth() - 1 - x, row.getHeight());
            g.setColour(theme::grid.withAlpha(0.5f));
            for (size_t k = 1; k < voices.size(); ++k)
                g.fillRect(x, row.getY() + static_cast<int>(k) * laneHeight_, headerWidth() - 1 - x, 1);
            const auto top = voicePill(ti, 0), bottom = voicePill(ti, static_cast<int>(voices.size()) - 1);
            g.setColour(theme::dim.withAlpha(0.45f));
            g.fillRect(x + 1, top.getCentreY(), 1, bottom.getCentreY() - top.getCentreY());
            for (size_t k = 0; k < voices.size(); ++k)
            {
                const auto r = voicePill(ti, static_cast<int>(k));
                g.setColour(theme::dim.withAlpha(0.45f));
                g.fillRect(x + 1, r.getCentreY(), r.getX() - x - 1, 1);
                drawToggle(r, "V" + juce::String(voices[k] + 1), (tr.mutedVoices >> voices[k]) & 1, theme::orange);
            }
        }
    }
    g.restoreState();

    // Corner over the headers.
    g.setColour(theme::panel);
    g.fillRect(0, 0, headerWidth(), kRulerHeight);
    g.setColour(theme::gridBar);
    g.fillRect(0, kRulerHeight - 1, headerWidth(), 1);
    g.setColour(theme::magenta);
    g.setFont(theme::pixel(8.0f));
    g.drawText("TRACKS " + juce::String(tracks.size()).paddedLeft('0', 2), juce::Rectangle<int>(14, 0, headerWidth(), kRulerHeight),
               juce::Justification::centredLeft);
}

// Mouse -----------------------------------------------------------------------

int TrackerView::selectionEdgeAt(juce::Point<float> p) const
{
    if (!model_.hasSelection() || p.x < lanesX())
        return -1;
    const float start = timeToX(model_.selectionStart()), end = timeToX(model_.selectionEnd());
    const float ds = std::abs(p.x - start), de = std::abs(p.x - end);
    if (std::min(ds, de) > 5.0f)
        return -1;
    return ds < de ? 0 : 1;
}

bool TrackerView::overSelectionBar(juce::Point<float> p) const
{
    if (model_.playing() || !model_.hasSelection() || p.y >= kRulerHeight || p.x < lanesX())
        return false;
    const double t = xToTime(p.x);
    return t > model_.selectionStart() && t < model_.selectionEnd();
}

int TrackerView::linkAt(juce::Point<float> p) const
{
    for (size_t i = 0; i < links_.size(); ++i)
        if (links_[i].first.contains(p))
            return static_cast<int>(i);
    return -1;
}

void TrackerView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (!model_.hasTune())
    {
        // A link opens in the browser: a song's, its download.
        if (const int link = linkAt(e.position); link >= 0)
            juce::URL(links_[static_cast<size_t>(link)].second).launchInDefaultBrowser();
        return;
    }
    const auto p = e.position;
    dragFrom_ = p;

    // A selection's edge, over the ruler or the lanes, drags out or in.
    if (const int edge = selectionEdgeAt(p); edge >= 0)
    {
        drag_ = Drag::SelectEdge;
        selectFrom_ = edge == 0 ? model_.selectionEnd() : model_.selectionStart();
        return;
    }

    // Stopped, the ruler inside the selection: a drag moves it, a click the playhead.
    if (overSelectionBar(p))
    {
        drag_ = Drag::SelectMove;
        selectFrom_ = xToTime(p.x) - model_.selectionStart();
        return;
    }

    if (p.y < kRulerHeight && p.x >= lanesX())
    {
        drag_ = Drag::Ruler;
        model_.setPosition(std::max(0.0, xToTime(p.x)));
        return;
    }

    const int t = yToTrack(p.y);
    if (p.x < lanesX())
    {
        if (t < 0)
            return;
        // The knob turns as the pointer goes up or right; Option-click puts it back to 0 dB.
        if (!compact() && knobBounds(t).expanded(3).contains(p.toInt()))
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

    // A click on a note, not a drag, selects it (see mouseUp).
    pressedNote_ = noteAt(p);

    // Stopped, a drag selects a section, and a click moves the playhead there.
    if (!model_.playing() && !e.mods.isShiftDown())
    {
        drag_ = Drag::Select;
        selectFrom_ = std::max(0.0, xToTime(p.x));
        return;
    }
    // Playing, or with Shift: a click moves the playhead there; held and
    // dragged, the hand moves the view across and up or down.
    drag_ = Drag::Lanes;
    panFrom_ = viewStart_;
    panFromY_ = scrollY_;
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
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
    if (drag_ == Drag::SelectMove)
    {
        if (e.getDistanceFromDragStart() > 3)
        {
            const double length = model_.selectionEnd() - model_.selectionStart();
            const double start = std::max(0.0, xToTime(p.x) - selectFrom_);
            model_.setSelection(start, start + length);
        }
        return;
    }
    if (drag_ == Drag::SelectEdge)
    {
        // Stopped, the playhead goes with the edge while it is the start;
        // playing, it stays unless it falls outside.
        const double t = std::max(0.0, xToTime(p.x));
        model_.setSelection(selectFrom_, t, model_.playing() || t > selectFrom_);
        return;
    }
    if (drag_ == Drag::Select && e.getDistanceFromDragStart() > 3)
    {
        model_.setSelection(selectFrom_, std::max(0.0, xToTime(p.x)));
        return;
    }
    if (drag_ == Drag::Lanes && e.getDistanceFromDragStart() > 3)
        drag_ = Drag::Pan;
    if (drag_ == Drag::Pan)
    {
        setViewStart(panFrom_ - (p.x - dragFrom_.x) * samplesPerPixel_);
        setScrollY(panFromY_ - static_cast<int>(p.y - dragFrom_.y));
        movedByHand();
    }
}

void TrackerView::mouseUp(const juce::MouseEvent& e)
{
    const Drag finished = drag_;
    drag_ = Drag::None;
    // A click on the lanes puts the playhead there, and playing goes on
    // from it; stopped, it also drops the selection.
    if (finished == Drag::SelectMove && e.getDistanceFromDragStart() <= 3)
        model_.setPosition(std::max(0.0, xToTime(e.position.x)));
    // A click on a note selects it, for Space or the play button to play
    // alone; Shift adds or removes it.
    const bool click = (finished == Drag::Lanes || finished == Drag::Select) && e.getDistanceFromDragStart() <= 3;
    const NoteRef pressed = std::exchange(pressedNote_, {});
    if (click && pressed.valid())
    {
        clickNote(pressed, e.mods.isShiftDown());
        mouseMove(e); // restores the cursor, which a press while playing turns into a hand
        return;
    }
    // A plain click off the notes, or a drag that selects a section, drops
    // the selected notes.
    const bool section = finished == Drag::Select && !click;
    if ((section || (click && !e.mods.isShiftDown())) && !selectedNotes_.empty())
        selectNotes({});
    if (finished == Drag::Lanes || (finished == Drag::Select && e.getDistanceFromDragStart() <= 3))
    {
        if (finished == Drag::Select)
            model_.clearSelection();
        model_.setPosition(std::max(0.0, xToTime(e.position.x)));
    }
    if (finished == Drag::Lanes || finished == Drag::Pan)
        mouseMove(e);
    repaint();
}

void TrackerView::clickNote(NoteRef note, bool add)
{
    std::set<NoteRef> notes = add ? selectedNotes_ : std::set<NoteRef> {};
    if (add && notes.count(note) > 0)
        notes.erase(note);
    else
        notes.insert(note);
    selectNotes(std::move(notes));
}

void TrackerView::selectNotes(std::set<NoteRef> notes)
{
    // Selected notes replace a section, even one they are in.
    if (!notes.empty() && model_.hasSelection())
        model_.clearSelection();
    selectedNotes_ = std::move(notes);
    selectedFile_ = model_.file();
    ++selectedRevision_;
    std::set<std::pair<int, int>> chosen;
    for (const NoteRef& n : selectedNotes_)
        chosen.insert({ n.track, n.id });
    model_.setSelectedNotes(chosen);
    repaint();
}

void TrackerView::mouseMove(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
    {
        setMouseCursor(linkAt(e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }
    hover_ = noteAt(e.position);
    setMouseCursor(selectionEdgeAt(e.position) >= 0                          ? juce::MouseCursor::LeftRightResizeCursor
                   : overSelectionBar(e.position)                            ? juce::MouseCursor::DraggingHandCursor
                   : e.position.y < kRulerHeight && e.position.x > lanesX() ? juce::MouseCursor::PointingHandCursor
                                                                            : juce::MouseCursor::NormalCursor);
    if (hover_.valid())
    {
        const int i = indexOf(hover_);
        const auto& tr = model_.tune().tracks[static_cast<size_t>(hover_.track)];
        const auto& n = tr.notes[static_cast<size_t>(i)];
        // "C-4 · 123.45 ms": the dot spaced by one, the length to two decimals.
        setTooltip(tr.kind == TuneTrack::Kind::Fm ? theme::noteName(model_.tune().pitch(tr, n)) + juce::String::fromUTF8(" \xc2\xb7 ")
                                                        + juce::String(static_cast<double>(n.length) / kVgmRate * 1000.0, 2) + " ms"
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
    if (model_.hasTune() && e.position.x < lanesX())
        if (const int t = yToTrack(e.position.y); t >= 0 && !compact() && knobBounds(t).expanded(3).contains(e.position.toInt()))
        {
            dragStarted_ = false;
            setVolume(t, 0.0f);
        }
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
    // It scrolls: sideways through time, up and down through tracks.
    const float dx = std::abs(w.deltaX) > 0 ? w.deltaX : (e.mods.isShiftDown() ? w.deltaY : 0.0f);
    const float dy = e.mods.isShiftDown() ? 0.0f : w.deltaY;
    if (std::abs(dx) > 0)
    {
        setViewStart(viewStart_ - dx * 400.0 * samplesPerPixel_);
        movedByHand();
    }
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

// Keys ------------------------------------------------------------------------

// Undo and redo take back mute, solo and volume, and Escape drops the
// selection; the notes cannot be edited.
bool TrackerView::keyPressed(const juce::KeyPress& key)
{
    wake();
    if (!model_.hasTune())
        return false;
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0))
        return model_.undo().undo(), true;
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return model_.undo().redo(), true;
    if (key == juce::KeyPress::escapeKey && !selectedNotes_.empty())
        return selectNotes({}), true;
    if (key == juce::KeyPress::escapeKey && model_.hasSelection())
        return model_.clearSelection(), true;
    return false;
}

} // namespace opm86
