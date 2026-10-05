// The tracker's piano roll: one track's notes by pitch, with a lane for
// their velocity, fine tune or pan under them. It shares the arrangement's
// time axis, ruler, selection, clipboard and note commands.

#include "TrackerView.h"

#include "Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

namespace opm86
{

namespace
{

constexpr int kLaneMargin = 6;      // pixels above and below the lane's values
constexpr int kMinLaneOpen = 64;    // the lane's least height open, tabs and all
constexpr float kBarReach = 5.0f;   // pixels either side of a bar that grab it

bool blackKey(int key)
{
    const int pc = ((key % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// A number with up to two decimals, the zeros after the point left out.
juce::String number(double v)
{
    juce::String s(v, 2);
    if (s.containsChar('.'))
        s = s.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return s == "-0" ? juce::String("0") : s;
}

void sortByStart(std::vector<TuneNote>& notes)
{
    std::stable_sort(notes.begin(), notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
}

// Where a note sits in the rows: on the key its label names, a fraction of
// a row up or down for its fine tune.
double rollPitch(const Tune& tune, const TuneTrack& track, const TuneNote& note)
{
    TuneNote key = note;
    key.fine = 0;
    return tune.pitch(track, key) + note.fine / 64.0;
}

} // namespace

// Entering and leaving -------------------------------------------------------

void TrackerView::setPianoRoll(int track)
{
    if (track >= 0 && (!model_.hasTune() || static_cast<size_t>(track) >= model_.tune().tracks.size()))
        return;
    if (track == rollTrack_)
        return;
    const bool opening = rollTrack_ < 0;
    holdKey(-1);
    drag_ = Drag::None;
    hover_ = {};
    laneValues_.clear();
    rollClick_ = false;
    rollTrack_ = track;
    if (track >= 0)
    {
        lastTrack_ = track;
        dropHiddenSelection();
        fitRoll(false);
    }
    setMouseCursor(juce::MouseCursor::NormalCursor);
    setTooltip({});
    repaint();
    sendChangeMessage();
    // Only on opening or closing, not on moving to another track.
    if (onPianoRoll && opening != (track < 0))
        onPianoRoll(track >= 0);
}

void TrackerView::songOpened()
{
    setPianoRoll(-1);
    lastTrack_ = -1;
}

int TrackerView::pianoRollTarget() const
{
    if (!model_.hasTune() || model_.tune().tracks.empty())
        return -1;
    if (rollTrack_ >= 0)
        return rollTrack_;
    if (const int t = selectedTrack(); t >= 0)
        return t;
    // With nothing selected or clicked yet, the first track.
    return lastTrack_ >= 0 && static_cast<size_t>(lastTrack_) < model_.tune().tracks.size() ? lastTrack_ : 0;
}

void TrackerView::dropHiddenSelection()
{
    if (rollTrack_ < 0)
        return;
    for (auto it = selection_.begin(); it != selection_.end();)
        it = it->track != rollTrack_ ? selection_.erase(it) : std::next(it);
}

// Geometry --------------------------------------------------------------------

int TrackerView::lanesTop() const
{
    const int room = getHeight() - rollTop();
    const int open = std::clamp(laneOpenHeight_, kMinLaneOpen, std::max(kMinLaneOpen, room / 2));
    return getHeight() - (laneOpen_ ? open : kLaneTabs);
}

float TrackerView::keyTop(double pitch) const
{
    return static_cast<float>(rollTop() - rollScroll_ + (127.0 - pitch) * rowHeight_);
}

int TrackerView::keyAt(float y) const
{
    const int row = static_cast<int>(std::floor((y - static_cast<float>(rollTop() - rollScroll_)) / static_cast<float>(rowHeight_)));
    return std::clamp(127 - row, 0, 127);
}

void TrackerView::setRollScroll(int y)
{
    const int room = lanesTop() - rollTop();
    rollScroll_ = std::clamp(y, 0, std::max(0, 128 * rowHeight_ - room));
    repaint();
}

void TrackerView::fitRoll(bool resize)
{
    if (rollTrack_ < 0)
        return;
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    double low = 127.0, high = 0.0;
    for (const TuneNote& n : tr.notes)
    {
        const double p = rollPitch(model_.tune(), tr, n);
        low = std::min(low, p);
        high = std::max(high, p);
    }
    // A track with no notes yet shows C3 to C5.
    if (low > high)
        low = 48.0, high = 72.0;
    low = std::floor(low), high = std::ceil(high);
    // Two rows to spare above and below.
    const double rows = high - low + 5.0;
    const int room = lanesTop() - rollTop();
    if (room > 0 && resize)
    {
        rowHeightExact_ = std::clamp(std::floor(room / rows), static_cast<double>(kMinRowHeight), static_cast<double>(kMaxRowHeight));
        rowHeight_ = static_cast<int>(rowHeightExact_);
    }
    const double middle = (low + high) * 0.5;
    setRollScroll(static_cast<int>(std::lround((127.0 - middle + 0.5) * rowHeight_ - room * 0.5)));
}

void TrackerView::zoomRows(double factor, float y)
{
    // The pitch under the pointer stays where it is; from a command, the middle's.
    if (y < static_cast<float>(rollTop()) || y >= static_cast<float>(lanesTop()))
        y = static_cast<float>(rollTop() + lanesTop()) * 0.5f;
    const double anchor = y - static_cast<float>(rollTop());
    const double content = (anchor + rollScroll_) / rowHeight_;
    rowHeightExact_ = std::clamp(rowHeightExact_ * factor, static_cast<double>(kMinRowHeight), static_cast<double>(kMaxRowHeight));
    rowHeight_ = static_cast<int>(std::lround(rowHeightExact_));
    setRollScroll(static_cast<int>(std::lround(content * rowHeight_ - anchor)));
}

juce::Rectangle<float> TrackerView::rollNoteBounds(int note) const
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    const TuneNote& n = tr.notes[static_cast<size_t>(note)];
    const float top = keyTop(rollPitch(model_.tune(), tr, n));
    const float x0 = timeToX(static_cast<double>(n.start));
    const float x1 = timeToX(static_cast<double>(n.start + n.length));
    return { x0, top + 1.0f, std::max(2.0f, x1 - x0), std::max(2.0f, static_cast<float>(rowHeight_) - 2.0f) };
}

TrackerView::NoteRef TrackerView::rollNoteAt(juce::Point<float> p, bool& onRightEdge) const
{
    onRightEdge = false;
    if (p.x < lanesX() || p.y < static_cast<float>(rollTop()) || p.y >= static_cast<float>(lanesTop()))
        return {};
    const auto& notes = model_.tune().tracks[static_cast<size_t>(rollTrack_)].notes;
    const double time = xToTime(p.x);
    const auto longest = static_cast<double>(layout().longest[static_cast<size_t>(rollTrack_)]);
    // The note under the pointer that starts last, so the shorter one on top wins.
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
    {
        const TuneNote& n = notes[static_cast<size_t>(i)];
        if (static_cast<double>(n.start) > time + kEdge * samplesPerPixel_)
            continue;
        if (static_cast<double>(n.start + n.length) + longest < time)
            break;
        const auto r = rollNoteBounds(i);
        if (r.expanded(1.0f, 0.0f).contains(p) || (p.x >= r.getRight() - kEdge && p.x <= r.getRight() + 2 && r.getY() <= p.y && p.y <= r.getBottom()))
        {
            onRightEdge = p.x >= r.getRight() - kEdge;
            return { rollTrack_, n.id };
        }
    }
    return {};
}

juce::Rectangle<int> TrackerView::rollButton(int track) const
{
    // In the piano roll, at the right of its header's name; else after the
    // bin, in a track's label strip or its only row.
    if (rollTrack_ >= 0)
        return { headerWidth() - 8 - 17, kRulerHeight + (kRollHeader - 17) / 2, 17, 17 };
    return removeTrackButton(track).translated(17 + 4, 0);
}

bool TrackerView::overLaneEdge(float y) const
{
    // A few pixels over the edge and up to the tabs below it, while the lane is open.
    const auto top = static_cast<float>(lanesTop());
    return laneOpen_ && y >= top - 4.0f && y < top + 3.0f;
}

juce::Rectangle<int> TrackerView::laneArea() const
{
    const int top = lanesTop() + kLaneTabs;
    return juce::Rectangle<int>(headerWidth(), top, getWidth() - headerWidth(), getHeight() - top).reduced(0, kLaneMargin);
}

juce::Rectangle<int> TrackerView::laneTab(Lane lane) const
{
    // Side by side over the rows, from the left.
    int x = headerWidth() + 8;
    for (const Lane l : { Lane::Velocity, Lane::Fine, Lane::Pan })
    {
        const juce::String name = l == Lane::Velocity ? "VELOCITY" : l == Lane::Fine ? "FINE" : "PAN";
        const int width = juce::GlyphArrangement::getStringWidthInt(theme::mono(11.0f, true), name) + 18;
        if (l == lane)
            return { x, lanesTop() + 3, width, kLaneTabs - 5 };
        x += width + 4;
    }
    return {};
}

juce::Rectangle<int> TrackerView::laneChevron() const
{
    // Where the strips under the tracks have theirs.
    return { headerWidth() - 36, lanesTop() + (kLaneTabs - 16) / 2 + 1, 24, 16 };
}

// Velocity from 0 dB at the top to -48 dB at the bottom; fine tune and pan
// round the middle, up for sharp or right.
float TrackerView::laneY(int value) const
{
    const auto a = laneArea().toFloat();
    if (lane_ == Lane::Velocity)
        return a.getY() + static_cast<float>(-value) / static_cast<float>(-TuneNote::kMinVelocity) * a.getHeight();
    return a.getCentreY() - static_cast<float>(value) / static_cast<float>(TuneNote::kPanRange) * a.getHeight() * 0.5f;
}

int TrackerView::laneValue(float y, bool fine) const
{
    const auto a = laneArea().toFloat();
    const bool drum = !fmTrack(rollTrack_);
    auto stepped = [fine](float raw, int coarse) {
        const int step = fine ? 1 : coarse;
        return static_cast<int>(std::lround(raw / static_cast<float>(step))) * step;
    };
    if (lane_ == Lane::Velocity)
    {
        // Coarse steps of 3 dB, fine of 0.75 dB.
        const float raw = -(y - a.getY()) / std::max(1.0f, a.getHeight()) * static_cast<float>(-TuneNote::kMinVelocity);
        return std::clamp(stepped(raw, 4), TuneNote::kMinVelocity, 0);
    }
    const float half = (a.getCentreY() - y) / std::max(1.0f, a.getHeight() * 0.5f);
    if (lane_ == Lane::Fine)
        return std::clamp(stepped(half * 64.0f, 8), -TuneNote::kMaxFine, TuneNote::kMaxFine);
    // An FM note's pan is one of three sides; a drum hit's, a balance.
    if (!drum)
        return half > 1.0f / 3.0f ? TuneNote::kPanRange : half < -1.0f / 3.0f ? -TuneNote::kPanRange : 0;
    return std::clamp(stepped(half * static_cast<float>(TuneNote::kPanRange), 8), -TuneNote::kPanRange, TuneNote::kPanRange);
}

int TrackerView::noteLaneValue(const TuneNote& note) const
{
    if (const auto it = laneValues_.find(note.id); it != laneValues_.end())
        return it->second;
    return lane_ == Lane::Velocity ? note.velocity : lane_ == Lane::Fine ? note.fine : note.pan.value_or(0);
}

int TrackerView::laneBarAt(float x) const
{
    const auto& notes = model_.tune().tracks[static_cast<size_t>(rollTrack_)].notes;
    const double from = xToTime(x - kBarReach), to = xToTime(x + kBarReach);
    auto it = std::lower_bound(notes.begin(), notes.end(), from, [](const TuneNote& n, double time) { return static_cast<double>(n.start) < time; });
    // Of the bars at the nearest start, as a chord's are, a selected one.
    int best = -1;
    bool bestSelected = false;
    float nearest = kBarReach + 1.0f;
    for (; it != notes.end() && static_cast<double>(it->start) <= to; ++it)
    {
        const float d = std::abs(timeToX(static_cast<double>(it->start)) - x);
        const bool selected = selection_.count({ rollTrack_, it->id }) > 0;
        if (d < nearest - 0.01f || (d < nearest + 0.01f && selected && !bestSelected))
            nearest = std::min(nearest, d), best = static_cast<int>(it - notes.begin()), bestSelected = selected;
    }
    return best;
}

// Painting --------------------------------------------------------------------

void TrackerView::paintRollIcon(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour colour)
{
    const auto c = box.getCentre();
    juce::Path icon;
    icon.addRoundedRectangle(c.x - 7.0f, c.y - 6.0f, 14.0f, 12.0f, 2.0f);
    icon.addRoundedRectangle(c.x - 5.8f, c.y - 4.8f, 11.6f, 9.6f, 1.2f);
    icon.addRoundedRectangle(c.x - 4.2f, c.y + 1.4f, 4.0f, 1.8f, 0.6f);
    icon.addRoundedRectangle(c.x - 1.6f, c.y - 1.0f, 3.6f, 1.8f, 0.6f);
    icon.addRoundedRectangle(c.x + 0.8f, c.y - 3.4f, 3.4f, 1.8f, 0.6f);
    icon.setUsingNonZeroWinding(false);
    g.setColour(colour);
    g.fillPath(icon);
}

void TrackerView::paintRoll(juce::Graphics& g)
{
    const Tune& tune = model_.tune();
    const auto& tracks = tune.tracks;
    const TuneTrack& tr = tracks[static_cast<size_t>(rollTrack_)];
    const int top = rollTop(), bottom = lanesTop();
    const auto rows = juce::Rectangle<int>(headerWidth(), top, getWidth() - headerWidth(), bottom - top);
    const auto rh = static_cast<float>(rowHeight_);
    g.saveState();
    g.reduceClipRegion(rows);

    // A row per pitch, the black keys' darker, a faint line between rows
    // with room for it and a bright one under each C.
    const int highest = keyAt(static_cast<float>(top)), lowest = keyAt(static_cast<float>(bottom - 1));
    const juce::Colour blackRow = theme::lane.interpolatedWith(theme::background, 0.5f);
    for (int k = lowest; k <= highest; ++k)
    {
        const float y = keyTop(k);
        g.setColour(blackKey(k) ? blackRow : theme::laneAlt);
        g.fillRect(juce::Rectangle<float>(static_cast<float>(rows.getX()), y, static_cast<float>(rows.getWidth()), rh));
        if (k % 12 == 0 || rowHeight_ >= 10)
        {
            g.setColour(k % 12 == 0 ? theme::gridBar : theme::grid.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(static_cast<float>(rows.getX()), y + rh - 1.0f, static_cast<float>(rows.getWidth()), 1.0f));
        }
    }
    paintGrid(g, top, bottom);

    const double from = viewStart_, to = xToTime(static_cast<float>(getWidth()));
    auto firstInView = [from](const std::vector<TuneNote>& notes, int64_t longest) {
        return std::lower_bound(notes.begin(), notes.end(), from - static_cast<double>(longest),
                                [](const TuneNote& n, double time) { return static_cast<double>(n.start) < time; });
    };

    // The other tracks of the same chip, faint outlines behind the track's notes.
    if (showGhosts_)
        for (size_t t = 0; t < tracks.size(); ++t)
        {
            if (static_cast<int>(t) == rollTrack_ || tracks[t].kind != tr.kind)
                continue;
            const auto& notes = tracks[t].notes;
            for (auto it = firstInView(notes, layout().longest[t]); it != notes.end() && static_cast<double>(it->start) <= to; ++it)
            {
                if (static_cast<double>(it->start + it->length) < from)
                    continue;
                const float y = keyTop(rollPitch(tune, tracks[t], *it));
                if (y > static_cast<float>(bottom) || y + rh < static_cast<float>(top))
                    continue;
                const float x0 = timeToX(static_cast<double>(it->start)), x1 = timeToX(static_cast<double>(it->start + it->length));
                const auto r = juce::Rectangle<float>(x0, y + 1.0f, std::max(2.0f, x1 - x0 - 1.0f), std::max(2.0f, rh - 2.0f));
                const juce::Colour c = tr.kind == TuneTrack::Kind::Drum ? theme::instrumentColour(static_cast<int>(t)) : theme::pitchColour(tune.pitch(tracks[t], *it));
                g.setColour(c.withAlpha(tune.audible(tracks[t], *it) ? 0.4f : 0.15f));
                g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
            }
        }

    // The track's notes in view, at their pitch; while dragging, the
    // selected ones where they will land. A track with more than one voice
    // tags each note with its own.
    const bool tagged = layout().voices[static_cast<size_t>(rollTrack_)].size() > 1;
    std::vector<std::tuple<int, juce::Rectangle<float>, juce::String>> sounding;
    const auto& notes = tr.notes;
    for (auto it = firstInView(notes, layout().longest[static_cast<size_t>(rollTrack_)]); it != notes.end() && static_cast<double>(it->start) <= to; ++it)
    {
        if (static_cast<double>(it->start + it->length) < from)
            continue;
        const int i = static_cast<int>(it - notes.begin());
        auto r = rollNoteBounds(i);
        if ((drag_ == Drag::Move || drag_ == Drag::Resize) && selection_.count({ rollTrack_, it->id }))
        {
            const float dx = static_cast<float>(dragDelta_ / samplesPerPixel_);
            if (drag_ == Drag::Move)
                r.translate(dx, -static_cast<float>(dragShift_) * rh);
            else
                r.setWidth(std::max(2.0f, r.getWidth() + dx));
        }
        if (r.getY() > static_cast<float>(bottom) || r.getBottom() < static_cast<float>(top))
            continue;
        const juce::String tag = tagged ? "V" + juce::String(tune.originalChannel(tr, *it) + 1) : juce::String();
        if (isSounding(*it))
            sounding.push_back({ i, r, tag });
        else
            paintNote(g, rollTrack_, i, r, tag);
    }
    for (const auto& [i, r, tag] : sounding)
        paintNote(g, rollTrack_, i, r, tag);

    if (drag_ == Drag::Box)
    {
        g.setColour(theme::cyan.withAlpha(0.12f));
        g.fillRect(box_);
        g.setColour(theme::cyan.withAlpha(0.8f));
        g.drawRect(box_, 1.0f);
    }
    g.restoreState();
    paintKeys(g);
    paintLane(g);
}

void TrackerView::paintKeys(juce::Graphics& g)
{
    const int width = headerWidth() - 1, top = rollTop(), bottom = lanesTop();
    const auto rh = static_cast<float>(rowHeight_);
    g.saveState();
    g.reduceClipRegion(0, top, width, bottom - top);
    g.setColour(theme::panel);
    g.fillRect(0, top, width, bottom - top);

    // Lit in their pitch colours: the key the mouse holds down, and the
    // track's notes under the playhead, playing or stopped.
    std::array<bool, 128> on {};
    if (heldKey_ >= 0 && heldKey_ < 128)
        on[static_cast<size_t>(heldKey_)] = true;
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    const double now = model_.displayPosition();
    // Back from the playhead as far as the track's longest note reaches.
    const int64_t longest = layout().longest[static_cast<size_t>(rollTrack_)];
    auto it = std::upper_bound(tr.notes.begin(), tr.notes.end(), now, [](double time, const TuneNote& n) { return time < static_cast<double>(n.start); });
    while (it != tr.notes.begin())
    {
        --it;
        if (static_cast<double>(it->start + longest) < now)
            break;
        if (now < static_cast<double>(it->start + it->length) && model_.tune().audible(tr, *it))
            if (const int p = model_.tune().pitch(tr, *it); p >= 0 && p < 128)
                on[static_cast<size_t>(p)] = true;
    }

    // Each row a key: a white key across the column, a black key over the
    // white ones from the left, as on the strip under the tracks.
    const juce::Colour white(0xffcfcde3), black(0xff1b1e30), seam(0xff9fa1bd);
    const float blackWidth = static_cast<float>(width) * 0.6f;
    const int highest = keyAt(static_cast<float>(top)), lowest = keyAt(static_cast<float>(bottom - 1));
    for (int k = lowest; k <= highest; ++k)
    {
        const float y = keyTop(k);
        const auto row = juce::Rectangle<float>(0.0f, y, static_cast<float>(width), rh);
        const bool lit = on[static_cast<size_t>(k)];
        if (!blackKey(k))
        {
            g.setColour(lit ? theme::pitchColour(k) : white);
            g.fillRect(row);
            // Two white keys side by side, B and C or E and F, have a seam between them.
            const int pc = k % 12;
            if (pc == 0 || pc == 5)
            {
                g.setColour(seam);
                g.fillRect(row.withTop(y + rh - 1.0f));
            }
            continue;
        }
        // Beside the black key, the white keys either side meet in its middle.
        g.setColour(white);
        g.fillRect(row);
        g.setColour(seam);
        g.fillRect(juce::Rectangle<float>(blackWidth, y + rh * 0.5f - 0.5f, static_cast<float>(width) - blackWidth, 1.0f));
        const auto key = row.withWidth(blackWidth).reduced(0.0f, std::min(1.0f, rh * 0.1f));
        g.setColour(lit ? theme::pitchColour(k) : black);
        g.fillRoundedRectangle(key, 2.0f);
        g.setColour(lit ? juce::Colour(0xffbcbcc6) : juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(key.reduced(0.5f), 2.0f, 1.0f);
    }
    // C keys carry their octave, at the keys' front.
    g.setFont(theme::mono(11.0f, true));
    for (int k = lowest; k <= highest; ++k)
        if (k % 12 == 0)
        {
            const float y = keyTop(k) + rh * 0.5f;
            g.setColour(on[static_cast<size_t>(k)] ? theme::background : juce::Colour(0xff6c6f8e));
            g.drawText(theme::noteName(k).removeCharacters("-"), juce::Rectangle<float>(static_cast<float>(width) - 40.0f, y - 7.0f, 34.0f, 14.0f),
                       juce::Justification::centredRight, false);
        }
    g.restoreState();
}

void TrackerView::paintRollHeader(juce::Graphics& g)
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    const bool fm = tr.kind == TuneTrack::Kind::Fm;

    // The header across the top: the track's name over the keys, with the
    // button that goes back to the arrangement, and over the rows its
    // instrument, its voices and how many notes it has.
    const auto strip = juce::Rectangle<int>(0, kRulerHeight, getWidth(), kRollHeader);
    g.setColour(theme::panel);
    g.fillRect(strip);
    g.setColour(theme::background.withAlpha(0.35f));
    g.fillRect(strip);
    g.setColour(theme::gridBar);
    g.fillRect(0, strip.getBottom() - 1, getWidth(), 1);
    g.setColour(model_.tune().audible(tr) ? theme::text : theme::faint);
    g.setFont(theme::mono(12.0f, true));
    g.drawText(juce::String(tr.name).toUpperCase(), juce::Rectangle<int>(14, strip.getY(), rollButton(rollTrack_).getX() - 18, kRollHeader),
               juce::Justification::centredLeft, true);
    paintRollIcon(g, rollButton(rollTrack_).toFloat(), theme::cyan);

    juce::StringArray parts;
    if (fm)
    {
        parts.add((tr.instrument >= 0 ? "PATCH " + juce::String(tr.instrument + 1) : juce::String("DEFAULT PATCH")) + (tr.patch ? " EDITED" : ""));
        juce::StringArray voices;
        for (const int v : layout().voices[static_cast<size_t>(rollTrack_)])
            voices.add("V" + juce::String(v + 1));
        parts.add(voices.isEmpty() ? juce::String("NO VOICE") : (voices.size() == 1 ? "VOICE " : "VOICES ") + voices.joinIntoString(" "));
    }
    else
        parts.add("PCM " + juce::String(tr.instrument + 1) + (tr.sample.any() ? " EDITED" : ""));
    const size_t count = tr.notes.size();
    parts.add(juce::String(count) + (fm ? (count == 1 ? " NOTE" : " NOTES") : (count == 1 ? " HIT" : " HITS")));
    g.setColour(theme::dim);
    g.setFont(theme::mono(11.0f, true));
    g.drawText(parts.joinIntoString(theme::dot()), strip.withTrimmedLeft(headerWidth() + 10).withTrimmedRight(10), juce::Justification::centredLeft, true);

    // The column's edge, down the whole height, and the corner over it.
    g.setColour(theme::gridBar);
    g.fillRect(headerWidth() - 1, 0, 1, getHeight());
    g.setColour(theme::panel);
    g.fillRect(0, 0, headerWidth() - 1, kRulerHeight);
    g.setColour(theme::gridBar);
    g.fillRect(0, kRulerHeight - 1, headerWidth(), 1);
    g.setColour(theme::magenta);
    g.setFont(theme::pixel(8.0f));
    g.drawText("PIANO ROLL", juce::Rectangle<int>(14, 0, headerWidth(), kRulerHeight), juce::Justification::centredLeft);
}

void TrackerView::paintLane(juce::Graphics& g)
{
    const int top = lanesTop();
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    const bool drum = tr.kind == TuneTrack::Kind::Drum;

    // The strip of tabs, as the strips under the tracks have their bar: its
    // name, the grip in its top edge while open and the chevron.
    g.setColour(theme::panel);
    g.fillRect(0, top, getWidth(), getHeight() - top);
    g.setColour(theme::gridBar);
    g.fillRect(0, top, getWidth(), 1);
    g.setFont(theme::pixel(8.0f));
    g.setColour(theme::orange);
    g.drawText("LANE", juce::Rectangle<int>(14, top + 1, headerWidth(), kLaneTabs), juce::Justification::centredLeft);
    if (laneOpen_)
    {
        g.setColour(juce::Colour(0xffc4c6e4));
        g.fillRoundedRectangle(static_cast<float>(headerWidth() + getWidth()) * 0.5f - 18.0f, static_cast<float>(top) + 2.0f, 36.0f, 2.0f, 1.0f);
    }
    {
        const auto r = laneChevron().toFloat().reduced(0.5f);
        const bool over = isMouseOver() && r.contains(getMouseXYRelative().toFloat());
        g.setColour(over ? theme::panelLight : theme::background);
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(over ? theme::dim : theme::gridBar);
        g.drawRoundedRectangle(r, 4.0f, 1.0f);
        const auto c = r.getCentre();
        juce::Path p;
        if (!laneOpen_)
            p.addTriangle(c.x - 4.0f, c.y + 2.0f, c.x + 4.0f, c.y + 2.0f, c.x, c.y - 2.5f);
        else
            p.addTriangle(c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 2.5f);
        g.setColour(over ? theme::text : theme::dim);
        g.fillPath(p);
    }
    // The tabs, the lane shown lit as a lit toggle is.
    g.setFont(theme::mono(11.0f, true));
    for (const Lane l : { Lane::Velocity, Lane::Fine, Lane::Pan })
    {
        const auto r = laneTab(l).toFloat();
        const bool on = l == lane_;
        g.setColour(on ? theme::cyan : theme::background.withAlpha(0.7f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(on ? theme::cyan : theme::gridBar.brighter(0.3f));
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(on ? theme::background : theme::text.withAlpha(0.85f));
        g.drawText(l == Lane::Velocity ? "VELOCITY" : l == Lane::Fine ? "FINE" : "PAN", r, juce::Justification::centred, false);
    }
    if (!laneOpen_)
        return;

    const auto body = juce::Rectangle<int>(headerWidth(), top + kLaneTabs, getWidth() - headerWidth(), getHeight() - top - kLaneTabs);
    const auto area = laneArea();
    g.saveState();
    g.reduceClipRegion(body);
    g.setColour(theme::lane);
    g.fillRect(body);
    paintGrid(g, body.getY(), body.getBottom());
    // Guides at the scale's marks; the middle bright for fine tune and pan.
    const std::vector<int> marks = lane_ == Lane::Velocity ? std::vector<int> { 0, -16, -32, -48, -64 } : std::vector<int> { 64, 32, 0, -32, -64 };
    for (const int m : marks)
    {
        g.setColour(lane_ != Lane::Velocity && m == 0 ? theme::gridBar : theme::grid);
        g.fillRect(juce::Rectangle<float>(static_cast<float>(body.getX()), std::round(laneY(m)), static_cast<float>(body.getWidth()), 1.0f));
    }

    // A bar for each note at its start, up to its value, and a faint line
    // at the value along the note. A ripped note's own level or pitch moves
    // go faint along it too, from its value.
    const double from = viewStart_, to = xToTime(static_cast<float>(getWidth()));
    const auto& notes = tr.notes;
    auto it = std::lower_bound(notes.begin(), notes.end(), from - static_cast<double>(layout().longest[static_cast<size_t>(rollTrack_)]),
                               [](const TuneNote& n, double time) { return static_cast<double>(n.start) < time; });
    const float base = lane_ == Lane::Velocity ? static_cast<float>(area.getBottom()) : laneY(0);
    for (; it != notes.end() && static_cast<double>(it->start) <= to; ++it)
    {
        const TuneNote& n = *it;
        if (static_cast<double>(n.start + n.length) < from)
            continue;
        const float x = timeToX(static_cast<double>(n.start)), x1 = timeToX(static_cast<double>(n.start + n.length));
        const int value = noteLaneValue(n);
        const float y = laneY(value);
        const bool audible = model_.tune().audible(tr, n);
        const bool selected = selection_.count({ rollTrack_, n.id }) > 0;
        const juce::Colour colour = (drum ? theme::instrumentColour(rollTrack_) : theme::pitchColour(model_.tune().pitch(tr, n))).withAlpha(audible ? 1.0f : 0.35f);

        if (!n.written() && lane_ != Lane::Pan && x1 - x >= 4.0f)
        {
            const NoteExpression& e = expressionOf(rollTrack_, n);
            const auto& curve = lane_ == Lane::Velocity ? e.level : e.pitch;
            if (!curve.empty())
            {
                // Velocity in 0.75 dB steps and fine tune in 64ths, as the lane's values.
                auto valueAt = [&](float moved) {
                    const float v = lane_ == Lane::Velocity ? static_cast<float>(value) + moved / 0.75f : static_cast<float>(value) + moved * 64.0f;
                    return lane_ == Lane::Velocity ? std::clamp(v, static_cast<float>(TuneNote::kMinVelocity), 0.0f)
                                                   : std::clamp(v, -static_cast<float>(TuneNote::kPanRange), static_cast<float>(TuneNote::kPanRange));
                };
                auto yOf = [&](float v) {
                    const auto a = area.toFloat();
                    return lane_ == Lane::Velocity ? a.getY() + -v / static_cast<float>(-TuneNote::kMinVelocity) * a.getHeight()
                                                   : a.getCentreY() - v / static_cast<float>(TuneNote::kPanRange) * a.getHeight() * 0.5f;
                };
                // A step for each change, at most one a pixel column.
                juce::Path path;
                float px = x, py = y;
                path.startNewSubPath(px, py);
                for (const auto& p : curve)
                {
                    const float cx = std::min(x1, x + static_cast<float>(static_cast<double>(p.offset) / samplesPerPixel_));
                    const float cy = yOf(valueAt(p.value));
                    if (cx - px >= 1.0f)
                    {
                        path.lineTo(cx, py);
                        px = cx;
                    }
                    path.lineTo(px, cy);
                    py = cy;
                    if (cx >= x1)
                        break;
                }
                path.lineTo(x1, py);
                g.setColour(juce::Colours::white.withAlpha(audible ? 0.3f : 0.12f));
                g.strokePath(path, juce::PathStrokeType(1.0f));
            }
        }

        // A pan left as played: an empty ring in the middle.
        if (lane_ == Lane::Pan && !n.pan && laneValues_.count(n.id) == 0)
        {
            g.setColour(colour.withAlpha(audible ? 0.6f : 0.25f));
            g.drawEllipse(x - 2.5f, y - 3.5f, 7.0f, 7.0f, 1.0f);
            continue;
        }
        g.setColour(colour.withAlpha(audible ? 0.35f : 0.15f));
        g.fillRect(juce::Rectangle<float>(x, y - 0.5f, std::max(0.0f, x1 - x), 1.0f));
        g.setColour(colour.withMultipliedBrightness(0.85f));
        g.fillRect(juce::Rectangle<float>(x, std::min(y, base), 2.0f, std::max(1.0f, std::abs(base - y))));
        g.setColour(colour);
        g.fillEllipse(x - 2.5f, y - 3.5f, 7.0f, 7.0f);
        if (selected)
        {
            g.setColour(juce::Colours::white);
            g.drawEllipse(x - 3.0f, y - 4.0f, 8.0f, 8.0f, 1.5f);
        }
    }
    g.restoreState();

    // The scale in the keys' column: the lane's name, then its marks' values.
    const auto column = juce::Rectangle<int>(0, body.getY(), headerWidth() - 1, body.getHeight());
    g.setColour(theme::dim);
    g.setFont(theme::mono(11.0f, true));
    g.drawText(lane_ == Lane::Velocity ? "dB" : lane_ == Lane::Fine ? "64THS" : drum ? "BALANCE" : "SIDES", column.withTrimmedLeft(14).withTrimmedTop(4).withHeight(14),
               juce::Justification::centredLeft, false);
    auto mark = [&](int value, const juce::String& text) {
        const float y = std::clamp(laneY(value), static_cast<float>(column.getY()) + 7.0f, static_cast<float>(column.getBottom()) - 7.0f);
        g.drawText(text, juce::Rectangle<float>(static_cast<float>(column.getRight()) - 64.0f, y - 7.0f, 56.0f, 14.0f), juce::Justification::centredRight, false);
    };
    if (column.getHeight() < 30)
        return;
    if (lane_ == Lane::Velocity)
        mark(0, "0"), mark(-32, "-24"), mark(-64, "-48");
    else if (lane_ == Lane::Fine)
        mark(64, "+63"), mark(0, "0"), mark(-64, "-63");
    else if (drum)
        mark(64, "R 64"), mark(0, "C"), mark(-64, "L 64");
    else
        mark(64, "R"), mark(0, "LR"), mark(-64, "L");
}

// Mouse -----------------------------------------------------------------------

void TrackerView::rollMouseDown(const juce::MouseEvent& e)
{
    // The rest of the click that opened the piano roll does nothing here.
    if (rollClick_ && e.getNumberOfClicks() > 1)
        return;
    rollClick_ = false;
    const auto p = e.position;
    const int lanes = lanesTop();
    if (p.y < static_cast<float>(rollTop()))
    {
        // The header: its button goes back to the arrangement.
        if (rollButton(rollTrack_).contains(p.toInt()))
            setPianoRoll(-1);
        return;
    }
    if (overLaneEdge(p.y))
    {
        drag_ = Drag::LaneHeight;
        dragHeight_ = getHeight() - lanes;
        return;
    }
    if (p.y >= static_cast<float>(lanes))
    {
        if (p.y < static_cast<float>(lanes + kLaneTabs))
        {
            // The chevron hides or shows the lane; a tab shows its lane.
            if (laneChevron().contains(p.toInt()))
                laneOpen_ = !laneOpen_;
            for (const Lane l : { Lane::Velocity, Lane::Fine, Lane::Pan })
                if (laneTab(l).contains(p.toInt()))
                    lane_ = l, laneOpen_ = true;
            setRollScroll(rollScroll_);
            return;
        }
        if (p.x < lanesX())
            return;
        // A selected bar of several moves the selection's together; else
        // the bar is set where it is pressed, and a drag draws across the
        // bars it crosses.
        const auto& notes = model_.tune().tracks[static_cast<size_t>(rollTrack_)].notes;
        const int i = laneBarAt(p.x);
        laneValues_.clear();
        laneGrabbed_ = i >= 0 ? notes[static_cast<size_t>(i)].id : -1;
        laneGroup_ = i >= 0 && selection_.size() > 1 && selection_.count({ rollTrack_, laneGrabbed_ }) > 0;
        laneLast_ = p;
        drag_ = Drag::LaneValue;
        if (laneGroup_)
            rollMouseDrag(e);
        else if (i >= 0)
            laneValues_[laneGrabbed_] = laneValue(p.y, e.mods.isShiftDown());
        repaint();
        return;
    }
    if (p.x < lanesX())
    {
        // A key sounds while the mouse holds it, and a drag goes from key to key.
        drag_ = Drag::Keys;
        holdKey(keyAt(p.y));
        return;
    }

    bool edge = false;
    const NoteRef hit = rollNoteAt(p, edge);
    if (hit.valid())
    {
        if (e.mods.isShiftDown())
        {
            if (!selection_.erase(hit))
                selection_.insert(hit);
        }
        else if (!selection_.count(hit))
            selection_ = { hit };
        if (!e.mods.isShiftDown())
            if (const int i = indexOf(hit); i >= 0)
                preview(hit, model_.tune().tracks[static_cast<size_t>(rollTrack_)].notes[static_cast<size_t>(i)]);
        dragNote_ = hit;
        dragShift_ = 0;
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

void TrackerView::rollMouseDrag(const juce::MouseEvent& e)
{
    const auto p = e.position;
    const auto& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    if (drag_ == Drag::Keys)
    {
        // Off the rows, the key down stays down.
        if (p.y >= static_cast<float>(rollTop()) && p.y < static_cast<float>(lanesTop()))
            holdKey(keyAt(p.y));
        return;
    }
    if (drag_ == Drag::LaneHeight)
    {
        laneOpenHeight_ = std::clamp(dragHeight_ + static_cast<int>(dragFrom_.y - p.y), kMinLaneOpen, std::max(kMinLaneOpen, (getHeight() - rollTop()) / 2));
        setRollScroll(rollScroll_);
        return;
    }
    if (drag_ == Drag::LaneValue)
    {
        const bool fine = e.mods.isShiftDown();
        if (laneGroup_)
        {
            // The selection's bars move by as much as the one dragged; an FM
            // note's pan, one of three sides, goes to the dragged one's.
            const auto grabbed = std::find_if(tr.notes.begin(), tr.notes.end(), [this](const TuneNote& n) { return n.id == laneGrabbed_; });
            if (grabbed == tr.notes.end())
                return;
            const int target = laneValue(p.y, fine);
            const int delta = target - (lane_ == Lane::Velocity ? grabbed->velocity : lane_ == Lane::Fine ? grabbed->fine : grabbed->pan.value_or(0));
            for (const TuneNote& n : tr.notes)
                if (selection_.count({ rollTrack_, n.id }))
                {
                    if (lane_ == Lane::Velocity)
                        laneValues_[n.id] = std::clamp(n.velocity + delta, TuneNote::kMinVelocity, 0);
                    else if (lane_ == Lane::Fine)
                        laneValues_[n.id] = std::clamp(n.fine + delta, -TuneNote::kMaxFine, TuneNote::kMaxFine);
                    else
                        laneValues_[n.id] = fmTrack(rollTrack_) ? target : std::clamp(n.pan.value_or(0) + delta, -TuneNote::kPanRange, TuneNote::kPanRange);
                }
        }
        else
        {
            drawLane(laneLast_.x, laneLast_.y, p.x, p.y, fine);
            // The bar grabbed follows the pointer while it stays in reach,
            // though the press was off the bar's exact start.
            if (const int i = indexOf({ rollTrack_, laneGrabbed_ }); i >= 0)
                if (std::abs(timeToX(static_cast<double>(tr.notes[static_cast<size_t>(i)].start)) - p.x) <= kBarReach)
                    laneValues_[laneGrabbed_] = laneValue(p.y, fine);
        }
        laneLast_ = p;
        repaint();
        return;
    }
    if (drag_ == Drag::Move || drag_ == Drag::Resize)
    {
        const int index = indexOf(dragNote_);
        if (index < 0)
            return;
        const TuneNote& n = tr.notes[static_cast<size_t>(index)];
        const double raw = (p.x - dragFrom_.x) * samplesPerPixel_;
        if (drag_ == Drag::Resize)
        {
            const double end = static_cast<double>(n.start + n.length);
            dragDelta_ = std::max(snapTime(end + raw), static_cast<double>(n.start) + kMinLength) - end;
            repaint();
            return;
        }
        dragDelta_ = snapTime(static_cast<double>(n.start) + raw) - static_cast<double>(n.start);
        // Up or down by semitones, or with Shift by octaves, as far as every
        // selected note's transposition goes.
        const float rows = (dragFrom_.y - p.y) / static_cast<float>(rowHeight_);
        int shift = e.mods.isShiftDown() ? static_cast<int>(std::lround(rows / 12.0f)) * 12 : static_cast<int>(std::lround(rows));
        int low = -96, high = 96;
        for (const TuneNote& x : tr.notes)
            if (selection_.count({ rollTrack_, x.id }))
                low = std::max(low, -48 - x.transpose), high = std::min(high, 48 - x.transpose);
        if (e.mods.isShiftDown())
        {
            // Whole octaves only: the range's multiples of 12.
            low = static_cast<int>(std::ceil(low / 12.0)) * 12;
            high = static_cast<int>(std::floor(high / 12.0)) * 12;
        }
        shift = low <= high ? std::clamp(shift, low, high) : 0;
        if (shift != dragShift_)
        {
            dragShift_ = shift;
            // The dragged note sounds at its new pitch, with the song stopped.
            if (!model_.playing())
            {
                TuneNote moved = n;
                moved.transpose += shift;
                preview(dragNote_, moved);
            }
        }
        repaint();
        return;
    }
    if (drag_ == Drag::Box)
    {
        box_ = juce::Rectangle<float>(dragFrom_, p);
        if (!e.mods.isShiftDown())
            selection_.clear();
        for (size_t i = 0; i < tr.notes.size(); ++i)
            if (rollNoteBounds(static_cast<int>(i)).intersects(box_))
                selection_.insert({ rollTrack_, tr.notes[i].id });
        repaint();
    }
}

void TrackerView::drawLane(float x0, float y0, float x1, float y1, bool fine)
{
    // Every bar between the two points takes the value on the line between
    // them, but for the other bars at the grabbed one's start, as a chord's
    // are: the bar grabbed changes alone.
    const auto& notes = model_.tune().tracks[static_cast<size_t>(rollTrack_)].notes;
    const int grabbed = indexOf({ rollTrack_, laneGrabbed_ });
    const int64_t grabbedStart = grabbed >= 0 ? notes[static_cast<size_t>(grabbed)].start : -1;
    const float left = std::min(x0, x1), right = std::max(x0, x1);
    const double from = xToTime(left), to = xToTime(right);
    auto it = std::lower_bound(notes.begin(), notes.end(), from, [](const TuneNote& n, double time) { return static_cast<double>(n.start) < time; });
    for (; it != notes.end() && static_cast<double>(it->start) <= to; ++it)
    {
        if (it->start == grabbedStart && it->id != laneGrabbed_)
            continue;
        const float x = timeToX(static_cast<double>(it->start));
        const float y = std::abs(x1 - x0) < 0.5f ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        laneValues_[it->id] = laneValue(y, fine);
    }
}

void TrackerView::rollMouseUp(const juce::MouseEvent& e)
{
    const Drag finished = drag_;
    drag_ = Drag::None;
    const auto& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
    // A click on empty rows, not a box drag, puts the stopped playhead there.
    if (finished == Drag::Box && e.mouseWasClicked() && !model_.playing())
        model_.setPosition(std::max(0.0, xToTime(e.position.x)));
    if (finished == Drag::Keys)
        holdKey(-1);
    if (finished == Drag::LaneValue)
    {
        // Only the values that change, as one step of undo.
        std::map<int, int> changed;
        for (const TuneNote& n : tr.notes)
            if (const auto it = laneValues_.find(n.id); it != laneValues_.end())
            {
                const bool same = lane_ == Lane::Velocity ? n.velocity == it->second : lane_ == Lane::Fine ? n.fine == it->second : n.pan == it->second;
                if (!same)
                    changed.insert(*it);
            }
        laneValues_.clear();
        const int track = rollTrack_;
        const Lane lane = lane_;
        if (!changed.empty()
            && model_.edit(lane == Lane::Velocity ? "Velocity" : lane == Lane::Fine ? "Fine tune" : "Pan", [track, lane, changed](Tune& tune) {
                   for (TuneNote& n : tune.tracks[static_cast<size_t>(track)].notes)
                       if (const auto it = changed.find(n.id); it != changed.end())
                       {
                           if (lane == Lane::Velocity)
                               n.velocity = it->second;
                           else if (lane == Lane::Fine)
                               n.fine = it->second;
                           else
                               n.pan = it->second;
                       }
               }))
        {
            // The bar grabbed sounds as it now is, else the first one drawn.
            const NoteRef heard { track, changed.count(laneGrabbed_) ? laneGrabbed_ : changed.begin()->first };
            if (const int i = indexOf(heard); i >= 0 && !model_.playing())
                preview(heard, model_.tune().tracks[static_cast<size_t>(track)].notes[static_cast<size_t>(i)]);
        }
        laneGrabbed_ = -1;
    }
    if ((finished == Drag::Move && (std::abs(dragDelta_) >= 1.0 || dragShift_ != 0)) || (finished == Drag::Resize && std::abs(dragDelta_) >= 1.0))
    {
        const auto chosen = selection_;
        const auto delta = static_cast<int64_t>(std::llround(dragDelta_));
        const int shift = finished == Drag::Move ? dragShift_ : 0;
        const bool move = finished == Drag::Move;
        const juce::String name = !move ? "Resize notes" : delta == 0 ? "Transpose" : "Move notes";
        model_.edit(name, [chosen, delta, shift, move, track = rollTrack_](Tune& tune) {
            auto& notes = tune.tracks[static_cast<size_t>(track)].notes;
            for (TuneNote& n : notes)
                if (chosen.count({ track, n.id }))
                {
                    if (move)
                    {
                        n.start = std::max<int64_t>(0, n.start + delta);
                        n.transpose = std::clamp(n.transpose + shift, -48, 48);
                    }
                    else
                        n.length = std::max<int64_t>(static_cast<int64_t>(kMinLength), n.length + delta);
                }
            sortByStart(notes);
        });
    }
    dragDelta_ = 0;
    dragShift_ = 0;
    repaint();
}

void TrackerView::rollMouseMove(const juce::MouseEvent& e)
{
    const auto p = e.position;
    bool edge = false;
    hover_ = rollNoteAt(p, edge);
    hoverEdge_ = edge;
    const int lanes = lanesTop();
    const bool overEdge = overLaneEdge(p.y);
    const bool overRuler = p.y < kRulerHeight && p.x >= lanesX();
    const bool overKeys = p.x < lanesX() && p.y >= static_cast<float>(rollTop()) && p.y < static_cast<float>(lanes);
    const int loopEdge = hover_.valid() ? 0 : loopEdgeAt(p);
    setMouseCursor((hover_.valid() && edge) || loopEdge != 0 ? juce::MouseCursor::LeftRightResizeCursor
                   : overEdge                                ? juce::MouseCursor::UpDownResizeCursor
                   : hover_.valid()                          ? juce::MouseCursor::DraggingHandCursor
                   : overRuler || overKeys                   ? juce::MouseCursor::PointingHandCursor
                                                             : juce::MouseCursor::NormalCursor);
    if (hover_.valid())
        return setTooltip(noteTooltip(hover_));

    juce::String tip;
    if (loopEdge != 0)
        tip = loopEdge < 0 ? "Loop start" : "Loop end";
    else if (overRuler)
        tip = "Click or drag to move the playhead; Shift-drag to set a loop";
    else if (rollButton(rollTrack_).contains(p.toInt()))
        tip = "Piano roll";
    else if (overEdge)
        tip = "Lane height";
    else if (overKeys)
        tip = theme::noteName(keyAt(p.y));
    else if (laneChevron().contains(p.toInt()))
        tip = laneOpen_ ? "Hide lane" : "Show lane";
    else if (laneTab(Lane::Velocity).contains(p.toInt()))
        tip = "Velocity";
    else if (laneTab(Lane::Fine).contains(p.toInt()))
        tip = "Fine tune";
    else if (laneTab(Lane::Pan).contains(p.toInt()))
        tip = "Pan";
    else if (laneOpen_ && p.y >= static_cast<float>(lanes + kLaneTabs) && p.x >= lanesX())
        if (const int i = laneBarAt(p.x); i >= 0)
        {
            // The bar's value, as the note's callout writes it.
            const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(rollTrack_)];
            const TuneNote& n = tr.notes[static_cast<size_t>(i)];
            if (lane_ == Lane::Velocity)
                tip = number(n.velocity * 0.75) + " dB";
            else if (lane_ == Lane::Fine)
                tip = (n.fine > 0 ? "+" : "") + number(std::round(n.fine * 1000.0 / 64.0) / 10.0) + " ct";
            else if (!n.pan)
                tip = "As played";
            else if (fmTrack(rollTrack_))
                tip = *n.pan < 0 ? "L" : *n.pan > 0 ? "R" : "LR";
            else
                tip = *n.pan == 0 ? juce::String("C") : (*n.pan < 0 ? "L " : "R ") + juce::String(std::abs(*n.pan));
        }
    setTooltip(tip);
}

void TrackerView::rollDoubleClick(const juce::MouseEvent& e)
{
    const auto p = e.position;
    if (p.x < lanesX() || p.y < static_cast<float>(rollTop()) || p.y >= static_cast<float>(lanesTop()))
        return;
    bool edge = false;
    if (const NoteRef hit = rollNoteAt(p, edge); hit.valid())
        editNote(hit);
    else
        addRollNote(keyAt(p.y), snapTime(xToTime(p.x)));
}

// Editing ---------------------------------------------------------------------

void TrackerView::holdKey(int key)
{
    if (key == heldKey_)
        return;
    const bool fm = fmTrack(rollTrack_);
    if (heldKey_ >= 0)
    {
        if (fm)
            model_.previewKey(heldKey_, false);
        else
            model_.previewSample(rollTrack_, heldKey_, false);
    }
    heldKey_ = -1;
    if (key >= 0 && rollTrack_ >= 0 && !model_.playing())
    {
        heldKey_ = key;
        if (fm)
        {
            model_.previewPatch(model_.editedPatch(rollTrack_), rollTrack_);
            model_.previewKey(key, true);
        }
        else
            model_.previewSample(rollTrack_, key, true);
    }
    repaint();
}

void TrackerView::addRollNote(int key, double time)
{
    const Tune& tune = model_.tune();
    const TuneTrack& tr = tune.tracks[static_cast<size_t>(rollTrack_)];
    TuneNote note;
    note.source = -1;
    note.start = static_cast<int64_t>(std::max(0.0, time));
    note.transpose = std::clamp(key - 60, -48, 48);
    if (tr.kind == TuneTrack::Kind::Fm)
    {
        // A beat long, or one step of the snap, on the track's voice that is
        // free for that long, else its first; a track with no voice takes a
        // free chip channel.
        note.length = static_cast<int64_t>(snap_ > 0 ? snap_ : tune.beatLength());
        note.voice = tune.freeVoice(tr, note.start, note.start + note.length);
        if (note.voice < 0)
            note.voice = std::max(0, freeVoice(rollTrack_));
    }
    else if (tune.sample(tr) != nullptr)
    {
        // A written hit of the track's sample on any free channel: as long
        // as the sample plays at that pitch, or a beat for one that loops.
        note.voice = -1;
        const int64_t natural = tune.sampleLength(tr, note.transpose);
        note.length = natural > 0 ? natural : static_cast<int64_t>(tune.beatLength());
    }
    else
        return;
    NoteRef added;
    const bool done = model_.edit("Add note", [track = rollTrack_, note, &added](Tune& changed) mutable {
        note.id = changed.newNoteId();
        added = { track, note.id };
        auto& list = changed.tracks[static_cast<size_t>(track)].notes;
        list.push_back(note);
        sortByStart(list);
    });
    if (done)
    {
        selection_ = { added };
        previewSelection();
    }
    repaint();
}

} // namespace opm86
