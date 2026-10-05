#include "TrackerView.h"

#include "Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <tuple>

namespace opm86
{

namespace
{

constexpr float kEdge = 6.0f;      // pixels at a note's right end that resize it
constexpr double kMinLength = 441; // 10 ms, the shortest note

// Header layout; see the comment before voicePill.
constexpr int kChipColumn = 24;  // width of the chip's column, at the left edge
constexpr int kKnobColumn = 48;  // width of the dB under the knob
constexpr int kKnob = 26;        // the knob's size
constexpr int kBlock = 44;       // height of the name and meter block
constexpr int kMinHeaderWidth = 150; // room for TRACKS and the strips' names and chevrons
constexpr float kKnobArc = juce::MathConstants<float>::pi * 0.75f; // the knob turns 135 degrees each way

// A number with up to two decimals, the zeros after the point left out.
juce::String trimmed(double v)
{
    juce::String s(v, 2);
    if (s.containsChar('.'))
        s = s.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return s == "-0" ? juce::String("0") : s;
}

juce::String signedNumber(double v)
{
    return (v > 0.0 ? "+" : "") + trimmed(v);
}

// A note's expression as the callout and the tooltip write it: velocity
// in dB, fine tune in cents ("+12.5 ct"), and pan as the output sides of an
// FM note ("L", "LR", "R") or a drum hit's balance ("L 32", "C", "R 20").
juce::String velocityText(int velocity)
{
    return trimmed(velocity * 0.75) + " dB";
}

juce::String fineText(int fine)
{
    return signedNumber(std::round(fine * 1000.0 / 64.0) / 10.0) + " ct";
}

juce::String panText(int pan, bool drum)
{
    if (!drum)
        return pan < 0 ? "L" : pan > 0 ? "R" : "LR";
    return pan == 0 ? juce::String("C") : (pan < 0 ? "L " : "R ") + juce::String(std::abs(pan));
}

// The parts of a note's expression away from as played, for its tooltip:
// "vel -6 dB, fine +12.5 ct, pan L". Empty when none is.
juce::String expressionText(const TuneNote& n, bool drum)
{
    juce::StringArray parts;
    if (n.velocity != 0)
        parts.add("vel " + velocityText(n.velocity));
    if (n.fine != 0)
        parts.add("fine " + fineText(n.fine));
    if (n.pan)
        parts.add("pan " + panText(*n.pan, drum));
    return parts.joinIntoString(", ");
}

// A pitch typed in the callout: "C#5", "c-4", "Bb3" or "60" for a note, and
// "+2", "-5" or "0" for a drum hit's transposition. None for text that is
// neither, or a key outside MIDI's.
std::optional<int> parsePitch(const juce::String& text, bool semitones)
{
    if (semitones)
    {
        const juce::String typed = text.trim().removeCharacters(" st");
        const juce::String digits = typed.trimCharactersAtStart("+-");
        if (digits.isEmpty() || !digits.containsOnly("0123456789") || typed.length() - digits.length() > 1)
            return std::nullopt;
        return typed.startsWithChar('-') ? -digits.getIntValue() : digits.getIntValue();
    }
    const juce::String t = text.trim().toUpperCase().removeCharacters("-");
    int note = -1;
    if (t.isNotEmpty() && t.containsOnly("0123456789"))
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
    if (note < 0 || note > 127)
        return std::nullopt;
    return note;
}

// A signed number typed with an optional unit after it, as "-6 dB" or
// "+12.5 ct". None for text that is no number.
std::optional<double> parseNumber(const juce::String& text, const char* units)
{
    const juce::String t = text.trim().toLowerCase().trimCharactersAtEnd(units).trim();
    const juce::String digits = t.trimCharactersAtStart("+-");
    if (digits.isEmpty() || !digits.containsOnly("0123456789.") || !digits.containsAnyOf("0123456789") || t.length() - digits.length() > 1)
        return std::nullopt;
    return t.startsWithChar('-') ? -digits.getDoubleValue() : digits.getDoubleValue();
}

// A note's properties in a callout: its pitch, typed as a name, and its
// fine tune, velocity and pan, each with a slider and a typed value.
// Nothing changes in the song until Return or the callout closes; then
// every change goes in at once, as one step of undo. Escape closes it
// with no change. Each change plays the note as it would sound, with the
// song stopped.
class NoteEditor : public juce::Component
{
public:
    struct Values
    {
        int pitch = 60;         // the MIDI key, or a drum hit's transposition
        int fine = 0;           // 64ths of a semitone
        int velocity = 0;       // 0.75 dB steps
        std::optional<int> pan; // none as played
        bool operator==(const Values&) const = default;
    };

    // With drum, the pitch is a transposition, typed and shown as "+2" or
    // "-5", and the pan a balance; else the pitch is a note name and the pan
    // the output sides. Count is how many notes it edits: with more than
    // one, the caller moves each by the change.
    NoteEditor(bool drum, int count, const Values& current, std::function<void(const Values&)> preview,
               std::function<bool(const Values&)> apply)
        : drum_(drum), initial_(current), values_(current), preview_(std::move(preview)), apply_(std::move(apply))
    {
        auto caption = [this](juce::Label& l, const juce::String& text, juce::Colour colour) {
            l.setText(text, juce::dontSendNotification);
            l.setFont(theme::pixel(8.0f));
            l.setColour(juce::Label::textColourId, colour);
            addAndMakeVisible(l);
        };
        caption(pitchCaption_, "PITCH", theme::orange);
        caption(fineCaption_, "FINE", theme::orange);
        caption(velocityCaption_, "VELOCITY", theme::orange);
        caption(panCaption_, drum ? "BALANCE" : "PAN", theme::orange);
        caption(count_, count > 1 ? juce::String(count) + " NOTES" : juce::String(), theme::dim);
        count_.setJustificationType(juce::Justification::centredRight);
        sixtyFourths_.setFont(theme::mono(11.0f));
        sixtyFourths_.setColour(juce::Label::textColourId, theme::dim);
        sixtyFourths_.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(sixtyFourths_);

        for (juce::TextEditor* e : { &pitch_, &fine_, &velocity_, &pan_ })
        {
            e->setFont(e == &pitch_ ? theme::mono(18.0f, true) : theme::mono(13.0f, true));
            e->setJustification(juce::Justification::centred);
            e->setColour(juce::TextEditor::backgroundColourId, theme::background);
            e->setColour(juce::TextEditor::textColourId, theme::text);
            e->setColour(juce::TextEditor::outlineColourId, theme::gridBar);
            e->setColour(juce::TextEditor::focusedOutlineColourId, theme::cyan);
            e->onReturnKey = [this] { commit(); };
            e->onEscapeKey = [this] { close(); };
            e->onFocusLost = [this, e] { read(*e); };
            addAndMakeVisible(*e);
        }
        pan_.setTextToShowWhenEmpty("as played", theme::faint);

        fineSlider_.setRange(-TuneNote::kMaxFine, TuneNote::kMaxFine, 1.0);
        velocitySlider_.setRange(TuneNote::kMinVelocity * 0.75, 0.0, 0.75);
        balance_.setRange(-TuneNote::kPanRange, TuneNote::kPanRange, 1.0);
        for (juce::Slider* s : { &fineSlider_, &velocitySlider_, &balance_ })
        {
            s->setSliderStyle(juce::Slider::LinearHorizontal);
            s->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            s->setWantsKeyboardFocus(false);
            s->getProperties().set("home", 0.0);
            s->onDragEnd = [this] { preview_(values_); };
        }
        fineSlider_.onValueChange = [this] {
            values_.fine = static_cast<int>(std::lround(fineSlider_.getValue()));
            refresh(&fine_);
        };
        velocitySlider_.onValueChange = [this] {
            values_.velocity = static_cast<int>(std::lround(velocitySlider_.getValue() / 0.75));
            refresh(&velocity_);
        };
        balance_.onValueChange = [this] {
            values_.pan = static_cast<int>(std::lround(balance_.getValue()));
            refresh(&pan_);
        };
        addAndMakeVisible(fineSlider_);
        addAndMakeVisible(velocitySlider_);
        // A drum hit's balance is a slider, its box cleared for as played; an
        // FM note's output, one of three sides, the one lit clicked again for
        // as played.
        if (drum)
        {
            balance_.setTooltip("Clear the box for as played");
            addAndMakeVisible(balance_);
        }
        else
            for (int k = 0; k < 3; ++k)
            {
                auto& b = sides_[static_cast<size_t>(k)];
                b.setButtonText(k == 0 ? "L" : k == 1 ? "LR" : "R");
                b.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
                b.setWantsKeyboardFocus(false);
                b.setTooltip("Click the lit side again for as played");
                b.onClick = [this, k] {
                    const int pan = (k - 1) * TuneNote::kPanRange;
                    if (values_.pan && TuneNote::sides(*values_.pan) == TuneNote::sides(pan))
                        values_.pan.reset();
                    else
                        values_.pan = pan;
                    refresh(&pan_);
                    preview_(values_);
                };
                addAndMakeVisible(b);
            }
        refresh();
        setSize(280, 246);
    }

    ~NoteEditor() override
    {
        // Closed by a click elsewhere: the changes go in.
        if (!closed_ && !(values_ == initial_))
            apply_(values_);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10, 8);
        auto top = area.removeFromTop(16);
        pitchCaption_.setBounds(top);
        count_.setBounds(top);
        pitch_.setBounds(area.removeFromTop(32));
        area.removeFromTop(6);
        // Each row: its caption, then the control and the typed value at the right.
        auto row = [&area](juce::Label& caption, juce::TextEditor& value) {
            caption.setBounds(area.removeFromTop(16));
            auto r = area.removeFromTop(26);
            value.setBounds(r.removeFromRight(80).reduced(0, 1));
            r.removeFromRight(10);
            return r;
        };
        fineSlider_.setBounds(row(fineCaption_, fine_));
        sixtyFourths_.setBounds(area.removeFromTop(14).removeFromRight(80));
        velocitySlider_.setBounds(row(velocityCaption_, velocity_));
        area.removeFromTop(6);
        const auto pan = row(panCaption_, pan_);
        balance_.setBounds(pan);
        auto sides = pan.reduced(0, 2);
        const int w = sides.getWidth() / 3;
        for (auto& b : sides_)
            b.setBounds(sides.removeFromLeft(w).reduced(2, 0));
    }

    void visibilityChanged() override
    {
        if (isShowing())
        {
            pitch_.grabKeyboardFocus();
            pitch_.selectAll();
        }
    }

private:
    // Every typed value read, then the changes in at once and the callout
    // closed. A value that is not one, or an edit the model refuses, keeps
    // it open; the model says why.
    void commit()
    {
        bool ok = true;
        for (juce::TextEditor* e : { &pitch_, &fine_, &velocity_, &pan_ })
            ok = read(*e) && ok;
        if (!ok)
            return;
        if (!(values_ == initial_) && !apply_(values_))
            return;
        close();
    }

    void close()
    {
        closed_ = true;
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    // A typed value into the values, played when it changes them; text that
    // is not one outlines its box in red and changes nothing.
    bool read(juce::TextEditor& e)
    {
        const Values before = values_;
        const juce::String text = e.getText();
        bool ok = true;
        if (&e == &pitch_)
        {
            const auto p = parsePitch(text, drum_);
            ok = p.has_value();
            if (p)
                values_.pitch = drum_ ? std::clamp(*p, -48, 48) : *p;
        }
        else if (&e == &fine_)
        {
            // Cents, or 64ths of a semitone typed as "8/64".
            std::optional<double> fine;
            if (text.containsChar('/'))
            {
                if (const auto n = parseNumber(text.upToFirstOccurrenceOf("/", false, false), ""); n && text.fromFirstOccurrenceOf("/", false, false).trim() == "64")
                    fine = *n;
            }
            else if (const auto cents = parseNumber(text, " ctens"))
                fine = *cents * 64.0 / 100.0;
            ok = fine.has_value();
            if (fine)
                values_.fine = std::clamp(static_cast<int>(std::lround(*fine)), -TuneNote::kMaxFine, TuneNote::kMaxFine);
        }
        else if (&e == &velocity_)
        {
            const auto db = parseNumber(text, " db");
            ok = db.has_value();
            if (db)
                values_.velocity = std::clamp(static_cast<int>(std::lround(*db / 0.75)), TuneNote::kMinVelocity, 0);
        }
        else
        {
            // Blank for as played; "L", "LR" or "R" for the sides, and for a
            // balance "C", "L 32", "R 20" or a signed number.
            const juce::String t = text.trim().toUpperCase().removeCharacters(" ");
            if (t.isEmpty())
                values_.pan.reset();
            else if (!drum_ && (t == "L" || t == "R" || t == "LR" || t == "L+R" || t == "C"))
                values_.pan = t == "L" ? -TuneNote::kPanRange : t == "R" ? TuneNote::kPanRange : 0;
            else if (drum_ && t == "C")
                values_.pan = 0;
            else if (drum_)
            {
                const juce::juce_wchar side = t[0];
                const bool named = side == 'L' || side == 'R';
                const auto n = parseNumber(named ? t.substring(1) : t, "");
                ok = n.has_value() && (!named || *n >= 0.0);
                if (ok)
                    values_.pan = std::clamp(static_cast<int>(std::lround(side == 'L' ? -*n : *n)), -TuneNote::kPanRange, TuneNote::kPanRange);
            }
            else
                ok = false;
        }
        e.setColour(juce::TextEditor::outlineColourId, ok ? theme::gridBar : theme::danger);
        if (ok)
        {
            refresh();
            if (!(values_ == before))
                preview_(values_);
        }
        return ok;
    }

    // The controls as the values have them; a box being typed in keeps its
    // text, unless a slider or a side button just set its value (changed),
    // so the box never reads its old text back over it.
    void refresh(const juce::TextEditor* changed = nullptr)
    {
        auto show = [changed](juce::TextEditor& e, const juce::String& text) {
            if (&e == changed)
                e.setColour(juce::TextEditor::outlineColourId, theme::gridBar);
            if (&e == changed || !e.hasKeyboardFocus(true))
                e.setText(text, false);
        };
        show(pitch_, drum_ ? signedNumber(values_.pitch) : theme::noteName(values_.pitch));
        show(fine_, fineText(values_.fine));
        show(velocity_, velocityText(values_.velocity));
        show(pan_, values_.pan ? panText(*values_.pan, drum_) : juce::String());
        sixtyFourths_.setText(signedNumber(values_.fine) + "/64", juce::dontSendNotification);
        fineSlider_.setValue(values_.fine, juce::dontSendNotification);
        velocitySlider_.setValue(values_.velocity * 0.75, juce::dontSendNotification);
        balance_.setValue(values_.pan.value_or(0), juce::dontSendNotification);
        balance_.setAlpha(values_.pan ? 1.0f : 0.5f);
        for (int k = 0; k < 3; ++k)
            sides_[static_cast<size_t>(k)].setToggleState(values_.pan && TuneNote::sides(*values_.pan) == TuneNote::sides((k - 1) * TuneNote::kPanRange),
                                                          juce::dontSendNotification);
    }

    bool drum_;
    Values initial_, values_;
    std::function<void(const Values&)> preview_;
    std::function<bool(const Values&)> apply_;
    bool closed_ = false; // applied or dropped already
    juce::Label pitchCaption_, fineCaption_, velocityCaption_, panCaption_, count_, sixtyFourths_;
    juce::TextEditor pitch_, fine_, velocity_, pan_;
    juce::Slider fineSlider_, velocitySlider_, balance_;
    std::array<juce::TextButton, 3> sides_;
};

// A drum hit's transposition as the pitch callout and the notes show it.
juce::String semitones(int transpose)
{
    return (transpose > 0 ? "+" : "") + juce::String(transpose);
}

// A short account of a note's expression for its tooltip, such as
// "vibrato ±0.3 st, bend +2 st, fade -12 dB". A curve that turns back and
// forth is a vibrato or tremolo, with a bend or fade when it ends past its
// swing; else it is the bend or fade it reaches furthest.
juce::String expressionSummary(const NoteExpression& e)
{
    // Two decimals under 1, so a fine vibrato or bend of a few 64ths of a
    // semitone still shows, one under 10 and none above; a part whose value
    // rounds to 0 goes.
    auto number = [](float v) {
        const float size = std::abs(v);
        juce::String s(v, size < 1.0f ? 2 : size < 10.0f ? 1 : 0);
        if (s.containsChar('.'))
            s = s.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        return s == "-0" ? juce::String("0") : s;
    };
    auto shows = [&](float v) { return number(v) != "0"; };
    auto withSign = [&](float v) { return (v > 0.0f ? "+" : "") + number(v); };
    const juce::String plusMinus = juce::String::fromUTF8("\xc2\xb1");
    juce::StringArray parts;
    auto describe = [&](const std::vector<NoteExpression::Point>& curve, const char* swing, const char* up, const char* down,
                        const char* unit) {
        if (curve.empty())
            return;
        float lo = 0.0f, hi = 0.0f, previous = 0.0f;
        int turns = 0, direction = 0;
        for (const auto& p : curve)
        {
            lo = std::min(lo, p.value);
            hi = std::max(hi, p.value);
            const int d = p.value > previous ? 1 : -1;
            if (direction != 0 && d != direction)
                ++turns;
            direction = d;
            previous = p.value;
        }
        const float last = curve.back().value, depth = (hi - lo) * 0.5f;
        if (turns >= 2)
        {
            if (shows(depth))
                parts.add(juce::String(swing) + " " + plusMinus + number(depth) + unit);
            if (std::abs(last) > depth && shows(last))
                parts.add(juce::String(last > 0.0f ? up : down) + " " + withSign(last) + unit);
        }
        else
        {
            const float reach = std::abs(hi) >= std::abs(lo) ? hi : lo;
            if (shows(reach))
                parts.add(juce::String(reach > 0.0f ? up : down) + " " + withSign(reach) + unit);
        }
    };
    describe(e.pitch, "vibrato", "bend", "bend", " st");
    describe(e.level, "tremolo", "swell", "fade", " dB");
    return parts.joinIntoString(", ");
}

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

        // An FM track gets a row per chip voice it plays on, one at least,
        // each with its own controls: a voice plays one note at a time, so
        // its notes never overlap in its row.
        std::vector<int> voices = model_.tune().voices(t);
        std::sort(voices.begin(), voices.end()); // V1 to V8, top to bottom
        const bool fixedVoices = t.kind == TuneTrack::Kind::Fm && !voices.empty();
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
        // A track with voices has a row per voice under a label strip.
        layout_.labelsBefore.push_back(layout_.labels);
        if (!layout_.voices.back().empty())
            ++layout_.labels;
        layout_.shown.push_back(layout_.rows.back());
        layout_.firstRow.push_back(layout_.totalRows);
        layout_.totalRows += layout_.shown.back();
        layout_.longest.push_back(longest);
        layout_.index.push_back(std::move(index));

        // The names in the rows, a track's or its voices', share a column as
        // wide as the widest, so mute and solo line up; the knobs go after them.
        const juce::String rowName = layout_.voices.back().empty() ? juce::String(t.name).toUpperCase() : juce::String("V8");
        const int name = std::min(juce::GlyphArrangement::getStringWidthInt(theme::mono(13.0f, true), rowName) + 2, 96);
        layout_.nameEnd = std::max(layout_.nameEnd, kChipColumn + 14 + name);
    }
    layout_.knobX = layout_.nameEnd + 8 + 2 * 23 + 3 + 4 + 17 + 8; // name, mute, solo, gear
    // The header ends past the knobs' column and the dB under them, with room to breathe.
    layout_.headerWidth = std::max(kMinHeaderWidth, layout_.knobX + (kKnob + kKnobColumn) / 2 + 10);
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
    const double length = model_.hasTune() ? static_cast<double>(model_.tune().length()) + 4 * kVgmRate : 0;
    viewStart_ = std::clamp(songTime, -kVgmRate, std::max(0.0, length - visibleSamples() * 0.5));
    repaint();
    sendChangeMessage();
}

void TrackerView::zoomAround(double factor, float x)
{
    const double anchor = xToTime(x);
    samplesPerPixel_ = std::clamp(samplesPerPixel_ / factor, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    setViewStart(anchor - (x - lanesX()) * samplesPerPixel_);
}

void TrackerView::followPlayhead()
{
    if (model_.playing())
        reveal(model_.displayPosition());
}

void TrackerView::reveal(double p)
{
    // With the whole song in view, the view stays still.
    if (!model_.hasTune() || (viewStart_ <= 0 && viewStart_ + visibleSamples() >= static_cast<double>(model_.tune().length())))
        return;
    // The playhead runs to the middle, then the song moves under it.
    if (p > viewStart_ + visibleSamples() * 0.5)
        setViewStart(p - visibleSamples() * 0.5);
    else if (p < viewStart_)
        setViewStart(p - visibleSamples() * 0.1);
}

void TrackerView::fitTime()
{
    // The whole rip across, from its start to its end, with a little room either side.
    if (!model_.hasTune())
        return;
    const double length = static_cast<double>(model_.tune().length());
    const double width = std::max(1.0, static_cast<double>(getWidth() - headerWidth()));
    samplesPerPixel_ = std::clamp(length * 1.04 / width, kMinSamplesPerPixel, kMaxSamplesPerPixel);
    setViewStart(-length * 0.02);
}

void TrackerView::fitTracks()
{
    if (!model_.hasTune() || layout().totalRows == 0)
        return;
    // The room the label strips leave, shared by the rows.
    const double room = getHeight() - kRulerHeight - layout().labels * kLabelHeight;
    laneHeightExact_ = std::clamp(std::floor(room / layout().totalRows), static_cast<double>(kMinLaneHeight), static_cast<double>(kMaxLaneHeight));
    laneHeight_ = static_cast<int>(laneHeightExact_);
    setScrollY(0);
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
    for (size_t t = 0; t < layout().shown.size(); ++t)
    {
        const int top = trackTop(static_cast<int>(t));
        if (y >= static_cast<float>(top) && y < static_cast<float>(top + trackHeight(static_cast<int>(t))))
            return static_cast<int>(t);
    }
    return -1;
}

int TrackerView::headerRow(int track, float y) const
{
    // A track with voices: -1 over its label, else the voice's row.
    const float top = static_cast<float>(rowTop(track, 0));
    if (labelled(track) && y < top)
        return -1;
    const int r = static_cast<int>((y - top) / static_cast<float>(laneHeight_));
    return std::clamp(r, 0, layout().shown[static_cast<size_t>(track)] - 1);
}

int TrackerView::trackTop(int track) const
{
    return kRulerHeight + layout().firstRow[static_cast<size_t>(track)] * laneHeight_ + layout().labelsBefore[static_cast<size_t>(track)] * kLabelHeight
           - scrollY_;
}

int TrackerView::trackHeight(int track) const
{
    return layout().shown[static_cast<size_t>(track)] * laneHeight_ + (labelled(track) ? kLabelHeight : 0);
}

int TrackerView::rowTop(int track, int row) const
{
    return trackTop(track) + (labelled(track) ? kLabelHeight : 0) + row * laneHeight_;
}

bool TrackerView::labelled(int track) const
{
    return !layout().voices[static_cast<size_t>(track)].empty();
}

int TrackerView::contentHeight() const
{
    return model_.hasTune() ? layout().totalRows * laneHeight_ + layout().labels * kLabelHeight + kAddRow : 0;
}

double TrackerView::snapTime(double t) const
{
    if (snap_ <= 0 || !model_.hasTune())
        return t;
    const double origin = model_.tune().firstBeat();
    return origin + std::round((t - origin) / snap_) * snap_;
}

juce::Rectangle<float> TrackerView::noteBounds(int track, int note) const
{
    const TuneNote& n = model_.tune().tracks[static_cast<size_t>(track)].notes[static_cast<size_t>(note)];
    const int row = layout().row[static_cast<size_t>(track)][static_cast<size_t>(note)];
    const float h = static_cast<float>(laneHeight_) - 6.0f;
    const float y = static_cast<float>(rowTop(track, row)) + 3.0f;
    const float x0 = timeToX(static_cast<double>(n.start));
    const float x1 = timeToX(static_cast<double>(n.start + n.length));
    return { x0, y, std::max(2.0f, x1 - x0), h };
}

TrackerView::NoteRef TrackerView::noteAt(juce::Point<float> p, bool& onRightEdge) const
{
    onRightEdge = false;
    const int t = yToTrack(p.y);
    if (t < 0 || p.x < lanesX())
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

    // Channel levels fall back over about a third of a second; a track shows its loudest channel.
    const auto peaks = model_.takePeaks();
    const float fall = static_cast<float>(std::pow(0.82, seconds * 60.0));
    for (size_t c = 0; c < peaks.size(); ++c)
        channelLevel_[c] = model_.playing() ? std::max(peaks[c], channelLevel_[c] * fall) : channelLevel_[c] * fall;
    // A dB scale from kMeterFloor to full scale, so a volume change moves
    // the meter by as much as it changes the sound; the peak hold waits a
    // second, then falls.
    auto meter = [seconds](float gain, float& level, float& hold, double& age) {
        const float db = juce::Decibels::gainToDecibels(gain, kMeterFloor);
        level = (db - kMeterFloor) / -kMeterFloor;
        if (level >= hold)
            hold = level, age = 0.0;
        else if ((age += seconds) > 1.0)
            hold = std::max(level, hold - static_cast<float>(seconds) * 0.6f);
    };
    level_.resize(tracks.size(), 0.0f);
    hold_.resize(tracks.size(), 0.0f);
    holdAge_.resize(tracks.size(), 0.0);
    voiceLevel_.resize(tracks.size());
    voiceHold_.resize(tracks.size());
    voiceHoldAge_.resize(tracks.size());
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        meter(trackLevel(static_cast<int>(t)), level_[t], hold_[t], holdAge_[t]);
        // A track with voice rows: a meter for each row's voice.
        const size_t rows = t < layout().voices.size() ? layout().voices[t].size() : 0;
        voiceLevel_[t].resize(rows, 0.0f);
        voiceHold_[t].resize(rows, 0.0f);
        voiceHoldAge_[t].resize(rows, 0.0);
        for (size_t r = 0; r < rows; ++r)
            meter(trackLevel(static_cast<int>(t), static_cast<int>(r)), voiceLevel_[t][r], voiceHold_[t][r], voiceHoldAge_[t][r]);
    }
}

float TrackerView::trackLevel(int track, int row) const
{
    const double now = model_.position();
    float loudest = 0.0f;
    for (size_t c = 0; c < owners_.size(); ++c)
    {
        const auto& o = owners_[c];
        const auto it = std::upper_bound(o.begin(), o.end(), Owner { static_cast<int64_t>(now), 0, 0 });
        if (it != o.begin() && std::prev(it)->track == track && (row < 0 || std::prev(it)->row == row))
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
    scrollY_ = std::clamp(y, 0, std::max(0, contentHeight() - (getHeight() - kRulerHeight)));
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
// with the level in dB under it. Compact lanes keep only the name line. A
// track opened into its voices has a row like that for each voice under its
// own.

juce::Rectangle<int> TrackerView::nameBounds(int track, int row) const
{
    // Up to the names' shared column, leaving mute and solo room before
    // the knob. Centred in a compact lane.
    const int left = kChipColumn + 14;
    const int right = compact() ? headerWidth() - 4 : knobBounds(track, row).getX() - 8;
    const int room = right - left - 2 * (pillWidth() + 3) - 5 - 21;
    const int width = std::min(layout().nameEnd - left, std::max(24, room));
    const int height = minimal() ? laneHeight_ - 6 : 17;
    return { left, rowTop(track, row) + (laneHeight_ - (compact() ? height : kBlock)) / 2, width, height };
}

juce::Rectangle<int> TrackerView::pill(int track, int which, int row) const
{
    const auto name = nameBounds(track, row);
    return { name.getRight() + 8 + which * (pillWidth() + 3), name.getY(), pillWidth(), name.getHeight() };
}

juce::Rectangle<int> TrackerView::gear(int track) const
{
    // In a label strip, after the name; else after solo.
    if (labelled(track))
        return { label(track).getRight() + 4, trackTop(track) + (kLabelHeight - 17) / 2 + 1, 17, 17 };
    const auto solo = pill(track, 1);
    return { solo.getRight() + 4, solo.getY() + (solo.getHeight() - 17) / 2, 17, 17 };
}

juce::Rectangle<int> TrackerView::addVoiceButton(int track) const
{
    if (!labelled(track) || layout().voices[static_cast<size_t>(track)].size() >= 8)
        return {};
    return gear(track).translated(17 + 4, 0);
}

juce::Rectangle<int> TrackerView::addTrackButton() const
{
    return { kChipColumn + 14, kRulerHeight + contentHeight() - kAddRow + 5 - scrollY_, 70, kLabelHeight };
}

juce::Rectangle<int> TrackerView::removeTrackButton(int track) const
{
    if (labelled(track))
        return gear(track).translated(2 * (17 + 4), 0);
    return gear(track).translated(17 + 4, 0);
}

juce::Rectangle<int> TrackerView::removeVoiceButton(int track, int row) const
{
    if (!labelled(track) || layout().voices[static_cast<size_t>(track)].size() < 2)
        return {};
    const auto solo = pill(track, 1, row);
    return { solo.getRight() + 4, solo.getY() + (solo.getHeight() - 17) / 2, 17, 17 };
}

juce::Rectangle<int> TrackerView::label(int track) const
{
    const juce::String name = juce::String(model_.tune().tracks[static_cast<size_t>(track)].name).toUpperCase();
    const int width = juce::GlyphArrangement::getStringWidthInt(theme::mono(12.0f, true), name) + 2;
    return { kChipColumn + 14, trackTop(track) + 1, width, kLabelHeight - 1 };
}

juce::Rectangle<int> TrackerView::knobBounds(int track, int row) const
{
    // In one column for all tracks and voices, right after the widest name line.
    const int top = rowTop(track, row) + (laneHeight_ - kBlock) / 2;
    return { layout().knobX, top, kKnob, kKnob };
}

juce::Rectangle<int> TrackerView::meterBounds(int track, int row) const
{
    // The width up to the knob and the level in dB under it.
    const int top = rowTop(track, row) + (laneHeight_ - kBlock) / 2;
    return { kChipColumn + 14, top + 25, knobBounds(track, row).getCentreX() - kKnobColumn / 2 - 4 - kChipColumn - 14, 16 };
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
        g.setColour(theme::dim);
        g.setFont(theme::pixel(12.0f));
        g.drawText("OPEN A RIP OR A PROJECT TO START", getLocalBounds(), juce::Justification::centred);
        g.setFont(theme::mono(14.0f));
        g.drawText("OPEN (Cmd+O) or drop one here, or start a new song with NEW (Cmd+N)", getLocalBounds().withTrimmedTop(60),
                   juce::Justification::centred);
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
    const float x = timeToX(model_.displayPosition());
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

    const Tune& tune = model_.tune();
    const double beat = tune.beatLength(), origin = tune.firstBeat();
    const long perBar = tune.beatsPerBar();
    const double beatPixels = beat / samplesPerPixel_;

    // The loop: a band between its two handles, lit while it is on.
    if (const LoopRegion loop = tune.loop; loop.valid())
    {
        const float x0 = std::max(lanesX(), timeToX(static_cast<double>(loop.start)));
        const float x1 = std::min(static_cast<float>(getWidth()), timeToX(static_cast<double>(loop.end)));
        const juce::Colour colour = loop.on ? theme::cyan : theme::faint;
        if (x1 > x0)
        {
            g.setColour(colour.withAlpha(loop.on ? 0.18f : 0.12f));
            g.fillRect(x0, 3.0f, x1 - x0, static_cast<float>(kRulerHeight - 4));
            g.setColour(colour.withAlpha(0.6f));
            g.fillRect(x0, 3.0f, x1 - x0, 1.0f);
        }
        g.setColour(colour);
        for (const double t : { static_cast<double>(loop.start), static_cast<double>(loop.end) })
            if (const float x = timeToX(t); x >= lanesX() - 3.0f && x <= static_cast<float>(getWidth()) + 3.0f)
            {
                // A handle: a tab at the top of a line down the ruler.
                g.fillRect(x - 0.75f, 3.0f, 1.5f, static_cast<float>(kRulerHeight - 4));
                g.fillRoundedRectangle(x - 3.0f, 3.0f, 6.0f, 10.0f, 1.5f);
            }
    }

    // Label every bar, or every 2nd, 4th... when bars are close together.
    int every = 1;
    while (beatPixels * static_cast<double>(perBar) * every < 44)
        every *= 2;
    const long first = static_cast<long>(std::floor((viewStart_ - origin) / beat)) - 1;
    const long last = static_cast<long>(std::ceil((xToTime(static_cast<float>(getWidth())) - origin) / beat));
    g.setFont(theme::pixel(10.0f));
    for (long k = first; k <= last; ++k)
    {
        const float x = timeToX(origin + static_cast<double>(k) * beat);
        if (x < lanesX())
            continue;
        const bool bar = ((k % perBar) + perBar) % perBar == 0;
        const long barNumber = (k >= 0 ? k / perBar : (k - perBar + 1) / perBar) + 1;
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
    const double beat = model_.tune().beatLength(), origin = model_.tune().firstBeat();
    const long perBar = model_.tune().beatsPerBar();
    const double beatPixels = beat / samplesPerPixel_;
    const auto lanes = juce::Rectangle<int>(headerWidth(), kRulerHeight, getWidth() - headerWidth(), getHeight() - kRulerHeight);
    g.saveState();
    g.reduceClipRegion(lanes);
    g.setColour(theme::lane);
    g.fillRect(lanes);

    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const int y = trackTop(static_cast<int>(t)), h = trackHeight(static_cast<int>(t));
        if (y > getHeight() || y + h < kRulerHeight)
            continue;
        g.setColour(t % 2 ? theme::lane : theme::laneAlt);
        g.fillRect(headerWidth(), y, getWidth() - headerWidth(), h);
        // A track with voices: its label strip a shade darker, and faint lines
        // between its voices' rows.
        if (labelled(static_cast<int>(t)))
        {
            g.setColour(theme::background.withAlpha(0.35f));
            g.fillRect(headerWidth(), y, getWidth() - headerWidth(), kLabelHeight);
            g.setColour(theme::grid.withAlpha(0.3f));
            for (size_t k = 0; k < layout().voices[t].size(); ++k)
                g.fillRect(headerWidth(), rowTop(static_cast<int>(t), static_cast<int>(k)), getWidth() - headerWidth(), 1);
        }
        g.setColour(theme::grid.withAlpha(0.6f));
        g.fillRect(headerWidth(), y + h - 1, getWidth() - headerWidth(), 1);
    }

    // Grid: bars bright, beats dim, sixteenths when there is room.
    const long first = static_cast<long>(std::floor((viewStart_ - origin) / beat)) - 1;
    const long last = static_cast<long>(std::ceil((xToTime(static_cast<float>(getWidth())) - origin) / beat));
    for (long k = first; k <= last; ++k)
    {
        const double t0 = origin + static_cast<double>(k) * beat;
        const bool bar = ((k % perBar) + perBar) % perBar == 0;
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

    // While notes move across rows, the row the dragged note lands on.
    if (drag_ == Drag::Move && dropTrack_ >= 0)
    {
        const auto [track, row] = landing(dragNote_.track, fromRow_);
        const auto band = juce::Rectangle<int>(headerWidth(), rowTop(track, row), getWidth() - headerWidth(), laneHeight_);
        g.setColour(theme::cyan.withAlpha(0.08f));
        g.fillRect(band);
        g.setColour(theme::cyan.withAlpha(0.5f));
        g.fillRect(band.withHeight(1));
        g.fillRect(band.withTop(band.getBottom() - 1));
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
                {
                    const int row = layout().row[t][static_cast<size_t>(i)];
                    const auto [track, landRow] = landing(static_cast<int>(t), row);
                    r.translate(dx, static_cast<float>(rowTop(track, landRow) - rowTop(static_cast<int>(t), row)));
                }
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
    // A quieter note is darker in its colour, down to half the brightness
    // at the lowest velocity. The curve is steepest near as played, so a
    // small cut already shows.
    const float loudness = 1.0f - 0.5f * std::sqrt(static_cast<float>(std::clamp(n.velocity, TuneNote::kMinVelocity, 0)) / static_cast<float>(TuneNote::kMinVelocity));
    const juce::Colour base = drum ? theme::instrumentColour(track).withMultipliedSaturation(0.8f) : theme::pitchColour(pitch);
    juce::Colour fill = !audible ? base.withMultipliedSaturation(0.15f).withMultipliedBrightness(0.6f).withAlpha(0.35f)
                        : sounding ? base.brighter(0.08f).withMultipliedBrightness(loudness)
                                   : base.withMultipliedSaturation(0.7f).withMultipliedBrightness(0.5f * loudness);
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
    // The rip's own expression, under the rims and the label, in blocks with room for it.
    if (showExpression_ && !n.written() && r.getWidth() >= 10.0f && r.getHeight() >= 12.0f)
    {
        // While a resize drags a selected note, its curve follows the
        // length it will get, as the release moves with the note's end.
        NoteExpression resized;
        const NoteExpression* e = &resized;
        if (drag_ == Drag::Resize && selected && std::abs(dragDelta_) >= 1.0)
        {
            TuneNote preview = n;
            preview.length = std::max<int64_t>(static_cast<int64_t>(kMinLength), n.length + static_cast<int64_t>(std::llround(dragDelta_)));
            resized = model_.tune().expression(tr, preview);
        }
        else
            e = &expressionOf(track, n);
        if (!e->empty())
            paintExpression(g, *e, r, juce::Colours::white.withAlpha(audible ? (sounding ? 0.7f : 0.45f) : 0.15f), audible ? 0.7f : 0.25f);
    }
    g.setColour(juce::Colours::white.withAlpha(audible ? (sounding ? 0.5f : 0.18f) : 0.05f));
    g.fillRect(r.withHeight(1.0f).reduced(2.0f, 0.0f));
    // Fine tune: a tick at the start, up from the middle for sharp and down
    // for flat, the whole half height at a semitone.
    if (n.fine != 0 && r.getWidth() >= 6.0f && r.getHeight() >= 8.0f)
    {
        const float reach = (r.getHeight() * 0.5f - 2.0f) * static_cast<float>(n.fine) / 64.0f;
        const float y = r.getCentreY();
        g.setColour(juce::Colours::white.withAlpha(audible ? 0.8f : 0.25f));
        g.fillRect(juce::Rectangle<float>(r.getX() + 2.0f, std::min(y, y - reach), 1.5f, std::max(1.5f, std::abs(reach))));
        g.fillRect(juce::Rectangle<float>(r.getX() + 2.0f, y - reach - (n.fine > 0 ? 0.0f : 1.5f), 4.0f, 1.5f));
    }
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
    // outline, so it reads on every colour. It ends where the next note in
    // the row begins, and notes with no room for 11 px text go without.
    const juce::String label = drum ? (n.transpose == 0 ? juce::String() : (n.transpose > 0 ? "+" : "") + juce::String(n.transpose))
                                    : theme::noteName(pitch).removeCharacters("-");
    auto room = r;
    const auto& rows = layout().row[static_cast<size_t>(track)];
    for (size_t j = static_cast<size_t>(note) + 1; j < tr.notes.size(); ++j)
        if (rows[j] == rows[static_cast<size_t>(note)])
        {
            const float next = timeToX(static_cast<double>(tr.notes[j].start));
            if (next > room.getX())
                room.setRight(std::min(room.getRight(), next));
            break;
        }
    const float height = std::min({ 26.0f, room.getHeight() * 0.62f, (room.getWidth() - 6.0f) / (0.64f * static_cast<float>(label.length())) });
    if (label.isNotEmpty() && height >= 11.0f)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText(theme::mono(height, true), label, room.getX(), room.getY(), room.getWidth(), room.getHeight(),
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

const NoteExpression& TrackerView::expressionOf(int track, const TuneNote& n) const
{
    if (expressionsRevision_ != model_.revision())
    {
        expressions_.clear();
        expressionsRevision_ = model_.revision();
    }
    auto it = expressions_.find(n.id);
    if (it == expressions_.end())
        it = expressions_.emplace(n.id, model_.tune().expression(model_.tune().tracks[static_cast<size_t>(track)], n)).first;
    return it->second;
}

// The level shades the block along time, darker where the note is quieter
// than its loudest moment, darkest 24 dB below it. The pitch is a thin line
// whose middle is the key on's pitch, two semitones to the block's half
// height, held at the edge with a dot where it goes further. Both draw only
// what is in view, at most one change per pixel column: a column takes its
// quietest level, and the line spans its lowest and highest pitch; after
// the column, both go on at the value the note holds. The shade is at most
// shadeAlpha dark, so it fades with the note.
void TrackerView::paintExpression(juce::Graphics& g, const NoteExpression& e, juce::Rectangle<float> r, juce::Colour line,
                                  float shadeAlpha) const
{
    const auto clip = g.getClipBounds().toFloat();
    const auto inside = r.reduced(1.0f);
    const float left = std::max(inside.getX(), clip.getX() - 1.0f), right = std::min(inside.getRight(), clip.getRight() + 1.0f);
    if (right <= left)
        return;
    auto xOf = [&](int64_t offset) {
        return std::min(inside.getRight(), r.getX() + static_cast<float>(static_cast<double>(offset) / samplesPerPixel_));
    };

    if (!e.level.empty())
    {
        constexpr float kDarkest = 24.0f; // dB below the loudest
        float peak = 0.0f;
        for (const auto& p : e.level)
            peak = std::max(peak, p.value);
        auto shade = [&](float from, float to, float value) {
            from = std::max(from, left);
            to = std::min(to, right);
            const float dark = std::clamp((peak - value) / kDarkest, 0.0f, 1.0f);
            if (to > from && dark > 0.02f)
            {
                g.setColour(juce::Colours::black.withAlpha(shadeAlpha * dark));
                g.fillRect(juce::Rectangle<float>(from, inside.getY(), to - from, inside.getHeight()));
            }
        };
        // The stretch not drawn yet starts with a one pixel column at its
        // quietest level, and then holds the level its last point leaves.
        float from = inside.getX(), low = 0.0f, held = 0.0f;
        auto stretch = [&](float to) {
            shade(from, std::min(from + 1.0f, to), low);
            shade(from + 1.0f, to, held);
        };
        for (const auto& p : e.level)
        {
            const float x = xOf(p.offset);
            if (x > right)
                break;
            if (x - from >= 1.0f || x < left)
            {
                stretch(x);
                from = x;
                low = held = p.value;
            }
            else
            {
                held = p.value;
                low = std::min(low, p.value);
            }
        }
        stretch(inside.getRight());
    }

    if (!e.pitch.empty())
    {
        constexpr float kRange = 2.0f; // semitones to the half height
        const float middle = inside.getCentreY(), half = std::max(1.0f, inside.getHeight() * 0.5f - 1.0f);
        auto yOf = [&](float value) { return middle - std::clamp(value, -kRange, kRange) / kRange * half; };
        juce::Path path;
        std::vector<juce::Point<float>> marks; // where the line is held at the edge
        float y = middle, column = 0.0f, low = 0.0f, high = 0.0f;
        bool started = false;
        auto start = [&] {
            column = std::max(inside.getX(), left);
            path.startNewSubPath(column, y);
            low = high = y;
            started = true;
        };
        for (const auto& p : e.pitch)
        {
            const float x = xOf(p.offset);
            if (x > right)
                break;
            const float to = yOf(p.value);
            if (x < left)
            {
                y = to;
                continue;
            }
            if (!started)
                start();
            if (x - column >= 1.0f)
            {
                path.lineTo(column, low);
                path.lineTo(column, high);
                path.lineTo(column, y);
                path.lineTo(x, y);
                column = x;
                low = high = y;
            }
            y = to;
            low = std::min(low, y);
            high = std::max(high, y);
            if (std::abs(p.value) > kRange && (marks.empty() || x - marks.back().x >= 4.0f))
                marks.push_back({ x, y });
        }
        if (!started)
            start();
        path.lineTo(column, low);
        path.lineTo(column, high);
        path.lineTo(column, y);
        path.lineTo(right, y);
        g.setColour(line);
        g.strokePath(path, juce::PathStrokeType(1.25f));
        for (const auto& m : marks)
            g.fillEllipse(m.x - 1.75f, m.y - 1.75f, 3.5f, 3.5f);
    }
}

// The meter: a dark groove of cells, always showing dimmed and lit up to
// the level, with a peak hold. As on a DAW's meter, cells are white up to
// -12 dB, yellow to -6, orange to -3 and red above; a silent track's whole
// meter goes neutral grey.
void TrackerView::paintMeter(juce::Graphics& g, juce::Rectangle<float> groove, float level, float hold, bool silent)
{
    const float corner = std::min(3.0f, groove.getHeight() * 0.5f);
    g.setColour(silent ? juce::Colour(0xff1d1e24) : juce::Colour(0xff1a1c24));
    g.fillRoundedRectangle(groove, corner);
    g.setColour(silent ? juce::Colour(0xff3a3b44) : theme::gridBar);
    g.drawRoundedRectangle(groove, corner, 1.0f);
    const auto cellsArea = groove.reduced(2.0f, std::min(2.0f, groove.getHeight() * 0.25f));
    const int cells = std::max(1, static_cast<int>(cellsArea.getWidth() / 3.0f));
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
        g.setColour(t % 2 ? theme::panel : theme::panelLight);
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

        // A header row: the name with mute and solo on its right, over the
        // meter, and the volume knob with the level in dB under it; compact
        // lanes keep only the name line. The track has one; opened, each of
        // its voices has one under it.
        auto strip = [&](int r, const juce::String& name, bool muted, bool soloed, float volume, float level, float hold, bool heard) {
            const bool dragging = drag_ == Drag::Knob && dragTrack_ == ti && dragRow_ == r;
            const bool silent = !heard || volume <= TuneTrack::kSilent;
            g.setColour(heard ? theme::text : theme::faint);
            g.setFont(theme::mono(minimal() ? 11.0f : 13.0f, true));
            g.drawText(name, nameBounds(ti, r), juce::Justification::centredLeft, true);
            drawToggle(pill(ti, 0, r), "M", muted, theme::orange);
            drawToggle(pill(ti, 1, r), "S", soloed, theme::cyan);
            if (compact())
                return;

            const auto knob = knobBounds(ti, r);
            const juce::String db = volume <= TuneTrack::kSilent ? juce::String("-inf dB") : (volume > 0.05f ? "+" : "") + juce::String(volume, 1) + " dB";
            g.setColour(dragging ? theme::cyan : silent ? theme::faint : std::abs(volume) < 0.05f ? theme::dim : theme::text);
            g.setFont(theme::mono(10.0f, true));
            g.drawText(db, juce::Rectangle<int>(knob.getCentreX() - kKnobColumn / 2, knob.getBottom(), kKnobColumn, 13), juce::Justification::centred);
            paintMeter(g, meterBounds(ti, r).toFloat(), level, hold, silent);

            // The volume knob: a ring from silence to +6 dB lit up to the
            // level, a tick at 0 dB, and a raised cap with a pointer.
            const auto bounds = knob.toFloat();
            const auto centre = bounds.getCentre();
            const float radius = bounds.getWidth() * 0.5f - 2.0f;
            const float angle = -kKnobArc + 2.0f * kKnobArc * knobPosition(volume);
            const juce::Colour accent = dragging ? theme::cyan.brighter(0.3f) : silent ? juce::Colour(0xff6a6c80) : theme::cyan;
            juce::Path ring;
            ring.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, -kKnobArc, kKnobArc, true);
            g.setColour(silent ? juce::Colour(0xff2c2d33) : juce::Colour(0xff2a2f4a));
            g.strokePath(ring, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            if (volume > TuneTrack::kSilent)
            {
                juce::Path value;
                value.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, -kKnobArc, angle, true);
                g.setColour(accent);
                g.strokePath(value, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            // 0 dB, as the game played it.
            const float zero = -kKnobArc + 2.0f * kKnobArc * knobPosition(0.0f);
            g.setColour(theme::text.withAlpha(0.8f));
            g.drawLine({ centre.getPointOnCircumference(radius - 2.5f, zero), centre.getPointOnCircumference(radius + 2.5f, zero) }, 1.2f);
            const auto cap = juce::Rectangle<float>(0, 0, (radius - 4.0f) * 2.0f, (radius - 4.0f) * 2.0f).withCentre(centre);
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillEllipse(cap.translated(0.0f, 1.5f));
            const juce::Colour light = silent ? juce::Colour(0xff6a6c84) : juce::Colour(0xffc9cbe8);
            const juce::Colour dark = silent ? juce::Colour(0xff34364a) : juce::Colour(0xff4f5378);
            g.setGradientFill(juce::ColourGradient(light, cap.getX(), cap.getY(), dark, cap.getX(), cap.getBottom(), false));
            g.fillEllipse(cap);
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.drawEllipse(cap.reduced(0.5f), 1.0f);
            g.setColour(dragging ? theme::cyan : theme::background);
            g.drawLine({ centre.getPointOnCircumference(cap.getWidth() * 0.12f, angle), centre.getPointOnCircumference(cap.getWidth() * 0.5f - 2.5f, angle) },
                       2.2f);
        };

        // The gear that opens an FM track's patch, or a drum track's sample: a toothed wheel with a hole.
        auto drawGear = [&] {
            const auto r = gear(ti).toFloat();
            const auto c = r.getCentre();
            const bool over = isMouseOver() && r.contains(getMouseXYRelative().toFloat());
            juce::Path wheel;
            for (int k = 0; k < 8; ++k)
            {
                const float a = static_cast<float>(k) * juce::MathConstants<float>::pi / 4.0f;
                juce::Path tooth;
                tooth.addRectangle(-1.6f, -7.0f, 3.2f, 3.0f);
                tooth.applyTransform(juce::AffineTransform::rotation(a).translated(c.x, c.y));
                wheel.addPath(tooth);
            }
            wheel.addEllipse(c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f);
            wheel.addEllipse(c.x - 2.0f, c.y - 2.0f, 4.0f, 4.0f);
            wheel.setUsingNonZeroWinding(false);
            g.setColour(over ? theme::text : theme::dim);
            g.fillPath(wheel);
        };

        // A bin that removes a track or a voice: a lid with its handle, and a
        // tapered body with two slots.
        auto drawBin = [&](juce::Rectangle<int> box) {
            if (box.isEmpty())
                return;
            const auto r = box.toFloat();
            const bool over = isMouseOver() && r.contains(getMouseXYRelative().toFloat());
            const auto c = r.getCentre();
            juce::Path bin;
            bin.addRoundedRectangle(c.x - 5.5f, c.y - 4.6f, 11.0f, 1.8f, 0.6f);
            bin.addRoundedRectangle(c.x - 2.0f, c.y - 6.4f, 4.0f, 1.8f, 0.6f);
            juce::Path body;
            body.addQuadrilateral(c.x - 4.4f, c.y - 2.0f, c.x + 4.4f, c.y - 2.0f, c.x + 3.6f, c.y + 6.4f, c.x - 3.6f, c.y + 6.4f);
            bin.addPath(body.createPathWithRoundedCorners(1.2f));
            bin.addRectangle(c.x - 2.2f, c.y - 0.2f, 1.2f, 4.8f);
            bin.addRectangle(c.x + 1.0f, c.y - 0.2f, 1.2f, 4.8f);
            bin.setUsingNonZeroWinding(false);
            g.setColour(over ? theme::text : theme::dim);
            g.fillPath(bin);
        };

        const bool audible = model_.tune().audible(tr);
        if (!labelled(ti))
        {
            // One row: the track's.
            strip(0, juce::String(tr.name).toUpperCase(), tr.mute, tr.solo, tr.volume, t < level_.size() ? level_[t] : 0.0f,
                  t < hold_.size() ? hold_[t] : 0.0f, audible);
            drawGear();
            drawBin(removeTrackButton(ti));
            continue;
        }

        // A label strip with the track's name and gear, then a row per voice.
        g.setColour(theme::background.withAlpha(0.35f));
        g.fillRect(kChipColumn, y, headerWidth() - 1 - kChipColumn, kLabelHeight);
        g.setColour(audible ? theme::text : theme::faint);
        g.setFont(theme::mono(12.0f, true));
        g.drawText(juce::String(tr.name).toUpperCase(), label(ti), juce::Justification::centredLeft, false);
        drawGear();
        // A plus in a ring that adds a voice.
        if (const auto plus = addVoiceButton(ti).toFloat(); !plus.isEmpty())
        {
            const bool over = isMouseOver() && plus.contains(getMouseXYRelative().toFloat());
            const auto c = plus.getCentre();
            juce::Path ring;
            ring.addEllipse(c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f);
            ring.addEllipse(c.x - 5.6f, c.y - 5.6f, 11.2f, 11.2f);
            ring.setUsingNonZeroWinding(false);
            g.setColour(over ? theme::text : theme::dim);
            g.fillPath(ring);
            g.fillRoundedRectangle(c.x - 3.6f, c.y - 0.8f, 7.2f, 1.6f, 0.5f);
            g.fillRoundedRectangle(c.x - 0.8f, c.y - 3.6f, 1.6f, 7.2f, 0.5f);
        }
        drawBin(removeTrackButton(ti));
        for (size_t k = 0; k < voices.size(); ++k)
        {
            const int r = static_cast<int>(k);
            const auto bit = static_cast<uint16_t>(1 << voices[k]);
            g.setColour(theme::grid.withAlpha(0.6f));
            g.fillRect(kChipColumn, rowTop(ti, r), headerWidth() - 1 - kChipColumn, 1);
            // Heard as Tune::audible has a note on this voice heard.
            const bool heard = audible && tr.volume > TuneTrack::kSilent && (tr.mutedVoices & bit) == 0
                               && (tr.solo || tr.soloedVoices == 0 || (tr.soloedVoices & bit) != 0);
            const auto& lv = t < voiceLevel_.size() ? voiceLevel_[t] : std::vector<float> {};
            const auto& hv = t < voiceHold_.size() ? voiceHold_[t] : std::vector<float> {};
            strip(r, "V" + juce::String(voices[k] + 1), (tr.mutedVoices & bit) != 0, (tr.soloedVoices & bit) != 0,
                  tr.voiceVolume[static_cast<size_t>(voices[k])], k < lv.size() ? lv[k] : 0.0f, k < hv.size() ? hv[k] : 0.0f, heard);
            drawBin(removeVoiceButton(ti, r));
        }
    }
    // The button that adds a track, under the last.
    {
        const auto b = addTrackButton().toFloat();
        const bool over = isMouseOver() && b.contains(getMouseXYRelative().toFloat());
        g.setColour(over ? theme::text : theme::dim);
        g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);
        g.setFont(theme::mono(11.0f, true));
        g.drawText("+ TRACK", b, juce::Justification::centred, false);
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

void TrackerView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (!model_.hasTune())
        return;
    const auto p = e.position;
    dragFrom_ = p;
    dragDelta_ = 0;
    dropTrack_ = dropRow_ = -1;
    dropShift_ = 0;

    if (p.y < kRulerHeight && p.x >= lanesX())
    {
        // A loop handle drags that end; Shift drags out a new loop; else the playhead.
        const int edge = loopEdgeAt(p);
        if (edge != 0)
            drag_ = edge < 0 ? Drag::LoopStart : Drag::LoopEnd;
        else if (e.mods.isShiftDown())
        {
            drag_ = Drag::Loop;
            loopAnchor_ = std::clamp(snapTime(xToTime(p.x)), 0.0, static_cast<double>(model_.tune().length()));
        }
        else
        {
            drag_ = Drag::Ruler;
            model_.setPosition(std::max(0.0, xToTime(p.x)));
        }
        return;
    }

    const int t = yToTrack(p.y);
    if (p.x < lanesX())
    {
        if (addTrackButton().contains(p.toInt()))
            return addTrack();
        if (t < 0)
            return;
        if (addVoiceButton(t).contains(p.toInt()))
            return addVoice(t);
        if (removeTrackButton(t).contains(p.toInt()))
            return removeTrack(t);
        if (const int r = headerRow(t, p.y); r >= 0 && removeVoiceButton(t, r).contains(p.toInt()))
            return removeVoice(t, r);
        // The header row clicked: the track's, or one of its voices'; -1 a label strip.
        // A voice's row is where a paste goes.
        const int r = headerRow(t, p.y);
        if (r >= 0)
        {
            pasteTrack_ = fmTrack(t) && labelled(t) ? t : -1;
            pasteVoice_ = pasteTrack_ >= 0 ? layout().voices[static_cast<size_t>(t)][static_cast<size_t>(r)] : -1;
        }
        // The knob turns as the pointer goes up or right; Option-click puts it back to 0 dB.
        if (r >= 0 && !compact() && knobBounds(t, r).expanded(3).contains(p.toInt()))
        {
            drag_ = Drag::Knob;
            dragTrack_ = t;
            dragRow_ = r;
            dragStarted_ = false;
            dragKnob_ = knobPosition(rowVolume(t, r));
            if (e.mods.isAltDown())
                setVolume(t, r, 0.0f);
            else
                repaint();
            return;
        }
        if (labelled(t))
        {
            // A voice's mute or solo.
            for (const int which : { 0, 1 })
                if (r >= 0 && pill(t, which, r).contains(p.toInt()))
                {
                    const auto bit = static_cast<uint16_t>(1 << layout().voices[static_cast<size_t>(t)][static_cast<size_t>(r)]);
                    model_.edit(which == 0 ? "Mute voice" : "Solo voice", [t, bit, which](Tune& tune) {
                        auto& tr = tune.tracks[static_cast<size_t>(t)];
                        (which == 0 ? tr.mutedVoices : tr.soloedVoices) ^= bit;
                    });
                    return;
                }
        }
        // The gear: an FM track's patch, or a drum track's sample; the track's mute or solo.
        if (gear(t).expanded(2).contains(p.toInt()))
        {
            const bool fm = model_.tune().tracks[static_cast<size_t>(t)].kind == TuneTrack::Kind::Fm;
            if (fm && onOpenPatch)
                onOpenPatch(t);
            else if (!fm && onOpenSample)
                onOpenSample(t);
            return;
        }
        const bool mute = !labelled(t) && pill(t, 0).contains(p.toInt());
        const bool solo = !labelled(t) && pill(t, 1).contains(p.toInt());
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
        // A note clicked sounds, though not again as it is dragged.
        if (!e.mods.isShiftDown())
            preview(hit);
        dragNote_ = hit;
        fromRow_ = layout().row[static_cast<size_t>(hit.track)][static_cast<size_t>(indexOf(hit))];
        drag_ = edge ? Drag::Resize : Drag::Move;
    }
    else
    {
        if (!e.mods.isShiftDown())
            selection_.clear();
        drag_ = Drag::Box;
        box_ = { p, p };
        // An empty spot clicked on a voice's row is where a paste goes; a box drag leaves it.
        const int r = t >= 0 && fmTrack(t) && labelled(t) ? headerRow(t, p.y) : -1;
        pressTrack_ = r >= 0 ? t : -1;
        pressVoice_ = r >= 0 ? layout().voices[static_cast<size_t>(t)][static_cast<size_t>(r)] : -1;
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
        setVolume(dragTrack_, dragRow_, knobVolume(position));
        return;
    }
    if (drag_ == Drag::Ruler)
    {
        model_.setPosition(std::max(0.0, xToTime(p.x)));
        return;
    }
    if (drag_ == Drag::Loop || drag_ == Drag::LoopStart || drag_ == Drag::LoopEnd)
    {
        // On the snap grid, within the song, never shorter than the shortest note.
        const double length = static_cast<double>(model_.tune().length());
        const double t = std::clamp(snapTime(xToTime(p.x)), 0.0, length);
        LoopRegion loop = model_.loop();
        if (drag_ == Drag::Loop)
        {
            if (std::abs(t - loopAnchor_) < kMinLength)
                return;
            loop.start = std::llround(std::min(t, loopAnchor_));
            loop.end = std::llround(std::max(t, loopAnchor_));
            loop.on = true;
        }
        else if (drag_ == Drag::LoopStart)
            loop.start = std::llround(std::clamp(t, 0.0, std::max(0.0, static_cast<double>(loop.end) - kMinLength)));
        else
            loop.end = std::llround(std::clamp(t, std::min(length, static_cast<double>(loop.start) + kMinLength), length));
        if (loop.start != model_.loop().start || loop.end != model_.loop().end || loop.on != model_.loop().on)
            model_.setLoop(loop);
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
        {
            dragDelta_ = snapTime(static_cast<double>(n.start) + raw) - static_cast<double>(n.start);
            updateDrop(p);
        }
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

void TrackerView::mouseUp(const juce::MouseEvent& e)
{
    const Drag finished = drag_;
    drag_ = Drag::None;
    // A click on empty lanes, not a box drag, puts the stopped playhead there
    // and picks the row a paste goes into.
    if (finished == Drag::Box && e.mouseWasClicked())
    {
        pasteTrack_ = pressTrack_, pasteVoice_ = pressVoice_;
        if (!model_.playing())
            model_.setPosition(std::max(0.0, xToTime(e.position.x)));
    }
    if (finished == Drag::Move && dropTrack_ >= 0)
    {
        // Across rows: each selected note that lands on another row goes to
        // that row's voice, in its track or another, with the time move.
        struct Move
        {
            NoteRef from;
            int track, voice;
        };
        std::vector<Move> moves;
        for (const NoteRef& ref : selection_)
            if (const int i = indexOf(ref); i >= 0)
            {
                const int row = layout().row[static_cast<size_t>(ref.track)][static_cast<size_t>(i)];
                const auto [track, to] = landing(ref.track, row);
                const auto& voices = layout().voices[static_cast<size_t>(track)];
                if ((track != ref.track || to != row) && static_cast<size_t>(to) < voices.size())
                    moves.push_back({ ref, track, voices[static_cast<size_t>(to)] });
            }
        const auto chosen = selection_;
        const auto delta = static_cast<int64_t>(std::llround(dragDelta_));
        std::set<NoteRef> landed = selection_;
        const bool done = model_.edit("Move notes", [chosen, delta, moves, &landed](Tune& tune) {
            std::vector<std::vector<int>> before;
            for (const TuneTrack& t : tune.tracks)
                before.push_back(tune.voices(t));
            for (size_t t = 0; t < tune.tracks.size(); ++t)
                for (TuneNote& n : tune.tracks[t].notes)
                    if (chosen.count({ static_cast<int>(t), n.id }))
                        n.start = std::max<int64_t>(0, n.start + delta);
            for (const Move& m : moves)
            {
                TuneTrack& from = tune.tracks[static_cast<size_t>(m.from.track)];
                const auto it = std::find_if(from.notes.begin(), from.notes.end(), [&](const TuneNote& n) { return n.id == m.from.id; });
                if (it == from.notes.end())
                    continue;
                if (m.track == m.from.track)
                {
                    tune.moveToVoice(from, *it, m.voice);
                    continue;
                }
                // Into another track, a written note at the pitch it sounds at.
                const TuneNote moved = tune.writtenCopy(from, *it, m.voice);
                from.notes.erase(it);
                tune.tracks[static_cast<size_t>(m.track)].notes.push_back(moved);
                landed.erase(m.from);
                landed.insert({ m.track, moved.id });
            }
            // The voices the notes left stay with their tracks, as rows to put notes on.
            for (size_t t = 0; t < tune.tracks.size(); ++t)
            {
                TuneTrack& track = tune.tracks[t];
                const std::vector<int> now = tune.voices(track);
                for (const int v : before[t])
                    if (std::find(now.begin(), now.end(), v) == now.end())
                        track.addedVoices = static_cast<uint8_t>(track.addedVoices | 1 << v);
                std::stable_sort(track.notes.begin(), track.notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
            }
        });
        if (done)
            selection_ = landed;
    }
    else if ((finished == Drag::Move || finished == Drag::Resize) && std::abs(dragDelta_) >= 1.0)
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
    dropTrack_ = dropRow_ = -1;
    dropShift_ = 0;
    repaint();
}

void TrackerView::updateDrop(juce::Point<float> p)
{
    // Only notes of an FM track with voice rows move across rows.
    const int from = dragNote_.track;
    if (!fmTrack(from) || !labelled(from))
        return;
    const int t = yToTrack(p.y);
    if (t < 0 || !fmTrack(t) || !labelled(t))
    {
        dropTrack_ = dropRow_ = -1;
        dropShift_ = 0;
        repaint();
        return;
    }
    // Over a label strip, the row stays as it was.
    const int r = headerRow(t, p.y);
    if (r < 0)
        return;
    if (t == from && r == fromRow_)
        dropTrack_ = dropRow_ = -1;
    else
        dropTrack_ = t, dropRow_ = r;
    // Within their tracks, the selected notes move by the same rows, as far
    // as every one of them stays on a row of its track.
    dropShift_ = 0;
    if (dropTrack_ == from)
    {
        int low = -fromRow_, high = static_cast<int>(layout().voices[static_cast<size_t>(from)].size()) - 1 - fromRow_;
        for (const NoteRef& ref : selection_)
            if (const int i = indexOf(ref); i >= 0 && fmTrack(ref.track) && labelled(ref.track))
            {
                const int row = layout().row[static_cast<size_t>(ref.track)][static_cast<size_t>(i)];
                low = std::max(low, -row);
                high = std::min(high, static_cast<int>(layout().voices[static_cast<size_t>(ref.track)].size()) - 1 - row);
            }
        dropShift_ = low <= high ? std::clamp(dropRow_ - fromRow_, low, high) : 0;
        if (dropShift_ == 0)
            dropTrack_ = dropRow_ = -1;
    }
    repaint();
}

std::pair<int, int> TrackerView::landing(int track, int row) const
{
    if (dropTrack_ < 0 || !fmTrack(track) || !labelled(track))
        return { track, row };
    if (dropTrack_ == dragNote_.track)
        return { track, row + dropShift_ };
    // Into another track: the dragged note's track's notes, keeping their
    // rows apart as far as the track has rows.
    if (track != dragNote_.track)
        return { track, row };
    const int rows = static_cast<int>(layout().voices[static_cast<size_t>(dropTrack_)].size());
    return { dropTrack_, std::clamp(dropRow_ + row - fromRow_, 0, rows - 1) };
}

int TrackerView::loopEdgeAt(juce::Point<float> p) const
{
    const LoopRegion& loop = model_.loop();
    if (!loop.valid() || p.y >= kRulerHeight || p.x < lanesX() - 4.0f)
        return 0;
    // The nearer handle within reach, the end first when both are, so a
    // short loop can still be made longer.
    const float start = std::abs(p.x - timeToX(static_cast<double>(loop.start)));
    const float end = std::abs(p.x - timeToX(static_cast<double>(loop.end)));
    constexpr float kReach = 6.0f;
    return end <= kReach && end <= start ? 1 : start <= kReach ? -1 : 0;
}

void TrackerView::mouseMove(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
        return;
    bool edge = false;
    hover_ = noteAt(e.position, edge);
    hoverEdge_ = edge;
    const int loopEdge = hover_.valid() ? 0 : loopEdgeAt(e.position);
    setMouseCursor((hover_.valid() && edge) || loopEdge != 0 ? juce::MouseCursor::LeftRightResizeCursor
                   : hover_.valid()       ? juce::MouseCursor::DraggingHandCursor
                   : e.position.y < kRulerHeight && e.position.x > lanesX() ? juce::MouseCursor::PointingHandCursor
                                                                                 : juce::MouseCursor::NormalCursor);
    if (hover_.valid())
    {
        const int i = indexOf(hover_);
        const auto& tr = model_.tune().tracks[static_cast<size_t>(hover_.track)];
        const auto& n = tr.notes[static_cast<size_t>(i)];
        // A note that moves after its key on says how, after the rest.
        const juce::String moves = expressionSummary(expressionOf(hover_.track, n));
        // Its expression, where set, comes before.
        const juce::String set = expressionText(n, tr.kind == TuneTrack::Kind::Drum);
        if (tr.kind == TuneTrack::Kind::Fm)
            setTooltip(theme::noteName(model_.tune().pitch(tr, n)) + theme::dot() + juce::String(static_cast<double>(n.length) / kVgmRate * 1000.0, 0)
                       + " ms" + (set.isEmpty() ? juce::String() : theme::dot() + set) + (moves.isEmpty() ? juce::String() : theme::dot() + moves));
        else
        {
            // A drum hit: the PCM channel it plays on, its step and the rate
            // that gives, its transposition and its volumes at the trigger,
            // with its expression.
            const auto& channels = model_.report().channels;
            const auto t = static_cast<size_t>(hover_.track), k = static_cast<size_t>(i);
            int ch = t < channels.size() && k < channels[t].size() ? channels[t][k] : -1;
            if (ch < 0)
                ch = model_.tune().originalChannel(tr, n);
            const PcmVoice v = model_.tune().hitVoice(tr, n, true);
            const double hz = v.regs[5] / 256.0 * model_.tune().source().pcmRate;
            setTooltip("PCM ch " + juce::String(ch + 1) + ", step 0x" + juce::String::toHexString(v.regs[5]).toUpperCase().paddedLeft('0', 2) + " ~ "
                       + juce::String(juce::roundToInt(hz)) + " Hz, " + semitones(n.transpose) + " st, L/R " + juce::String(v.regs[0] & 0x7F) + "/"
                       + juce::String(v.regs[1] & 0x7F) + (set.isEmpty() ? juce::String() : ", " + set) + (moves.isEmpty() ? juce::String() : ", " + moves));
        }
    }
    else
    {
        // Over a track's gear: what it opens.
        const int t = e.position.x < lanesX() ? yToTrack(e.position.y) : -1;
        const bool overGear = t >= 0 && gear(t).expanded(2).contains(e.position.toInt());
        const bool drumGear = overGear && model_.tune().tracks[static_cast<size_t>(t)].kind == TuneTrack::Kind::Drum;
        const bool overPlus = t >= 0 && addVoiceButton(t).contains(e.position.toInt());
        const bool overRemove = t >= 0 && removeTrackButton(t).contains(e.position.toInt());
        const bool overRemoveVoice = t >= 0 && headerRow(t, e.position.y) >= 0
                                     && removeVoiceButton(t, headerRow(t, e.position.y)).contains(e.position.toInt());
        const bool overAdd = e.position.x < lanesX() && addTrackButton().contains(e.position.toInt());
        // A row's mute, solo, knob and meter: the track's, or with voices a voice's.
        const int r = t < 0 ? -1 : labelled(t) ? headerRow(t, e.position.y) : 0;
        const bool overMute = r >= 0 && pill(t, 0, r).contains(e.position.toInt());
        const bool overSolo = r >= 0 && pill(t, 1, r).contains(e.position.toInt());
        const bool overKnob = r >= 0 && !compact() && knobBounds(t, r).expanded(3).contains(e.position.toInt());
        const bool overMeter = r >= 0 && !compact() && meterBounds(t, r).contains(e.position.toInt());
        const bool overRuler = e.position.y < kRulerHeight && e.position.x >= lanesX();
        setTooltip(loopEdge < 0      ? "Loop start"
                   : loopEdge > 0    ? "Loop end"
                   : overRuler       ? "Click or drag to move the playhead; Shift-drag to set a loop"
                   : drumGear        ? "Sample"
                   : overGear        ? "Patch"
                   : overPlus        ? "Add a voice"
                   : overAdd         ? "Add a track"
                   : overRemove      ? "Remove the track"
                   : overRemoveVoice ? "Remove the voice"
                   : overMute        ? "Mute"
                   : overSolo        ? "Solo"
                   : overKnob        ? "Volume; double-click or Option-click for 0 dB"
                   : overMeter       ? "Level"
                                     : juce::String());
    }
}

float TrackerView::rowVolume(int track, int row) const
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track)];
    if (!labelled(track))
        return tr.volume;
    return tr.voiceVolume[static_cast<size_t>(layout().voices[static_cast<size_t>(track)][static_cast<size_t>(row)])];
}

void TrackerView::setVolume(int track, int row, float volume)
{
    // In tenths of a dB: a drag edits only when the level changes, as one undo step.
    volume = volume <= TuneTrack::kSilent ? TuneTrack::kSilent : std::round(juce::jlimit(TuneTrack::kSilent, TuneTrack::kMaxVolume, volume) * 10.0f) / 10.0f;
    if (std::abs(rowVolume(track, row) - volume) < 0.01f)
    {
        repaint();
        return;
    }
    const int voice = labelled(track) ? layout().voices[static_cast<size_t>(track)][static_cast<size_t>(row)] : -1;
    model_.edit(voice < 0 ? "Volume" : "Voice volume", [track, voice, volume](Tune& tune) {
        auto& tr = tune.tracks[static_cast<size_t>(track)];
        (voice < 0 ? tr.volume : tr.voiceVolume[static_cast<size_t>(voice)]) = volume;
    }, !dragStarted_);
    dragStarted_ = true;
}

void TrackerView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (model_.hasTune() && e.position.x < lanesX())
        if (const int t = yToTrack(e.position.y); t >= 0 && !compact())
            if (const int r = headerRow(t, e.position.y); r >= 0 && knobBounds(t, r).expanded(3).contains(e.position.toInt()))
            {
                dragStarted_ = false;
                setVolume(t, r, 0.0f);
                return;
            }
    if (!model_.hasTune() || e.position.x < lanesX() || e.position.y < kRulerHeight)
        return;
    bool edge = false;
    const NoteRef hit = noteAt(e.position, edge);
    if (hit.valid())
        editNote(hit);
    else if (const int t = yToTrack(e.position.y); t >= 0)
        addNoteAt(t, headerRow(t, e.position.y), snapTime(xToTime(e.position.x)));
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
        // With Option over a selected note, the wheel changes its pitch: one
        // semitone a notch, an octave with Shift. Elsewhere it zooms the tracks' height.
        bool edge = false;
        const NoteRef hit = noteAt(e.position, edge);
        if (hit.valid() && selection_.count(hit) && std::abs(w.deltaY) > std::abs(w.deltaX))
        {
            wheelPitch_ += w.deltaY;
            if (std::abs(wheelPitch_) >= 0.12f)
            {
                transposeSelection((wheelPitch_ > 0 ? 1 : -1) * (e.mods.isShiftDown() ? 12 : 1));
                wheelPitch_ = 0;
            }
            return;
        }
        zoomLanes(std::pow(1.6, static_cast<double>(w.deltaY) * 2.0), e.position.y);
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

void TrackerView::selectAll()
{
    if (!model_.hasTune())
        return;
    selection_.clear();
    const auto& tracks = model_.tune().tracks;
    for (size_t t = 0; t < tracks.size(); ++t)
        for (const TuneNote& n : tracks[t].notes)
            selection_.insert({ static_cast<int>(t), n.id });
    repaint();
}

void TrackerView::deselect()
{
    selection_.clear();
    repaint();
}

void TrackerView::step(int direction)
{
    if (!model_.hasTune())
        return;
    const double length = snap_ > 0 ? snap_ : model_.tune().beatLength() / 4;
    // With nothing selected, the playhead moves one step of the snap.
    if (selection_.empty())
    {
        model_.setPosition(model_.displayPosition() + direction * length);
        reveal(model_.position());
        return;
    }
    const auto delta = static_cast<int64_t>(std::llround(direction * length));
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
}

void TrackerView::transposeSelection(int semitones)
{
    if (selection_.empty())
        return;
    const auto chosen = selection_;
    const bool done = model_.edit("Transpose", [chosen, semitones](Tune& tune) {
        for (size_t t = 0; t < tune.tracks.size(); ++t)
            for (TuneNote& n : tune.tracks[t].notes)
                if (chosen.count({ static_cast<int>(t), n.id }))
                    n.transpose = std::clamp(n.transpose + semitones, -48, 48);
    });
    // The first FM note at its new pitch: a drum hit has no preview.
    if (done)
        if (const auto it = std::find_if(chosen.begin(), chosen.end(), [this](const NoteRef& ref) { return fmTrack(ref.track); }); it != chosen.end())
            preview(*it);
}

bool TrackerView::fmTrack(int track) const
{
    return model_.hasTune() && track >= 0 && static_cast<size_t>(track) < model_.tune().tracks.size()
           && model_.tune().tracks[static_cast<size_t>(track)].kind == TuneTrack::Kind::Fm;
}

int TrackerView::selectedTrack() const
{
    // The notes go by track index, so a drum track's can come before an FM track's.
    for (const NoteRef& ref : selection_)
        if (fmTrack(ref.track))
            return ref.track;
    return selection_.empty() ? -1 : selection_.begin()->track;
}

void TrackerView::preview(NoteRef ref)
{
    if (const int i = indexOf(ref); i >= 0)
        model_.previewNote(ref.track, model_.tune().tracks[static_cast<size_t>(ref.track)].notes[static_cast<size_t>(i)]);
}

void TrackerView::deleteSelection()
{
    removeSelection("Delete notes");
}

void TrackerView::removeSelection(const juce::String& name)
{
    if (selection_.empty())
        return;
    const auto chosen = selection_;
    const bool done = model_.edit(name, [chosen](Tune& tune) {
        for (size_t t = 0; t < tune.tracks.size(); ++t)
        {
            auto& notes = tune.tracks[t].notes;
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [&](const TuneNote& n) { return chosen.count({ static_cast<int>(t), n.id }) > 0; }),
                        notes.end());
        }
    });
    if (done)
        selection_.clear();
}

void TrackerView::selectionSpan(int64_t& first, int64_t& last) const
{
    const auto& tracks = model_.tune().tracks;
    first = std::numeric_limits<int64_t>::max(), last = 0;
    for (const NoteRef& r : selection_)
        if (const int i = indexOf(r); i >= 0)
        {
            const TuneNote& n = tracks[static_cast<size_t>(r.track)].notes[static_cast<size_t>(i)];
            first = std::min(first, n.start);
            last = std::max(last, n.start + n.length);
        }
}

LoopRegion TrackerView::selectionLoop() const
{
    LoopRegion loop;
    if (selection_.empty() || !model_.hasTune())
        return loop;
    int64_t first = 0, last = 0;
    selectionSpan(first, last);
    // Out to the snap grid's lines either side, or the beats' with the snap off.
    const double step = snap_ > 0 ? snap_ : model_.tune().beatLength();
    const double origin = model_.tune().firstBeat();
    loop.start = std::max<int64_t>(0, std::llround(origin + std::floor((static_cast<double>(first) - origin) / step) * step));
    loop.end = std::llround(origin + std::ceil((static_cast<double>(last) - origin) / step) * step);
    loop.on = loop.valid();
    return loop;
}

void TrackerView::duplicateSelection()
{
    if (selection_.empty())
        return;
    // The copy goes right after the selection, rounded up to a whole bar.
    int64_t first = 0, last = 0;
    selectionSpan(first, last);
    const double bar = model_.tune().barLength();
    const auto shift = static_cast<int64_t>(std::ceil(static_cast<double>(last - first) / bar) * bar);
    const auto chosen = selection_;
    std::set<NoteRef> copies;
    const bool done = model_.edit("Duplicate notes", [chosen, shift, &copies](Tune& tune) {
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
    if (done)
        selection_ = copies;
    repaint();
}

void TrackerView::copySelection()
{
    if (selection_.empty() || !model_.hasTune())
        return;
    int64_t first = 0, last = 0;
    selectionSpan(first, last);
    const auto& tracks = model_.tune().tracks;
    clipboard_.clear();
    for (const NoteRef& ref : selection_)
        if (const int i = indexOf(ref); i >= 0)
        {
            const TuneTrack& tr = tracks[static_cast<size_t>(ref.track)];
            ClipNote c { ref.track, layout().row[static_cast<size_t>(ref.track)][static_cast<size_t>(i)], tr.kind, tr.name,
                         tr.notes[static_cast<size_t>(i)] };
            c.note.start -= first;
            clipboard_.push_back(c);
        }
    clipSong_ = model_.tune().sourcePointer();
    pasteTrack_ = pasteVoice_ = -1;
}

void TrackerView::cutSelection()
{
    // Refused whole while playing, so the clipboard never holds notes still in the song.
    if (selection_.empty() || refusedWhilePlaying())
        return;
    copySelection();
    removeSelection("Cut notes");
}

bool TrackerView::canPaste() const
{
    return model_.hasTune() && !clipboard_.empty() && clipSong_.lock() == model_.tune().sourcePointer();
}

void TrackerView::paste()
{
    if (!canPaste())
        return;
    const Tune& tune = model_.tune();
    const auto at = std::max<int64_t>(0, std::llround(snapTime(model_.position())));
    // A copied note's track: where it was, else by its name and kind, as
    // tracks before it may have gone.
    auto trackOf = [&tune](const ClipNote& c) {
        auto same = [&c](const TuneTrack& t) { return t.kind == c.kind && t.name == c.name; };
        if (c.track >= 0 && static_cast<size_t>(c.track) < tune.tracks.size() && same(tune.tracks[static_cast<size_t>(c.track)]))
            return c.track;
        const auto it = std::find_if(tune.tracks.begin(), tune.tracks.end(), same);
        return it == tune.tracks.end() ? -1 : static_cast<int>(it - tune.tracks.begin());
    };
    // Into the row clicked since the copy, when every note comes from one FM track.
    const bool oneTrack = std::all_of(clipboard_.begin(), clipboard_.end(), [this](const ClipNote& c) {
        return c.kind == TuneTrack::Kind::Fm && c.track == clipboard_.front().track && c.name == clipboard_.front().name;
    });
    int targetRow = -1;
    if (oneTrack && fmTrack(pasteTrack_) && labelled(pasteTrack_))
    {
        const auto& voices = layout().voices[static_cast<size_t>(pasteTrack_)];
        if (const auto it = std::find(voices.begin(), voices.end(), pasteVoice_); it != voices.end())
            targetRow = static_cast<int>(it - voices.begin());
    }
    int topRow = std::numeric_limits<int>::max();
    for (const ClipNote& c : clipboard_)
        topRow = std::min(topRow, c.row);

    struct Placed
    {
        int track;
        TuneNote note;
    };
    std::vector<Placed> placed;
    size_t lost = 0;
    for (const ClipNote& c : clipboard_)
    {
        TuneNote n = c.note;
        n.start += at;
        const int own = trackOf(c);
        if (targetRow < 0)
        {
            if (own < 0)
                ++lost;
            else
                placed.push_back({ own, n });
            continue;
        }
        // The rows keep apart as far as the track has rows; in another track
        // a ripped note is written, at the pitch it sounds at.
        const auto& voices = layout().voices[static_cast<size_t>(pasteTrack_)];
        const int row = std::clamp(targetRow + c.row - topRow, 0, static_cast<int>(voices.size()) - 1);
        const int voice = voices[static_cast<size_t>(row)];
        const TuneTrack& target = tune.tracks[static_cast<size_t>(pasteTrack_)];
        if (own == pasteTrack_)
            tune.moveToVoice(target, n, voice);
        else
            n = tune.writtenCopy(TuneTrack {}, n, voice);
        placed.push_back({ pasteTrack_, n });
    }
    if (placed.empty())
    {
        if (model_.onNotice)
            model_.onNotice("The copied notes' tracks are gone");
        return;
    }

    std::set<NoteRef> pasted;
    NoteRef first;
    int64_t earliest = std::numeric_limits<int64_t>::max();
    const bool done = model_.edit("Paste notes", [&placed, &pasted, &first, &earliest](Tune& changed) {
        int id = changed.newNoteId();
        for (Placed& p : placed)
        {
            p.note.id = id++;
            changed.tracks[static_cast<size_t>(p.track)].notes.push_back(p.note);
            pasted.insert({ p.track, p.note.id });
            // The earliest FM note is the one heard.
            if (changed.tracks[static_cast<size_t>(p.track)].kind == TuneTrack::Kind::Fm && p.note.start < earliest)
                earliest = p.note.start, first = { p.track, p.note.id };
        }
        for (TuneTrack& t : changed.tracks)
            std::stable_sort(t.notes.begin(), t.notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
    });
    if (!done)
        return;
    selection_ = pasted;
    if (first.valid())
        preview(first);
    if (lost > 0 && model_.onNotice)
        model_.onNotice(juce::String(lost) + (lost == 1 ? " copied note's track is gone" : " copied notes' tracks are gone"));
    repaint();
}

void TrackerView::moveConflicts()
{
    if (!model_.hasTune())
        return;
    // Tried on a copy first, so a command that moves nothing makes no step of undo.
    Tune moved = model_.tune();
    size_t stuck = 0;
    const size_t count = moved.moveConflicts(stuck);
    if (count > 0 && !model_.edit("Move conflicts", [tracks = moved.tracks](Tune& tune) { tune.tracks = tracks; }))
        return;
    juce::String notice = "No conflicts";
    if (count > 0 || stuck > 0)
        notice = "Moved " + juce::String(count) + (count == 1 ? " note" : " notes") + " to free voices, " + juce::String(stuck)
                 + " could not move";
    // A note with no free channel shares one wherever it goes.
    if (stuck > 0)
        notice << (stuck == 1 ? ": every voice is busy when it starts" : ": every voice is busy when they start");
    if (model_.onNotice)
        model_.onNotice(notice);
}

void TrackerView::editNote(NoteRef ref)
{
    const int i = indexOf(ref);
    if (i < 0)
        return;
    const Tune& tune = model_.tune();
    const auto& tr = tune.tracks[static_cast<size_t>(ref.track)];
    const TuneNote n = tr.notes[static_cast<size_t>(i)];
    // A drum hit's pitch is its transposition from the sample's own; a
    // note's is its key, the fine tune on top of it.
    const bool drum = tr.kind == TuneTrack::Kind::Drum;
    TuneNote key = n;
    key.fine = 0;
    const NoteEditor::Values current { drum ? n.transpose : tune.pitch(tr, key), n.fine, n.velocity, n.pan };
    // Opened on a selected note, it edits every selected note: each moves by
    // the change to its pitch, fine tune and velocity, and takes the pan.
    const std::set<NoteRef> chosen = selection_.count(ref) > 0 ? selection_ : std::set<NoteRef> { ref };
    auto changed = [current](TuneNote x, const NoteEditor::Values& v) {
        x.transpose = std::clamp(x.transpose + v.pitch - current.pitch, -48, 48);
        x.fine = std::clamp(x.fine + v.fine - current.fine, -TuneNote::kMaxFine, TuneNote::kMaxFine);
        x.velocity = std::clamp(x.velocity + v.velocity - current.velocity, TuneNote::kMinVelocity, 0);
        if (v.pan != current.pan)
            x.pan = v.pan;
        return x;
    };
    juce::Component::SafePointer<TrackerView> self(this);
    auto preview = [self, ref, n, changed](const NoteEditor::Values& v) {
        if (self != nullptr)
            self->preview(ref, changed(n, v));
    };
    auto apply = [self, chosen, changed](const NoteEditor::Values& v) {
        if (self == nullptr)
            return true;
        return self->model_.edit("Edit note", [&chosen, &changed, &v](Tune& t) {
            for (size_t k = 0; k < t.tracks.size(); ++k)
                for (TuneNote& x : t.tracks[k].notes)
                    if (chosen.count({ static_cast<int>(k), x.id }))
                        x = changed(x, v);
        });
    };
    auto editor = std::make_unique<NoteEditor>(drum, static_cast<int>(chosen.size()), current, preview, apply);
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(editor), noteBounds(ref.track, i).toNearestInt() + getScreenPosition(), nullptr);
    box.setLookAndFeel(&getLookAndFeel());
}

void TrackerView::fineTuneSelection(int sixtyFourths)
{
    if (selection_.empty())
        return;
    const auto chosen = selection_;
    if (model_.edit("Fine tune", [chosen, sixtyFourths](Tune& tune) {
            for (size_t t = 0; t < tune.tracks.size(); ++t)
                for (TuneNote& n : tune.tracks[t].notes)
                    if (chosen.count({ static_cast<int>(t), n.id }))
                        n.fine = std::clamp(n.fine + sixtyFourths, -TuneNote::kMaxFine, TuneNote::kMaxFine);
        }))
        previewSelection();
}

void TrackerView::changeVelocity(int steps)
{
    if (selection_.empty())
        return;
    const auto chosen = selection_;
    if (model_.edit("Velocity", [chosen, steps](Tune& tune) {
            for (size_t t = 0; t < tune.tracks.size(); ++t)
                for (TuneNote& n : tune.tracks[t].notes)
                    if (chosen.count({ static_cast<int>(t), n.id }))
                        n.velocity = std::clamp(n.velocity + steps, TuneNote::kMinVelocity, 0);
        }))
        previewSelection();
}

void TrackerView::previewSelection()
{
    // The first FM note, else the first drum hit.
    const auto it = std::find_if(selection_.begin(), selection_.end(), [this](const NoteRef& ref) { return fmTrack(ref.track); });
    const NoteRef ref = it != selection_.end() ? *it : *selection_.begin();
    if (const int i = indexOf(ref); i >= 0)
        preview(ref, model_.tune().tracks[static_cast<size_t>(ref.track)].notes[static_cast<size_t>(i)]);
}

void TrackerView::preview(NoteRef ref, const TuneNote& note)
{
    if (fmTrack(ref.track))
        model_.previewNote(ref.track, note);
    else
        model_.previewHit(ref.track, note);
}

void TrackerView::addNoteAt(int track, int row, double time)
{
    if (row < 0)
        return; // a label strip
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track)];
    const auto& voices = layout().voices[static_cast<size_t>(track)];
    const int voice = static_cast<size_t>(row) < voices.size() ? voices[static_cast<size_t>(row)] : -1;
    auto nearest = [time](const TuneNote* best, const TuneNote& n) {
        return best == nullptr || std::abs(static_cast<double>(n.start) - time) < std::abs(static_cast<double>(best->start) - time) ? &n : best;
    };
    // A copy of the nearest note on the voice, the instrument's own sound there.
    const TuneNote *onVoice = nullptr, *anywhere = nullptr;
    for (const TuneNote& n : tr.notes)
    {
        anywhere = nearest(anywhere, n);
        if (voice < 0 || model_.tune().originalChannel(tr, n) == voice)
            onVoice = nearest(onVoice, n);
    }
    TuneNote note;
    if (onVoice != nullptr)
        note = *onVoice;
    else if (tr.kind == TuneTrack::Kind::Fm && voice >= 0)
    {
        // A voice with no notes yet gets a written one, a beat long or one
        // step of the snap, at the pitch of the track's nearest note, else C4.
        note.source = -1;
        note.voice = voice;
        note.transpose = anywhere != nullptr ? model_.tune().pitch(tr, *anywhere) - 60 : 0;
        note.length = static_cast<int64_t>(snap_ > 0 ? snap_ : model_.tune().beatLength());
    }
    else if (tr.kind == TuneTrack::Kind::Drum && model_.tune().sample(tr) != nullptr)
    {
        // A drum track with no hits gets a written one of its sample, on any
        // free channel: as long as the sample plays, or a beat for one that loops.
        note.source = -1;
        note.voice = -1;
        const int64_t natural = model_.tune().sampleLength(tr, 0);
        note.length = natural > 0 ? natural : static_cast<int64_t>(model_.tune().beatLength());
    }
    else
        return;
    note.start = static_cast<int64_t>(std::max(0.0, time));
    NoteRef added;
    const bool done = model_.edit("Add note", [track, note, &added](Tune& tune) mutable {
        note.id = tune.newNoteId();
        added = { track, note.id };
        auto& list = tune.tracks[static_cast<size_t>(track)].notes;
        list.push_back(note);
        std::stable_sort(list.begin(), list.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
    });
    if (done)
    {
        selection_ = { added };
        preview(added);
    }
    repaint();
}

int TrackerView::freeVoice(int track) const
{
    uint8_t used = 0, own = 0;
    for (size_t t = 0; t < model_.tune().tracks.size(); ++t)
        for (const int v : model_.tune().voices(model_.tune().tracks[t]))
        {
            used = static_cast<uint8_t>(used | 1 << v);
            if (static_cast<int>(t) == track)
                own = static_cast<uint8_t>(own | 1 << v);
        }
    for (const uint8_t taken : { used, own })
        for (int v = 0; v < 8; ++v)
            if (((taken >> v) & 1) == 0)
                return v;
    return -1;
}

void TrackerView::confirm(const juce::String& title, const juce::String& message, const juce::String& button, std::function<void()> then)
{
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle(title)
                                     .withMessage(message)
                                     .withButton(button)
                                     .withButton("Cancel")
                                     .withAssociatedComponent(this),
                                 [then](int result) {
                                     if (result == 1)
                                         then();
                                 });
}

bool TrackerView::refusedWhilePlaying()
{
    if (!model_.playing())
        return false;
    if (model_.onNotice)
        model_.onNotice("Stop playback to edit notes (Space)");
    return true;
}

namespace
{
juce::String notesOf(size_t count)
{
    return count == 0 ? juce::String("It has no notes.") : juce::String(count) + (count == 1 ? " note goes" : " notes go") + " with it.";
}
} // namespace

void TrackerView::addTrack()
{
    if (refusedWhilePlaying())
        return;
    // A song with no samples, as one written from empty, offers only FM.
    const auto& samples = model_.tune().samples();
    if (samples.empty())
        return addFmTrack();
    // Each sample with its number, its length and the hits that use it.
    std::vector<int> uses(samples.size(), 0);
    for (const VgmHit& h : model_.tune().imported().hits)
        if (h.sample >= 0 && static_cast<size_t>(h.sample) < uses.size())
            ++uses[static_cast<size_t>(h.sample)];
    juce::PopupMenu drums;
    for (size_t k = 0; k < samples.size(); ++k)
    {
        const size_t bytes = samples[k].data.size();
        const juce::String length = bytes >= 1024 ? juce::String(static_cast<double>(bytes) / 1024.0, 1) + " KB" : juce::String(bytes) + " bytes";
        drums.addItem(static_cast<int>(k) + 2, "PCM " + juce::String(k + 1) + theme::dot() + length + theme::dot() + juce::String(uses[k])
                                                    + (uses[k] == 1 ? " hit" : " hits"));
    }
    juce::PopupMenu menu;
    menu.addItem(1, "FM track");
    menu.addSubMenu("Drum track", drums);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(addTrackButton() + getScreenPosition()), [this](int chosen) {
        if (chosen == 1)
            addFmTrack();
        else if (chosen >= 2)
            addDrumTrack(chosen - 2);
    });
}

void TrackerView::addDrumTrack(int sample)
{
    if (refusedWhilePlaying() || sample < 0 || static_cast<size_t>(sample) >= model_.tune().samples().size())
        return;
    // Named Drum and the next number no track has.
    int number = 1;
    for (const TuneTrack& t : model_.tune().tracks)
        if (t.name.rfind("Drum ", 0) == 0)
            number = std::max(number, std::atoi(t.name.c_str() + 5) + 1);
    const std::string name = "Drum " + std::to_string(number);
    confirm("Add a track?", juce::String(name) + " plays PCM " + juce::String(sample + 1) + ". It has no hits: double-click its row to write one.", "Add",
            [this, sample, name] {
                model_.edit("Add track", [sample, name](Tune& tune) {
                    TuneTrack t;
                    t.kind = TuneTrack::Kind::Drum;
                    t.instrument = sample;
                    t.name = name;
                    tune.tracks.push_back(t);
                });
            });
}

void TrackerView::addFmTrack()
{
    if (refusedWhilePlaying())
        return;
    const int voice = std::max(0, freeVoice(-1));
    // Named FM and the next number no track has.
    int number = 1;
    for (const TuneTrack& t : model_.tune().tracks)
        if (t.name.rfind("FM ", 0) == 0)
            number = std::max(number, std::atoi(t.name.c_str() + 3) + 1);
    const std::string name = "FM " + std::to_string(number);
    confirm("Add a track?", juce::String(name) + " plays the default instrument on voice V" + juce::String(voice + 1) + ".", "Add", [this, voice, name] {
        model_.edit("Add track", [voice, name](Tune& tune) {
            TuneTrack t;
            t.kind = TuneTrack::Kind::Fm;
            t.instrument = -1;
            t.name = name;
            t.addedVoices = static_cast<uint8_t>(1 << voice);
            tune.tracks.push_back(t);
        });
    });
}

void TrackerView::removeTrack(int track)
{
    if (refusedWhilePlaying())
        return;
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track)];
    const std::string name = tr.name;
    confirm("Remove " + juce::String(name) + "?", notesOf(tr.notes.size()) + " Undo brings it back.", "Remove", [this, track, name] {
        // The track asked about, unless an edit since moved it.
        if (track >= static_cast<int>(model_.tune().tracks.size()) || model_.tune().tracks[static_cast<size_t>(track)].name != name)
            return;
        // Every later track moves up one place, so the selection and the row for a paste go.
        if (model_.edit("Remove track", [track](Tune& tune) { tune.tracks.erase(tune.tracks.begin() + track); }))
        {
            selection_.clear();
            hover_ = {};
            pasteTrack_ = pasteVoice_ = -1;
        }
    });
}

void TrackerView::addVoice(int track)
{
    const int voice = freeVoice(track);
    if (voice < 0)
        return;
    const std::string name = model_.tune().tracks[static_cast<size_t>(track)].name;
    confirm("Add a voice to " + juce::String(name) + "?", "Voice V" + juce::String(voice + 1) + " joins " + juce::String(name) + ".", "Add",
            [this, track, voice] {
                model_.edit("Add voice", [track, voice](Tune& tune) {
                    auto& t = tune.tracks[static_cast<size_t>(track)];
                    t.addedVoices = static_cast<uint8_t>(t.addedVoices | 1 << voice);
                });
            });
}

void TrackerView::removeVoice(int track, int row)
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track)];
    const int voice = layout().voices[static_cast<size_t>(track)][static_cast<size_t>(row)];
    const std::string name = tr.name;
    const auto count = static_cast<size_t>(std::count_if(tr.notes.begin(), tr.notes.end(),
                                                         [&](const TuneNote& n) { return model_.tune().originalChannel(tr, n) == voice; }));
    // A voice with no notes leaves the song to build as it is, so playback allows it.
    if (count > 0 && refusedWhilePlaying())
        return;
    confirm("Remove V" + juce::String(voice + 1) + " from " + juce::String(name) + "?", notesOf(count) + " Undo brings it back.", "Remove",
            [this, track, voice, name] {
                if (track >= static_cast<int>(model_.tune().tracks.size()) || model_.tune().tracks[static_cast<size_t>(track)].name != name)
                    return;
                const bool done = model_.edit("Remove voice", [track, voice](Tune& tune) {
                    auto& t = tune.tracks[static_cast<size_t>(track)];
                    std::erase_if(t.notes, [&](const TuneNote& n) { return tune.originalChannel(t, n) == voice; });
                    const auto bit = static_cast<uint16_t>(1 << voice);
                    t.addedVoices = static_cast<uint8_t>(t.addedVoices & ~bit);
                    t.mutedVoices = static_cast<uint16_t>(t.mutedVoices & ~bit);
                    t.soloedVoices = static_cast<uint16_t>(t.soloedVoices & ~bit);
                    t.voiceVolume[static_cast<size_t>(voice)] = 0.0f;
                    t.voiceSides[static_cast<size_t>(voice)] = 0;
                });
                if (done)
                {
                    selection_.clear();
                    hover_ = {};
                }
            });
}

} // namespace opm86
