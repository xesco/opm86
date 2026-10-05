#include "MainComponent.h"

#include "Project.h"

#include "Theme.h"

#include <cmath>
#include <numeric>

namespace opm86
{

namespace
{

constexpr int kTopHeight = 64;
constexpr int kToolHeight = 44;
constexpr int kOverviewHeight = 54;
constexpr int kPianoHeight = 66;
constexpr int kStatusHeight = 30;

// Flat, rounded controls in the theme's colours and fonts.
class TrackerLook : public juce::LookAndFeel_V4
{
public:
    TrackerLook()
    {
        setColour(juce::ComboBox::backgroundColourId, theme::background);
        setColour(juce::ComboBox::outlineColourId, theme::gridBar);
        setColour(juce::ComboBox::textColourId, theme::text);
        setColour(juce::ComboBox::arrowColourId, theme::dim);
        setColour(juce::PopupMenu::backgroundColourId, theme::panel);
        setColour(juce::PopupMenu::textColourId, theme::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::magenta);
        setColour(juce::PopupMenu::highlightedTextColourId, theme::background);
        setColour(juce::TooltipWindow::backgroundColourId, theme::panelLight);
        setColour(juce::TooltipWindow::textColourId, theme::text);
        setColour(juce::TooltipWindow::outlineColourId, theme::gridBar);
        setColour(juce::ResizableWindow::backgroundColourId, theme::panel);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        const juce::Colour accent = b.findColour(juce::TextButton::buttonOnColourId);
        const bool on = b.getToggleState();
        g.setColour(on ? accent : (down ? theme::panelLight.brighter(0.1f) : over ? theme::panelLight : theme::background));
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(on || over ? accent : theme::gridBar);
        g.drawRoundedRectangle(r, 5.0f, 1.0f);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int) override { return theme::pixel(8.0f); }

    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool over, bool) override
    {
        g.setFont(getTextButtonFont(b, b.getHeight()));
        // A button's own text colour when it sets one, as an edited value does.
        const juce::Colour own = b.findColour(juce::TextButton::textColourOffId);
        g.setColour(b.getToggleState() ? theme::background
                    : b.isColourSpecified(juce::TextButton::textColourOffId) ? (over ? own.brighter(0.2f) : own)
                    : over ? theme::text : theme::dim);
        g.drawText(b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }

    // A groove filled cyan up to a small console cap.
    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle,
                          juce::Slider& slider) override
    {
        const auto box = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
        const auto groove = box.withSizeKeepingCentre(box.getWidth(), 6.0f);
        g.setColour(juce::Colour(0xff2a2c34));
        g.fillRoundedRectangle(groove, 3.0f);
        g.setColour(theme::cyan.withAlpha(slider.isMouseOverOrDragging() ? 0.9f : 0.6f));
        g.fillRoundedRectangle(groove.withRight(pos), 3.0f);
        g.setColour(theme::gridBar);
        g.drawRoundedRectangle(groove, 3.0f, 1.0f);
        // The home mark: a tick across the groove.
        if (const juce::var& home = slider.getProperties()["home"]; !home.isVoid())
        {
            const auto hx = static_cast<float>(slider.getPositionOfValue(home));
            g.setColour(theme::text.withAlpha(0.75f));
            g.fillRoundedRectangle(hx - 1.0f, box.getCentreY() - 8.0f, 2.0f, 16.0f, 1.0f);
        }
        const auto cap = juce::Rectangle<float>(10.0f, std::min(20.0f, box.getHeight())).withCentre({ pos, box.getCentreY() });
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillRoundedRectangle(cap.translated(0.0f, 1.0f), 2.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff2f0ff), cap.getX(), cap.getY(), juce::Colour(0xff8f93bd), cap.getX(),
                                               cap.getBottom(), false));
        g.fillRoundedRectangle(cap, 2.0f);
        g.setColour(theme::background);
        g.fillRect(pos - 0.75f, cap.getY() + 2.0f, 1.5f, cap.getHeight() - 4.0f);
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return theme::mono(13.0f, true); }
    juce::Font getPopupMenuFont() override { return theme::mono(14.0f); }

    void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&) override
    {
        const auto r = juce::Rectangle<float>(0, 0, static_cast<float>(w), static_cast<float>(h)).reduced(0.5f);
        g.setColour(theme::background);
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(theme::gridBar);
        g.drawRoundedRectangle(r, 5.0f, 1.0f);
        juce::Path arrow;
        const float x = static_cast<float>(w) - 14.0f, y = static_cast<float>(h) * 0.5f;
        arrow.addTriangle(x - 4, y - 2, x + 4, y - 2, x, y + 3);
        g.setColour(theme::dim);
        g.fillPath(arrow);
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(4, 0, box.getWidth() - 22, box.getHeight());
        label.setFont(getComboBoxFont(box));
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        g.fillAll(theme::panelLight);
        g.setColour(theme::cyan);
        g.drawRect(0, 0, w, h);
        g.setColour(theme::text);
        g.setFont(theme::mono(13.0f, true));
        g.drawText(text, 0, 0, w, h, juce::Justification::centred);
    }

    juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> at, juce::Rectangle<int> parent) override
    {
        const int w = juce::GlyphArrangement::getStringWidthInt(theme::mono(13.0f, true), text) + 20;
        return juce::Rectangle<int>(at.x + 12, at.y + 18, w, 24).constrainedWithin(parent);
    }

    // A callout: the panel's colour, edged in cyan.
    void drawCallOutBoxBackground(juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&) override
    {
        g.setColour(theme::panel);
        g.fillPath(path);
        g.setColour(theme::cyan);
        g.strokePath(path, juce::PathStrokeType(1.5f));
    }

    // A command's key with nothing to act on, as Delete with no note
    // selected, does nothing quietly.
    void playAlertSound() override {}
};

// The song's tempo and meter, in a callout from the BPM counter: the tempo
// dragged or typed, the beats in a bar, and bar 1 moved to the playhead.
// Each follows the model, as an undo changes it.
class TempoEditor : public juce::Component, private juce::ChangeListener
{
public:
    explicit TempoEditor(AppModel& model) : model_(model)
    {
        for (auto [caption, text] : { std::pair { &tempoCaption_, "TEMPO (BPM)" }, std::pair { &meterCaption_, "BEATS PER BAR" } })
        {
            caption->setText(text, juce::dontSendNotification);
            caption->setFont(theme::pixel(8.0f));
            caption->setColour(juce::Label::textColourId, theme::orange);
            addAndMakeVisible(caption);
        }
        tempo_.setRange(SongGrid::kMinTempo, SongGrid::kMaxTempo, 0.1);
        tempo_.setSkewFactorFromMidPoint(120.0);
        meter_.setRange(1.0, SongGrid::kMaxBeatsPerBar, 1.0);
        for (auto [slider, value] : { std::pair { &tempo_, &tempoValue_ }, std::pair { &meter_, &meterValue_ } })
        {
            slider->setSliderStyle(juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            slider->setWantsKeyboardFocus(false);
            // A drag is one step of undo. On release the slider shows the
            // song's value again.
            slider->onDragStart = [this] { dragging_ = true, stepTaken_ = false; };
            slider->onDragEnd = [this] {
                dragging_ = false;
                sync();
            };
            addAndMakeVisible(slider);
            // The value, typed after a click.
            value->setEditable(true, true, false);
            value->setFont(theme::mono(14.0f, true));
            value->setJustificationType(juce::Justification::centred);
            value->setColour(juce::Label::textColourId, theme::text);
            value->setColour(juce::Label::backgroundColourId, theme::background);
            value->setColour(juce::Label::outlineColourId, theme::gridBar);
            value->setColour(juce::Label::textWhenEditingColourId, theme::text);
            value->setColour(juce::Label::backgroundWhenEditingColourId, theme::background);
            value->setColour(juce::Label::outlineWhenEditingColourId, theme::cyan);
            value->setColour(juce::TextEditor::highlightColourId, theme::magenta.withAlpha(0.5f));
            addAndMakeVisible(value);
        }
        tempo_.onValueChange = [this] {
            if (!syncing_ && !model_.setTempo(tempo_.getValue(), newStep()))
                sync();
        };
        meter_.onValueChange = [this] {
            if (!syncing_ && !model_.setBeatsPerBar(static_cast<int>(meter_.getValue()), newStep()))
                sync();
        };
        // A typed value out of range goes to the nearest end; text that is no number changes nothing.
        tempoValue_.onTextChange = [this] {
            const juce::String t = tempoValue_.getText().trim();
            if (t.containsOnly("0123456789.") && t.containsAnyOf("0123456789"))
                tempo_.setValue(std::round(juce::jlimit(SongGrid::kMinTempo, SongGrid::kMaxTempo, t.getDoubleValue()) * 10.0) / 10.0);
            sync();
        };
        meterValue_.onTextChange = [this] {
            const juce::String t = meterValue_.getText().trim();
            if (t.containsOnly("0123456789") && t.isNotEmpty())
                meter_.setValue(juce::jlimit(1, SongGrid::kMaxBeatsPerBar, t.getIntValue()));
            sync();
        };
        here_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
        here_.setTooltip("Bar 1 starts at the playhead, which is then on a downbeat");
        here_.setWantsKeyboardFocus(false);
        here_.onClick = [this] { model_.barOneHere(); };
        addAndMakeVisible(here_);
        model_.addChangeListener(this);
        sync();
        setSize(300, 150);
    }

    ~TempoEditor() override { model_.removeChangeListener(this); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10, 8);
        for (auto [caption, slider, value] : { std::tuple { &tempoCaption_, &tempo_, &tempoValue_ }, std::tuple { &meterCaption_, &meter_, &meterValue_ } })
        {
            caption->setBounds(area.removeFromTop(16));
            auto row = area.removeFromTop(28);
            value->setBounds(row.removeFromRight(64).reduced(0, 2));
            row.removeFromRight(10);
            slider->setBounds(row);
            area.removeFromTop(6);
        }
        area.removeFromTop(4);
        here_.setBounds(area.removeFromTop(28));
    }

private:
    // The first change of a drag, or any change not dragged, begins a step.
    bool newStep()
    {
        const bool first = !dragging_ || !stepTaken_;
        stepTaken_ = true;
        return first;
    }

    void sync()
    {
        if (!model_.hasTune())
            return;
        syncing_ = true;
        const Tune& tune = model_.tune();
        if (!tempo_.isMouseButtonDown())
            tempo_.setValue(tune.tempo(), juce::dontSendNotification);
        if (!meter_.isMouseButtonDown())
            meter_.setValue(tune.beatsPerBar(), juce::dontSendNotification);
        if (!tempoValue_.isBeingEdited())
            tempoValue_.setText(juce::String(tune.tempo(), 1), juce::dontSendNotification);
        if (!meterValue_.isBeingEdited())
            meterValue_.setText(juce::String(tune.beatsPerBar()), juce::dontSendNotification);
        syncing_ = false;
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { sync(); }

    AppModel& model_;
    juce::Label tempoCaption_, meterCaption_, tempoValue_, meterValue_;
    juce::Slider tempo_, meter_;
    juce::TextButton here_ { "BAR 1 STARTS AT THE PLAYHEAD" };
    bool dragging_ = false, stepTaken_ = false, syncing_ = false;
};

// Song time as an LED readout: bar.beat.sixteenth on the song's grid, and m:ss.cc.
juce::String barBeat(const Tune& tune, double t)
{
    const long perBar = 4L * tune.beatsPerBar(); // sixteenths
    const double beats = (t - tune.firstBeat()) / tune.beatLength();
    const auto sixteenths = static_cast<long>(std::floor(beats * 4));
    const long bar = (sixteenths >= 0 ? sixteenths / perBar : (sixteenths - perBar + 1) / perBar) + 1;
    const long inBar = ((sixteenths % perBar) + perBar) % perBar;
    // A bar before bar 1 has its sign in front of the digits.
    const juce::String number = juce::String(std::abs(bar)).paddedLeft('0', bar < 0 ? 2 : 3);
    return (bar < 0 ? "-" : "") + number + "." + juce::String(inBar / 4 + 1) + "." + juce::String(inBar % 4 + 1);
}

juce::String clock(double t)
{
    const double s = std::max(0.0, t / kVgmRate);
    const int minutes = static_cast<int>(s / 60);
    const double rest = s - minutes * 60;
    return juce::String(minutes) + ":" + juce::String(static_cast<int>(rest)).paddedLeft('0', 2) + "."
           + juce::String(static_cast<int>((rest - std::floor(rest)) * 100)).paddedLeft('0', 2);
}

void drawLed(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, juce::Colour colour)
{
    g.setColour(theme::background);
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    g.setColour(theme::grid);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.0f, 1.0f);
    g.setFont(theme::mono(11.0f, true));
    g.setColour(theme::dim);
    g.drawText(caption, r.reduced(8, 3), juce::Justification::topLeft);
    // Unlit segments behind the value, like a real display.
    g.setFont(theme::pixel(12.0f));
    const auto valueArea = r.reduced(8, 3).withTrimmedTop(13);
    g.setColour(colour.withAlpha(0.10f));
    g.drawText(juce::String::repeatedString("8", value.length()).replaceCharacters(".:", ".:"), valueArea,
               juce::Justification::centredLeft);
    g.setColour(colour.withAlpha(0.35f));
    g.drawText(value, valueArea.translated(0, 1), juce::Justification::centredLeft);
    g.setColour(colour);
    g.drawText(value, valueArea, juce::Justification::centredLeft);
}

// The keys a keyboard has in black: C#, D#, F#, G# and A#.
bool isBlackKey(int key)
{
    const int pc = key % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

} // namespace

// IconButton ------------------------------------------------------------------

void IconButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    getLookAndFeel().drawButtonBackground(g, *this, {}, over, down);
    const auto box = getLocalBounds().toFloat().withSizeKeepingCentre(14.0f, 14.0f);
    juce::Path p;
    switch (icon_)
    {
        case Icon::Rewind: // a bar and a triangle pointing left
            p.addRectangle(box.getX(), box.getY(), 3.0f, box.getHeight());
            p.addTriangle(box.getRight(), box.getY(), box.getRight(), box.getBottom(), box.getX() + 3.0f, box.getCentreY());
            break;
        case Icon::Play:
            p.addTriangle(box.getX() + 1.0f, box.getY(), box.getX() + 1.0f, box.getBottom(), box.getRight(), box.getCentreY());
            break;
        case Icon::Stop:
            p.addRectangle(box.reduced(1.0f));
            break;
        case Icon::Playhead: // the playhead: a line under a small triangle
            p.addTriangle(box.getCentreX() - 5.0f, box.getY(), box.getCentreX() + 5.0f, box.getY(), box.getCentreX(), box.getY() + 6.0f);
            p.addRectangle(box.getCentreX() - 1.0f, box.getY(), 2.0f, box.getHeight());
            break;
        case Icon::Reset: // an arrow going back round a circle
        {
            const auto c = box.getCentre();
            juce::Path arc;
            arc.addCentredArc(c.x, c.y, 5.5f, 5.5f, 0.0f, 0.6f, 5.6f, true);
            juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath(p, arc);
            const auto tip = c.getPointOnCircumference(5.5f, 0.6f);
            p.addTriangle(tip.x - 3.5f, tip.y - 3.0f, tip.x + 3.0f, tip.y - 3.5f, tip.x + 0.5f, tip.y + 3.0f);
            break;
        }
        case Icon::Instruments: // a quaver
        {
            const auto c = box.getCentre();
            p.addEllipse(c.x - 5.5f, c.y + 1.5f, 6.0f, 5.0f);
            p.addRectangle(c.x - 0.2f, c.y - 6.5f, 1.5f, 10.0f);
            p.addTriangle(c.x + 1.3f, c.y - 6.5f, c.x + 5.5f, c.y - 3.0f, c.x + 1.3f, c.y - 2.0f);
            break;
        }
        case Icon::Drums: // a drum, its skin on top
        {
            const auto c = box.getCentre();
            juce::Path outline;
            outline.addEllipse(c.x - 6.5f, c.y - 5.5f, 13.0f, 5.0f);
            outline.startNewSubPath(c.x - 6.5f, c.y - 3.0f);
            outline.lineTo(c.x - 6.5f, c.y + 3.5f);
            outline.addCentredArc(c.x, c.y + 3.5f, 6.5f, 2.5f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, false);
            outline.lineTo(c.x + 6.5f, c.y - 3.0f);
            juce::PathStrokeType(1.5f).createStrokedPath(p, outline);
            break;
        }
        case Icon::Chip: // a chip: its body, a notch at the top, three pins each side
        {
            const auto c = box.getCentre();
            p.addRoundedRectangle(c.x - 4.5f, c.y - 6.0f, 9.0f, 12.0f, 1.0f);
            for (int k = 0; k < 3; ++k)
            {
                const float y = c.y - 4.0f + static_cast<float>(k) * 3.25f;
                p.addRectangle(c.x - 7.5f, y, 2.5f, 1.5f);
                p.addRectangle(c.x + 5.0f, y, 2.5f, 1.5f);
            }
            p.setUsingNonZeroWinding(false);
            p.addEllipse(c.x - 1.5f, c.y - 7.5f, 3.0f, 3.0f); // the notch, cut out
            break;
        }
        case Icon::Loop: // a loop: a rounded band, an arrow head on each run
        {
            const auto c = box.getCentre();
            juce::Path band;
            band.addRoundedRectangle(c.x - 6.5f, c.y - 4.5f, 13.0f, 9.0f, 4.0f);
            juce::PathStrokeType(1.5f).createStrokedPath(p, band);
            p.addTriangle(c.x - 0.5f, c.y - 8.0f, c.x - 0.5f, c.y - 1.0f, c.x + 3.5f, c.y - 4.5f);
            p.addTriangle(c.x + 0.5f, c.y + 1.0f, c.x + 0.5f, c.y + 8.0f, c.x - 3.5f, c.y + 4.5f);
            break;
        }
        case Icon::Home: // a house
        {
            const auto c = box.getCentre();
            p.addTriangle(c.x - 7.0f, c.y - 0.5f, c.x + 7.0f, c.y - 0.5f, c.x, c.y - 7.0f);
            p.addRectangle(c.x - 5.0f, c.y - 0.5f, 10.0f, 7.0f);
            p.setUsingNonZeroWinding(false);
            p.addRectangle(c.x - 1.5f, c.y + 2.5f, 3.0f, 4.0f); // the door, cut out
            break;
        }
        case Icon::FitTracks: // arrows to the top and bottom edges
        case Icon::FitTime:   // arrows to the left and right edges
        case Icon::FitBoth:   // arrows to all four corners
        {
            const auto c = box.getCentre();
            auto arrow = [&](float angle) {
                // A line from near the centre with a head at the edge.
                const auto tip = c.getPointOnCircumference(7.0f, angle);
                const auto from = c.getPointOnCircumference(1.5f, angle);
                juce::Path line;
                line.startNewSubPath(from);
                line.lineTo(tip);
                juce::Path stroked;
                juce::PathStrokeType(1.5f).createStrokedPath(stroked, line);
                p.addPath(stroked);
                const auto l = tip.getPointOnCircumference(4.0f, angle + juce::MathConstants<float>::pi * 0.8f);
                const auto rr = tip.getPointOnCircumference(4.0f, angle - juce::MathConstants<float>::pi * 0.8f);
                p.addTriangle(tip, l, rr);
            };
            const float pi = juce::MathConstants<float>::pi;
            if (icon_ == Icon::FitTracks)
                arrow(0.0f), arrow(pi);
            else if (icon_ == Icon::FitTime)
                arrow(pi * 0.5f), arrow(pi * 1.5f);
            else
                for (int k = 0; k < 4; ++k)
                    arrow(pi * 0.25f + static_cast<float>(k) * pi * 0.5f);
            // The edges the arrows reach.
            if (icon_ == Icon::FitTracks)
                p.addRectangle(box.getX(), box.getY() - 1.0f, box.getWidth(), 1.5f), p.addRectangle(box.getX(), box.getBottom() - 0.5f, box.getWidth(), 1.5f);
            else if (icon_ == Icon::FitTime)
                p.addRectangle(box.getX() - 1.0f, box.getY(), 1.5f, box.getHeight()), p.addRectangle(box.getRight() - 0.5f, box.getY(), 1.5f, box.getHeight());
            break;
        }
    }
    g.setColour(getToggleState() ? theme::background : over ? theme::text : theme::dim);
    g.fillPath(p);
}

// SnapSlider ------------------------------------------------------------------

void SnapSlider::setHome(double home)
{
    getProperties().set("home", home); // the look draws the mark from it
    setDoubleClickReturnValue(true, home);
    repaint();
}

double SnapSlider::snapValue(double attempted, DragMode)
{
    // Within a twenty-fifth of the range of home, the value is home.
    const juce::var& home = getProperties()["home"];
    if (!home.isVoid() && std::abs(attempted - static_cast<double>(home)) < (getMaximum() - getMinimum()) / 25.0)
        return home;
    return attempted;
}

// TopBar ----------------------------------------------------------------------

TopBar::TopBar(AppModel& model, TrackerView& view) : model_(model), view_(view)
{
    rewind_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    play_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    stop_.setColour(juce::TextButton::buttonOnColourId, theme::orange);
    open_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    rewind_.setTooltip("Rewind to the start (Return)");
    play_.setTooltip("Play (Space)");
    stop_.setTooltip("Stop (Space); again to return to the start");
    open_.setTooltip("Open (Cmd+O)");
    rewind_.onClick = [this] { rewind(); };
    play_.onClick = [this] { model_.play(); };
    stop_.onClick = [this] {
        // A second stop returns to the start.
        if (!model_.playing())
            model_.setPosition(0);
        model_.stop();
    };
    open_.onClick = [this] {
        if (onOpen)
            onOpen();
    };
    json_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    json_.setTooltip("Export or import the project as JSON");
    json_.onClick = [this] {
        if (onJson)
            onJson();
    };
    new_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    new_.setTooltip("New song (Cmd+N)");
    new_.onClick = [this] {
        if (onNew)
            onNew();
    };
    save_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    save_.setTooltip("Save the project (Cmd+S)");
    save_.onClick = [this] {
        if (onSave)
            onSave();
    };
    for (juce::Button* b : std::initializer_list<juce::Button*> { &rewind_, &play_, &stop_, &new_, &open_, &save_, &json_ })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
}

TopBar::~TopBar()
{
    // The tempo callout follows the model, so it goes first.
    delete tempo_.getComponent();
}

juce::String TopBar::getTooltip()
{
    if (model_.hasTune() && bpmArea_.contains(getMouseXYRelative()))
        return "Tempo and beats per bar";
    return model_.hasTune() && fileArea_.contains(getMouseXYRelative()) && model_.file() != juce::File() ? model_.file().getFullPathName() : juce::String();
}

void TopBar::showTempo()
{
    if (!model_.hasTune() || tempo_ != nullptr)
        return;
    auto& box = juce::CallOutBox::launchAsynchronously(std::make_unique<TempoEditor>(model_), bpmArea_ + getScreenPosition(), nullptr);
    box.setLookAndFeel(&getLookAndFeel());
    tempo_ = &box;
}

void TopBar::mouseDown(const juce::MouseEvent& e)
{
    if (bpmArea_.contains(e.getPosition()))
        showTempo();
}

void TopBar::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(model_.hasTune() && bpmArea_.contains(e.getPosition()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void TopBar::rewind()
{
    model_.setPosition(0);
    if (model_.hasTune())
        view_.setViewStart(model_.tune().firstBeat() - model_.tune().beatLength());
}

void TopBar::resized()
{
    auto r = getLocalBounds().reduced(14, 13);
    r.removeFromLeft(196); // title
    rewind_.setBounds(r.removeFromLeft(44));
    r.removeFromLeft(6);
    play_.setBounds(r.removeFromLeft(44));
    r.removeFromLeft(6);
    stop_.setBounds(r.removeFromLeft(44));
    r.removeFromLeft(14);
    json_.setBounds(r.removeFromRight(70));
    r.removeFromRight(8);
    open_.setBounds(r.removeFromRight(70));
    r.removeFromRight(8);
    save_.setBounds(r.removeFromRight(70));
    r.removeFromRight(8);
    new_.setBounds(r.removeFromRight(70));
    r.removeFromRight(14);
    counters_ = r;
}

void TopBar::paint(juce::Graphics& g)
{
    g.setGradientFill(juce::ColourGradient(theme::panelLight, 0, 0, theme::panel, 0, static_cast<float>(getHeight()), false));
    g.fillAll();

    // Title.
    auto title = juce::Rectangle<int>(16, 12, 190, getHeight() - 24);
    g.setFont(theme::pixel(15.0f));
    g.setColour(theme::magenta.withAlpha(0.5f));
    g.drawText("OPM86", title.removeFromTop(20).translated(2, 2), juce::Justification::topLeft);
    g.setColour(theme::orange);
    g.drawText("OPM86", juce::Rectangle<int>(16, 12, 190, 20), juce::Justification::topLeft);
    g.setFont(theme::mono(12.0f, true));
    g.setColour(theme::dim);
    g.drawText("TRACKER  YM2151+SEGAPCM", juce::Rectangle<int>(16, 38, 200, 12), juce::Justification::topLeft);

    play_.setToggleState(model_.playing(), juce::dontSendNotification);

    // Counters.
    auto c = counters_;
    if (model_.hasTune())
    {
        const Tune& tune = model_.tune();
        const double p = model_.displayPosition();
        drawLed(g, c.removeFromLeft(130), "BAR.BEAT", barBeat(tune, p), theme::orange);
        c.removeFromLeft(6);
        drawLed(g, c.removeFromLeft(120), "TIME", clock(p), theme::cyan);
        c.removeFromLeft(6);
        // The tempo heard, and the meter; a click sets them.
        bpmArea_ = c.removeFromLeft(80);
        drawLed(g, bpmArea_, "BPM " + juce::String(tune.beatsPerBar()) + "/4", juce::String(tune.tempo() * model_.speed(), 1), theme::magenta);
        c.removeFromLeft(10);
        g.setFont(theme::mono(13.0f, true));
        g.setColour(theme::text);
        fileArea_ = c;
        g.drawText(model_.file() != juce::File() ? model_.file().getFileName() : model_.name(), c.withTrimmedBottom(c.getHeight() / 2), juce::Justification::bottomLeft, true);
        g.setFont(theme::mono(12.0f));
        g.setColour(theme::dim);
        g.drawText("length " + clock(static_cast<double>(model_.tune().length())), c.withTrimmedTop(c.getHeight() / 2 + 2), juce::Justification::topLeft, true);
    }
    else
    {
        drawLed(g, c.removeFromLeft(130), "BAR.BEAT", "---.-.-", theme::faint);
        bpmArea_ = {};
    }
}

// JogWheel --------------------------------------------------------------------

float JogWheel::angleAt(juce::Point<float> p) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    return std::atan2(p.y - c.y, p.x - c.x);
}

void JogWheel::turn(double turns, bool fine)
{
    turns *= fine ? 0.25 : 1.0;
    angle_ += static_cast<float>(turns * juce::MathConstants<double>::twoPi);
    repaint();
    if (onTurn)
        onTurn(turns);
}

void JogWheel::mouseDown(const juce::MouseEvent& e)
{
    lastAngle_ = angleAt(e.position);
}

void JogWheel::mouseDrag(const juce::MouseEvent& e)
{
    // Round the centre, by the change in the pointer's angle.
    const float a = angleAt(e.position);
    float d = a - lastAngle_;
    if (d > juce::MathConstants<float>::pi)
        d -= juce::MathConstants<float>::twoPi;
    else if (d < -juce::MathConstants<float>::pi)
        d += juce::MathConstants<float>::twoPi;
    lastAngle_ = a;
    turn(d / juce::MathConstants<float>::twoPi, e.mods.isShiftDown());
}

void JogWheel::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Scrolling down or right goes forward, a quarter turn for a full flick.
    const float delta = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : -w.deltaY;
    turn(delta * 0.25, e.mods.isShiftDown());
}

void JogWheel::paint(juce::Graphics& g)
{
    // A round, lit-from-above wheel with finger dimples and a cyan mark.
    const auto box = getLocalBounds().toFloat().reduced(1.0f);
    const float size = std::min(box.getWidth(), box.getHeight());
    const auto face = juce::Rectangle<float>(size, size).withCentre(box.getCentre());
    const auto c = face.getCentre();
    const bool over = isMouseOverOrDragging();
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(face.translated(0.0f, 1.5f));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4f5378), face.getX(), face.getY(), juce::Colour(0xff23263a), face.getX(),
                                           face.getBottom(), false));
    g.fillEllipse(face);
    g.setColour(over ? theme::cyan : theme::gridBar.brighter(0.3f));
    g.drawEllipse(face.reduced(0.5f), 1.0f);
    for (int k = 0; k < 8; ++k)
    {
        const auto dimple = c.getPointOnCircumference(size * 0.32f, angle_ + static_cast<float>(k) * juce::MathConstants<float>::pi / 4.0f);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillEllipse(juce::Rectangle<float>(3.0f, 3.0f).withCentre(dimple));
    }
    g.setColour(theme::cyan);
    g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f).withCentre(c.getPointOnCircumference(size * 0.32f, angle_)));
}

// GridPicker ------------------------------------------------------------------

juce::Rectangle<float> GridPicker::segment(int index) const
{
    const int rows = (labels_.size() + columns_ - 1) / columns_;
    const float w = static_cast<float>(getWidth()) / static_cast<float>(columns_);
    const float h = static_cast<float>(getHeight()) / static_cast<float>(rows);
    return { w * static_cast<float>(index % columns_), h * static_cast<float>(index / columns_), w, h };
}

void GridPicker::paint(juce::Graphics& g)
{
    // One rounded groove split into cells; the chosen one lit cyan.
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(theme::background);
    g.fillRoundedRectangle(r, 4.0f);
    const auto mouse = getMouseXYRelative().toFloat();
    g.setFont(theme::mono(11.0f, true));
    for (int k = 0; k < labels_.size(); ++k)
    {
        const auto s = segment(k).reduced(1.5f);
        const bool on = k == selected_, over = isMouseOver() && segment(k).contains(mouse);
        if (on || over)
        {
            g.setColour(on ? theme::cyan : theme::panelLight);
            g.fillRoundedRectangle(s, 3.0f);
        }
        g.setColour(on ? theme::background : over ? theme::text : theme::dim);
        g.drawText(labels_[k], s, juce::Justification::centred);
        // A faint line before each cell in a row, but not beside the lit one.
        if (k % columns_ > 0 && !on && k - 1 != selected_)
        {
            const auto cell = segment(k);
            g.setColour(theme::grid);
            g.fillRect(cell.getX(), cell.getY() + 3.0f, 1.0f, cell.getHeight() - 6.0f);
        }
    }
    g.setColour(theme::gridBar);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
}

void GridPicker::mouseDown(const juce::MouseEvent& e)
{
    for (int k = 0; k < labels_.size(); ++k)
        if (segment(k).contains(e.position) && k != selected_)
        {
            setSelected(k);
            if (onChange)
                onChange();
        }
}

// ToolBar ---------------------------------------------------------------------

ToolBar::ToolBar(AppModel& model, TrackerView& view) : model_(model), view_(view)
{
    snap_.setSelected(5); // 1/16
    snap_.setTooltip("Snap grid: a bar, or a note value");
    snap_.onChange = [this] {
        // A bar, or a note: a whole one is four beats, as the beat is a quarter note.
        static constexpr int kDivision[] = { 0, 1, 2, 4, 8, 16 }; // 0 is off
        const int division = kDivision[snap_.selected()];
        if (!model_.hasTune() || division == 0)
            view_.setSnap(0);
        else
            view_.setSnap(division == 1 ? model_.tune().barLength() : model_.tune().beatLength() * 4 / division);
    };
    addAndMakeVisible(snap_);

    keepPlayhead_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    keepPlayhead_.setClickingTogglesState(true);
    keepPlayhead_.setTooltip("Keep playhead");
    keepPlayhead_.onClick = [this] { setKeepPlayhead(keepPlayhead_.getToggleState()); };
    loop_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    loop_.setTooltip("Loop (Cmd+Shift+L); Shift-drag on the ruler to set it");
    loop_.onClick = [this] {
        if (onLoop)
            onLoop();
    };
    reset_.setTooltip("Reset mix");
    reset_.onClick = [this] {
        if (model_.hasTune())
            model_.edit("Reset mix", [](Tune& tune) {
                // Every track as the game played it: heard, at 0 dB.
                for (auto& tr : tune.tracks)
                {
                    tr.mute = tr.solo = false, tr.mutedVoices = tr.soloedVoices = 0, tr.volume = 0.0f;
                    tr.voiceVolume.fill(0.0f);
                }
            });
    };
    instruments_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    instruments_.setTooltip("Only instruments");
    instruments_.onClick = [this] { soloOnly(Only::Instruments); };
    drums_.setColour(juce::TextButton::buttonOnColourId, theme::orange);
    drums_.setTooltip("Only drums");
    drums_.onClick = [this] { soloOnly(Only::Drums); };
    chip_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    chip_.setTooltip("Chip: the LFO and noise every voice shares");
    chip_.onClick = [this] {
        if (onChip)
            onChip();
    };
    fitTracks_.setTooltip("Fit tracks");
    fitTracks_.onClick = [this] { view_.fitTracks(); };
    fitTime_.setTooltip("Fit time");
    fitTime_.onClick = [this] { view_.fitTime(); };
    fitBoth_.setTooltip("Fit both");
    fitBoth_.onClick = [this] { view_.fitTracks(), view_.fitTime(); };
    zoomHome_.setTooltip("Default zoom");
    zoomHome_.onClick = [this] { defaultZoom(); };
    for (juce::Button* b : std::initializer_list<juce::Button*> { &keepPlayhead_, &loop_, &reset_, &instruments_, &drums_, &chip_, &fitTracks_, &fitTime_, &fitBoth_, &zoomHome_ })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }

    // Speed on a log scale, 25% to 200%, home at 100%.
    for (auto* slider : { &speed_, &heightZoom_, &timeZoom_ })
    {
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider->setWantsKeyboardFocus(false);
        addAndMakeVisible(slider);
    }
    speed_.setRange(-2.0, 1.0);
    speed_.setValue(0.0, juce::dontSendNotification);
    speed_.setHome(0.0);
    speed_.setTooltip("Speed");
    speed_.onValueChange = [this] {
        model_.setSpeed(std::exp2(speed_.getValue()));
        repaint();
    };
    jog_.setTooltip("Jog");
    jog_.onTurn = [this](double turns) {
        // A turn is a bar. The wheel takes over from playback, which waits
        // for Play, and lets each place it reaches sound; the view follows.
        if (!model_.hasTune())
            return;
        double from = model_.position();
        if (model_.playing())
        {
            from = model_.displayPosition();
            model_.stop();
        }
        model_.scrub(std::max(0.0, from + turns * model_.tune().barLength()));
        view_.reveal(model_.position());
    };
    addAndMakeVisible(jog_);

    // Zoom. Time goes on a log scale, right zooming in.
    timeZoom_.setRange(-std::log2(TrackerView::kMaxSamplesPerPixel), -std::log2(TrackerView::kMinSamplesPerPixel));
    timeZoom_.setTooltip("Time zoom");
    timeZoom_.onValueChange = [this] {
        view_.zoomAround(view_.samplesPerPixel() / std::exp2(-timeZoom_.getValue()), view_.middle());
    };
    heightZoom_.setRange(TrackerView::kMinLaneHeight, TrackerView::kMaxLaneHeight, 1.0);
    heightZoom_.setHome(view_.laneHeight()); // the lanes' first height
    heightZoom_.setTooltip("Track height");
    heightZoom_.onValueChange = [this] {
        const int height = view_.inPianoRoll() ? view_.rowHeight() : view_.laneHeight();
        view_.zoomLanes(heightZoom_.getValue() / height, TrackerView::kRulerHeight);
    };
}

void ToolBar::songOpened()
{
    snap_.onChange();
    timeZoom_.setHome(-std::log2(view_.samplesPerPixel()));
    syncZoom();
    refresh();
}

void ToolBar::defaultZoom()
{
    for (auto* slider : { &heightZoom_, &timeZoom_ })
        if (const juce::var& home = slider->getProperties()["home"]; !home.isVoid())
            slider->setValue(home, juce::sendNotificationSync);
}

void ToolBar::setKeepPlayhead(bool keep)
{
    keepPlayhead_.setToggleState(keep, juce::dontSendNotification);
    view_.setKeepPlayhead(keep);
}

void ToolBar::syncZoom()
{
    // In the piano roll the height slider sets its rows' height, with a
    // range and home of their own; the lanes' come back with the arrangement.
    if (view_.inPianoRoll() != rowZoom_)
    {
        rowZoom_ = view_.inPianoRoll();
        const juce::var laneHome = heightZoom_.getProperties()["laneHome"];
        if (rowZoom_)
        {
            heightZoom_.getProperties().set("laneHome", heightZoom_.getProperties()["home"]);
            heightZoom_.setRange(TrackerView::kMinRowHeight, TrackerView::kMaxRowHeight, 1.0);
            heightZoom_.setHome(TrackerView::kDefaultRowHeight);
        }
        else
        {
            heightZoom_.setRange(TrackerView::kMinLaneHeight, TrackerView::kMaxLaneHeight, 1.0);
            if (!laneHome.isVoid())
                heightZoom_.setHome(laneHome);
        }
        heightZoom_.setTooltip(rowZoom_ ? "Row height" : "Track height");
    }
    if (!timeZoom_.isMouseButtonDown())
        timeZoom_.setValue(-std::log2(view_.samplesPerPixel()), juce::dontSendNotification);
    if (!heightZoom_.isMouseButtonDown())
        heightZoom_.setValue(rowZoom_ ? view_.rowHeight() : view_.laneHeight(), juce::dontSendNotification);
}

ToolBar::Only ToolBar::soloed() const
{
    if (!model_.hasTune())
        return Only::None;
    bool fm = false, fmAll = true, drums = false, drumsAll = true;
    for (const auto& tr : model_.tune().tracks)
    {
        const bool drum = tr.kind == TuneTrack::Kind::Drum;
        (drum ? drums : fm) = (drum ? drums : fm) || tr.solo;
        (drum ? drumsAll : fmAll) = (drum ? drumsAll : fmAll) && tr.solo;
    }
    return fmAll && fm && !drums ? Only::Instruments : drumsAll && drums && !fm ? Only::Drums : Only::None;
}

void ToolBar::soloOnly(Only chosen)
{
    // Solo every track of one kind and none of the other, so one "only"
    // turns the other off; again, and no track is soloed.
    if (!model_.hasTune())
        return;
    const bool off = soloed() == chosen;
    model_.edit(off ? "Solo none" : chosen == Only::Instruments ? "Solo instruments" : "Solo drums", [off, chosen](Tune& tune) {
        for (auto& tr : tune.tracks)
        {
            tr.solo = !off && (tr.kind == TuneTrack::Kind::Drum) == (chosen == Only::Drums);
            tr.soloedVoices = 0;
        }
    });
}

void ToolBar::refresh()
{
    snap_.onChange(); // the grid may have changed
    loop_.setToggleState(model_.hasTune() && model_.loop().active(), juce::dontSendNotification);
    const Only only = soloed();
    instruments_.setToggleState(only == Only::Instruments, juce::dontSendNotification);
    drums_.setToggleState(only == Only::Drums, juce::dontSendNotification);
}

void ToolBar::resized()
{
    // Groups left to right, a wider gap between them; the zoom at the right.
    auto r = getLocalBounds().reduced(14, 0).withTrimmedBottom(2);
    auto button = [&](juce::Component& c, int width = 34) {
        c.setBounds(r.removeFromLeft(width).withSizeKeepingCentre(width, 28));
        r.removeFromLeft(6);
    };
    auto group = [&](juce::Slider& slider, juce::Rectangle<int>& caption, juce::Rectangle<int> g) {
        g = g.withSizeKeepingCentre(g.getWidth(), 34);
        caption = g.removeFromTop(12);
        slider.setBounds(g);
    };
    button(keepPlayhead_);
    button(loop_);
    r.removeFromLeft(10);
    button(reset_);
    button(instruments_);
    button(drums_);
    r.removeFromLeft(10);
    button(chip_);
    r.removeFromLeft(10);
    group(speed_, speedCaption_, r.removeFromLeft(140));
    r.removeFromLeft(8);
    jog_.setBounds(r.removeFromLeft(34).withSizeKeepingCentre(34, 34));

    group(timeZoom_, timeCaption_, r.removeFromRight(130));
    r.removeFromRight(14);
    group(heightZoom_, heightCaption_, r.removeFromRight(130));
    r.removeFromRight(14);
    zoomHome_.setBounds(r.removeFromRight(34).withSizeKeepingCentre(34, 28));
    r.removeFromRight(6);
    fitBoth_.setBounds(r.removeFromRight(34).withSizeKeepingCentre(34, 28));
    r.removeFromRight(6);
    fitTime_.setBounds(r.removeFromRight(34).withSizeKeepingCentre(34, 28));
    r.removeFromRight(6);
    fitTracks_.setBounds(r.removeFromRight(34).withSizeKeepingCentre(34, 28));
    r.removeFromRight(14);
    snap_.setBounds(r.removeFromRight(114).withSizeKeepingCentre(114, 36));
    syncZoom();
}

void ToolBar::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    g.setColour(theme::grid);
    g.fillRect(0, 0, getWidth(), 1);
    // The sunset stripe along the bottom edge.
    juce::ColourGradient stripe(theme::orange, 0, 0, theme::cyan, static_cast<float>(getWidth()), 0, false);
    stripe.addColour(0.5, theme::magenta);
    g.setGradientFill(stripe);
    g.fillRect(0, getHeight() - 2, getWidth(), 2);

    g.setFont(theme::mono(11.0f, true));
    g.setColour(theme::dim);
    g.drawText("SPEED " + juce::String(juce::roundToInt(model_.speed() * 100.0)) + "%", speedCaption_, juce::Justification::centred);
    g.drawText("HEIGHT", heightCaption_, juce::Justification::centred);
    g.drawText("TIME", timeCaption_, juce::Justification::centred);
}

// SlidingStrip ----------------------------------------------------------------

SlidingStrip::SlidingStrip(const juce::String& name) : name_(name)
{
    chevron_.setWantsKeyboardFocus(false);
    chevron_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    chevron_.setTooltip("Hide " + name.toLowerCase());
    chevron_.onClick = [this] {
        if (onSlide)
            onSlide();
    };
    addAndMakeVisible(chevron_);
}

void SlidingStrip::setOpen(bool open)
{
    open_ = open;
    chevron_.up = !open;
    chevron_.setTooltip((open ? "Hide " : "Show ") + name_.toLowerCase());
    chevron_.repaint();
}

void SlidingStrip::setHeaderWidth(int width)
{
    if (width == headerWidth_)
        return;
    headerWidth_ = width;
    resized();
    repaint();
}

void SlidingStrip::resized()
{
    // At the right of the header column, level with the name.
    chevron_.setBounds(headerWidth_ - 36, (kClosedHeight - 16) / 2 + 1, 24, 16);
}

void SlidingStrip::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(!open_ ? juce::MouseCursor::NormalCursor : e.y < kEdge ? juce::MouseCursor::UpDownResizeCursor : contentCursor());
}

void SlidingStrip::mouseDown(const juce::MouseEvent& e)
{
    resizing_ = open_ && e.y < kEdge;
    if (resizing_)
    {
        resizeFromY_ = e.getScreenY();
        resizeFromHeight_ = getHeight();
    }
    else if (open_)
        contentDown(e);
}

void SlidingStrip::mouseDrag(const juce::MouseEvent& e)
{
    if (resizing_)
    {
        // By screen position: the strip moves as it grows.
        if (onHeight)
            onHeight(resizeFromHeight_ + resizeFromY_ - e.getScreenY());
    }
    else if (open_)
        contentDrag(e);
}

void SlidingStrip::mouseUp(const juce::MouseEvent& e)
{
    if (!resizing_)
        contentUp(e);
    resizing_ = false;
}

float SlidingStrip::contentAlpha() const
{
    return juce::jlimit(0.0f, 1.0f, static_cast<float>(getHeight() - kClosedHeight) / static_cast<float>(std::max(1, openHeight - kClosedHeight)));
}

void SlidingStrip::paintBar(juce::Graphics& g, juce::Colour labelColour)
{
    g.fillAll(theme::panel);
    g.setColour(theme::gridBar);
    g.fillRect(0, 0, getWidth(), 1);
    g.setFont(theme::pixel(8.0f));
    g.setColour(labelColour);
    g.drawText(name_, juce::Rectangle<int>(14, 1, headerWidth_, kClosedHeight), juce::Justification::centredLeft);
    // The grip in the top edge, over the middle of the keys.
    if (const float alpha = contentAlpha(); alpha > 0.0f)
    {
        g.setColour(juce::Colour(0xffc4c6e4).withAlpha(alpha));
        const float middle = static_cast<float>(headerWidth_ + getWidth()) * 0.5f;
        g.fillRoundedRectangle(middle - 18.0f, 2.0f, 36.0f, 2.0f, 1.0f);
    }
}

void SlidingStrip::Chevron::paintButton(juce::Graphics& g, bool over, bool)
{
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(over ? theme::panelLight : theme::background);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(over ? theme::dim : theme::gridBar);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
    // Down to slide the strip away, up to bring it back.
    const auto c = r.getCentre();
    juce::Path p;
    if (up)
        p.addTriangle(c.x - 4.0f, c.y + 2.0f, c.x + 4.0f, c.y + 2.0f, c.x, c.y - 2.5f);
    else
        p.addTriangle(c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 2.5f);
    g.setColour(over ? theme::text : theme::dim);
    g.fillPath(p);
}

// OverviewStrip ---------------------------------------------------------------

OverviewStrip::OverviewStrip(AppModel& model, TrackerView& view) : SlidingStrip("OVERVIEW"), model_(model), view_(view)
{
    model_.addChangeListener(this);
    view_.addChangeListener(this);
}

OverviewStrip::~OverviewStrip()
{
    model_.removeChangeListener(this);
    view_.removeChangeListener(this);
}

void OverviewStrip::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    juce::ignoreUnused(source);
    repaint();
}

double OverviewStrip::songLength() const
{
    return model_.hasTune() ? std::max(1.0, static_cast<double>(model_.tune().length())) : 1.0;
}

juce::Rectangle<float> OverviewStrip::lanes() const
{
    return content().toFloat().withTrimmedLeft(static_cast<float>(headerWidth())).reduced(8.0f, 7.0f);
}

void OverviewStrip::paint(juce::Graphics& g)
{
    paintBar(g, theme::cyan);
    const float alpha = contentAlpha();
    if (!model_.hasTune() || alpha <= 0.0f)
        return;
    if (alpha < 1.0f)
        g.beginTransparencyLayer(alpha);

    const auto area = lanes();
    const double length = songLength();
    const auto& tracks = model_.tune().tracks;
    // Each track a thin row; each note a tick in its pitch colour.
    if (density_.isNull() || density_.getWidth() != static_cast<int>(area.getWidth())
        || density_.getHeight() != static_cast<int>(area.getHeight()) || densityRevision_ != model_.revision())
    {
        density_ = juce::Image(juce::Image::ARGB, std::max(1, static_cast<int>(area.getWidth())),
                               std::max(1, static_cast<int>(area.getHeight())), true);
        densityRevision_ = model_.revision();
        juce::Graphics d(density_);
        const float rowHeight = static_cast<float>(density_.getHeight()) / static_cast<float>(std::max<size_t>(1, tracks.size()));
        for (size_t t = 0; t < tracks.size(); ++t)
        {
            for (const TuneNote& n : tracks[t].notes)
            {
                const bool audible = model_.tune().audible(tracks[t], n);
                const float x0 = static_cast<float>(static_cast<double>(n.start) / length * density_.getWidth());
                const float w = std::max(1.0f, static_cast<float>(static_cast<double>(n.length) / length * density_.getWidth()));
                const juce::Colour c = tracks[t].kind == TuneTrack::Kind::Drum ? theme::instrumentColour(static_cast<int>(t))
                                                                               : theme::pitchColour(model_.tune().pitch(tracks[t], n));
                d.setColour(c.withAlpha(audible ? 0.75f : 0.15f));
                d.fillRect(x0, static_cast<float>(t) * rowHeight, w, std::max(1.0f, rowHeight - 0.5f));
            }
        }
    }
    g.setColour(theme::background);
    g.fillRoundedRectangle(area.expanded(2.0f), 4.0f);
    g.drawImageAt(density_, static_cast<int>(area.getX()), static_cast<int>(area.getY()));

    // The visible window.
    const float x0 = area.getX() + static_cast<float>(view_.viewStart() / length) * area.getWidth();
    const float w = std::max(6.0f, static_cast<float>(view_.visibleSamples() / length) * area.getWidth());
    const auto window = juce::Rectangle<float>(x0, area.getY() - 3, w, area.getHeight() + 6).getIntersection(area.expanded(2.0f, 3.0f));
    g.setColour(theme::cyan.withAlpha(0.12f));
    g.fillRoundedRectangle(window, 3.0f);
    g.setColour(theme::cyan);
    g.drawRoundedRectangle(window, 3.0f, 1.5f);

    // Playhead.
    const float px = area.getX() + static_cast<float>(model_.displayPosition() / length) * area.getWidth();
    g.setColour(theme::orange);
    g.fillRect(px - 0.5f, area.getY() - 4, 1.5f, area.getHeight() + 8);
    if (alpha < 1.0f)
        g.endTransparencyLayer();
}

void OverviewStrip::contentDown(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
        return;
    const auto area = lanes();
    const double t = (e.position.x - area.getX()) / area.getWidth() * songLength();
    // Grab the window where it was clicked, or centre it on the click.
    if (t >= view_.viewStart() && t <= view_.viewStart() + view_.visibleSamples())
        grabOffset_ = t - view_.viewStart();
    else
    {
        grabOffset_ = view_.visibleSamples() * 0.5;
        view_.setViewStart(t - grabOffset_);
    }
}

void OverviewStrip::contentDrag(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
        return;
    const auto area = lanes();
    const double t = (e.position.x - area.getX()) / area.getWidth() * songLength();
    view_.setViewStart(t - grabOffset_);
}

// PianoStrip ------------------------------------------------------------------

void PianoStrip::updateRange()
{
    if (revision_ == model_.revision() || !model_.hasTune())
        return;
    revision_ = model_.revision();
    int low = 127, high = 0;
    for (const TuneTrack& t : model_.tune().tracks)
        if (t.kind == TuneTrack::Kind::Fm)
            for (const TuneNote& n : t.notes)
            {
                const int p = model_.tune().pitch(t, n);
                low = std::min(low, p);
                high = std::max(high, p);
            }
    if (low > high)
        low = 48, high = 83;
    // Whole octaves, at least four, and one spare below and above; the spare
    // below stops at C1 unless a note sounds lower.
    low_ = std::clamp(std::max(low / 12 * 12 - 12, std::min(24, low / 12 * 12)), 0, 120);
    high_ = std::clamp(high / 12 * 12 + 11 + 12, low_ + 47, 127);
}

juce::Rectangle<float> PianoStrip::keysArea() const
{
    return content().withTrimmedLeft(headerWidth()).reduced(8, 6).withTrimmedTop(3).toFloat();
}

int PianoStrip::keyAt(juce::Point<float> p) const
{
    const auto area = keysArea();
    int whites = 0;
    for (int k = low_; k <= high_; ++k)
        whites += isBlackKey(k) ? 0 : 1;
    if (whites == 0 || !area.contains(p))
        return -1;
    const float keyWidth = area.getWidth() / static_cast<float>(whites);
    // A black key first, as it lies over the white ones; it sits on the line
    // after the white keys before it.
    int before = 0;
    for (int k = low_; k <= high_; ++k)
    {
        if (!isBlackKey(k))
        {
            ++before;
            continue;
        }
        const float x = area.getX() + keyWidth * static_cast<float>(before);
        if (juce::Rectangle<float>(x - keyWidth * 0.32f, area.getY(), keyWidth * 0.64f, area.getHeight() * 0.6f).contains(p))
            return k;
    }
    const int white = std::min(whites - 1, static_cast<int>((p.x - area.getX()) / keyWidth));
    for (int k = low_, n = 0; k <= high_; ++k)
        if (!isBlackKey(k) && n++ == white)
            return k;
    return -1;
}

void PianoStrip::hold(int key)
{
    if (key == held_)
        return;
    if (held_ >= 0)
        model_.previewKey(held_, false);
    held_ = -1;
    if (const int track = key >= 0 && !model_.playing() && previewTrack ? previewTrack() : -1; track >= 0)
    {
        held_ = key;
        model_.previewPatch(model_.editedPatch(track), track);
        model_.previewKey(held_, true);
    }
    repaint();
}

void PianoStrip::contentDown(const juce::MouseEvent& e)
{
    if (model_.hasTune() && !model_.playing())
        hold(keyAt(e.position));
}

void PianoStrip::contentDrag(const juce::MouseEvent& e)
{
    // Off the keys, the key down stays down until the pointer reaches another.
    if (held_ >= 0)
        if (const int key = keyAt(e.position); key >= 0)
            hold(key);
}

void PianoStrip::contentUp(const juce::MouseEvent&)
{
    hold(-1);
}

void PianoStrip::paint(juce::Graphics& g)
{
    paintBar(g, theme::magenta);
    const float alpha = contentAlpha();
    if (!model_.hasTune() || alpha <= 0.0f)
        return;
    updateRange();
    if (alpha < 1.0f)
        g.beginTransparencyLayer(alpha);

    // The colour of each key under the playhead, playing or stopped, from the
    // tracks that can be heard.
    std::array<juce::Colour, 128> lit {};
    std::array<bool, 128> on {};
    const double now = model_.displayPosition();
    for (const TuneTrack& t : model_.tune().tracks)
        {
            if (t.kind != TuneTrack::Kind::Fm || !model_.tune().audible(t))
                continue;
            auto it = std::upper_bound(t.notes.begin(), t.notes.end(), now,
                                       [](double time, const TuneNote& n) { return time < static_cast<double>(n.start); });
            // A track plays one note at a time; look back a few in case of overlaps.
            for (int back = 0; back < 4 && it != t.notes.begin(); ++back)
            {
                --it;
                if (now < static_cast<double>(it->start + it->length) && model_.tune().audible(t, *it))
                {
                    const int p = model_.tune().pitch(t, *it);
                    if (p >= 0 && p < 128 && !on[static_cast<size_t>(p)])
                        on[static_cast<size_t>(p)] = true, lit[static_cast<size_t>(p)] = theme::pitchColour(p);
                }
            }
        }
    // And the key the mouse holds down.
    if (held_ >= 0 && held_ < 128)
        on[static_cast<size_t>(held_)] = true, lit[static_cast<size_t>(held_)] = theme::pitchColour(held_);

    int whites = 0;
    for (int k = low_; k <= high_; ++k)
        whites += isBlackKey(k) ? 0 : 1;
    const auto area = keysArea();
    const float keyWidth = area.getWidth() / static_cast<float>(whites);

    // White keys, then black keys over them.
    float x = area.getX();
    for (int k = low_; k <= high_; ++k)
    {
        if (isBlackKey(k))
            continue;
        const auto key = juce::Rectangle<float>(x, area.getY(), keyWidth, area.getHeight()).reduced(0.5f, 0.0f);
        const bool down = on[static_cast<size_t>(k)];
        g.setColour(down ? lit[static_cast<size_t>(k)] : juce::Colour(0xffcfcde3));
        g.fillRoundedRectangle(key, 2.5f);
        if (down)
        {
            g.setColour(juce::Colour(0xffbcbcc6));
            g.drawRoundedRectangle(key.reduced(0.5f), 2.5f, 1.5f);
        }
        // C keys carry their octave.
        if (!down && k % 12 == 0)
        {
            g.setColour(juce::Colour(0xff6c6f8e));
            g.setFont(theme::mono(std::min(11.0f, keyWidth * 0.55f), true));
            g.drawText(theme::noteName(k).removeCharacters("-"), key.withTrimmedTop(key.getHeight() - 16.0f), juce::Justification::centred, false);
        }
        x += keyWidth;
    }
    x = area.getX();
    for (int k = low_; k <= high_; ++k)
    {
        if (!isBlackKey(k))
        {
            x += keyWidth;
            continue;
        }
        const auto key = juce::Rectangle<float>(x - keyWidth * 0.32f, area.getY(), keyWidth * 0.64f, area.getHeight() * 0.6f);
        const bool down = on[static_cast<size_t>(k)];
        g.setColour(down ? lit[static_cast<size_t>(k)] : juce::Colour(0xff1b1e30));
        g.fillRoundedRectangle(key, 2.0f);
        g.setColour(down ? juce::Colour(0xffbcbcc6) : juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(key.reduced(0.5f), 2.0f, 1.0f);
    }

    // The notes sounding, named under their keys: white with a dark outline,
    // drawn last so no key covers them.
    x = area.getX();
    for (int k = low_; k <= high_; ++k)
    {
        const bool black = isBlackKey(k);
        const float centre = black ? x : x + keyWidth * 0.5f;
        if (!black)
            x += keyWidth;
        if (!on[static_cast<size_t>(k)])
            continue;
        const float bottom = black ? area.getY() + area.getHeight() * 0.6f - 2.0f : area.getBottom() - 3.0f;
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(theme::mono(13.0f, true), theme::noteName(k).removeCharacters("-"), 0.0f, 0.0f);
        const auto bounds = glyphs.getBoundingBox(0, -1, true);
        glyphs.moveRangeOfGlyphs(0, -1, centre - bounds.getCentreX(), bottom - bounds.getBottom());
        juce::Path text;
        glyphs.createPath(text);
        g.setColour(theme::background.withAlpha(0.85f));
        g.strokePath(text, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
        g.setColour(juce::Colours::white);
        g.fillPath(text);
    }
    if (alpha < 1.0f)
        g.endTransparencyLayer();
}

// MainComponent ---------------------------------------------------------------

MainComponent::MainComponent() : look_(std::make_unique<TrackerLook>())
{
    juce::PropertiesFile::Options options;
    options.applicationName = "OPM86 Tracker";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    settings_.setStorageParameters(options);
    setLookAndFeel(look_.get());
    addAndMakeVisible(top_);
    addAndMakeVisible(tools_);
    addAndMakeVisible(view_);
    addAndMakeVisible(overview_);
    addAndMakeVisible(piano_);
    top_.onNew = [this] { whenSaved([this] { newSong(); }); };
    top_.onJson = [this] {
        juce::PopupMenu menu;
        menu.addCommandItem(&commands_, command::exportJson);
        menu.addCommandItem(&commands_, command::importJson);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&top_.jsonButton()));
    };
    top_.onOpen = [this] { whenSaved([this] { openDialog(); }); };
    top_.onSave = [this] { save(); };
    overview_.onHeight = [this](int height) {
        overviewHeight_ = std::clamp(height, kOverviewHeight, 2 * kPianoHeight); // as tall as the keys can go
        overview_.openHeight = overviewHeight_;
        overviewShown_ = overviewHeight_;
        resized();
    };
    piano_.onHeight = [this](int height) {
        pianoHeight_ = std::clamp(height, kPianoHeight, 2 * kPianoHeight);
        piano_.openHeight = pianoHeight_;
        pianoShown_ = pianoHeight_;
        resized();
    };
    // The chevrons slide a strip down or up; frame() eases it there.
    piano_.openHeight = pianoHeight_;
    overview_.openHeight = overviewHeight_;
    pianoShown_ = pianoHeight_;
    overviewShown_ = overviewHeight_;
    piano_.onSlide = [this] { piano_.setOpen(!piano_.isOpen()); };
    overview_.onSlide = [this] { overview_.setOpen(!overview_.isOpen()); };
    juce::Desktop::getInstance().addGlobalMouseListener(&activity_);
    view_.onOpenPatch = [this](int track) {
        // One patch window at a time: a gear replaces what it shows. It
        // opens where it was last.
        closePatch();
        patch_ = std::make_unique<PatchWindow>(model_, track, look_.get(), commands_);
        patchTrack_ = track;
        patchName_ = model_.tune().tracks[static_cast<size_t>(track)].name;
        // Its place only: its size depends on the instrument's voices.
        const auto place = juce::StringArray::fromTokens(settings_.getUserSettings()->getValue("patchWindow"), " ", "");
        if (place.size() >= 2)
            patch_->setTopLeftPosition(place[0].getIntValue(), place[1].getIntValue());
        patch_->onClose = [this] { juce::MessageManager::callAsync([this] { closePatch(); }); };
        patch_->onChip = [this] { openChip(); };
    };
    view_.onOpenSample = [this](int track) {
        // One sample window at a time, beside the patch window: a drum
        // track's gear replaces what it shows. It opens where it was last.
        closeSample();
        sample_ = std::make_unique<SampleWindow>(model_, track, look_.get(), commands_);
        sampleTrack_ = track;
        sampleName_ = model_.tune().tracks[static_cast<size_t>(track)].name;
        const auto place = juce::StringArray::fromTokens(settings_.getUserSettings()->getValue("sampleWindow"), " ", "");
        if (place.size() >= 2)
            sample_->setTopLeftPosition(place[0].getIntValue(), place[1].getIntValue());
        sample_->onClose = [this] { juce::MessageManager::callAsync([this] { closeSample(); }); };
    };
    // The KEYS strip plays the instrument of the patch window's track, else
    // the selected notes' track's, else the first FM track's.
    piano_.previewTrack = [this] {
        if (!model_.hasTune())
            return -1;
        const auto& tracks = model_.tune().tracks;
        auto fm = [&tracks](int t) { return t >= 0 && static_cast<size_t>(t) < tracks.size() && tracks[static_cast<size_t>(t)].kind == TuneTrack::Kind::Fm; };
        if (patch_ && fm(patchTrack_))
            return patchTrack_;
        if (const int t = view_.selectedTrack(); fm(t))
            return t;
        for (size_t t = 0; t < tracks.size(); ++t)
            if (fm(static_cast<int>(t)))
                return static_cast<int>(t);
        return -1;
    };
    tools_.onChip = [this] { commands_.invokeDirectly(command::chip, false); };
    tools_.onLoop = [this] { commands_.invokeDirectly(command::loop, false); };
    model_.onNotice = [this](const juce::String& notice) {
        status_ = notice;
        repaint();
    };
    model_.addChangeListener(this);
    // Every command comes here, from the menus and from the keys in this
    // window and in the patch window.
    commands_.registerAllCommandsForTarget(this);
    commands_.setFirstCommandTarget(this);
    addKeyListener(commands_.getKeyMappings());
    setApplicationCommandManagerToWatch(&commands_);
    setWantsKeyboardFocus(true);
    setSize(1400, 820);
}

MainComponent::~MainComponent()
{
    closePatch(); // where it was, for next time
    closeSample();
    closeChip();
    shortcuts_.reset();
    setApplicationCommandManagerToWatch(nullptr);
    removeKeyListener(commands_.getKeyMappings());
    commands_.setFirstCommandTarget(nullptr);
    juce::Desktop::getInstance().removeGlobalMouseListener(&activity_);
    model_.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

bool MainComponent::open(const juce::File& file)
{
    juce::String error;
    const bool project = file.hasFileExtension(project::kExtension) || file.hasFileExtension(".json");
    if (!(project ? model_.openProject(file, error) : model_.open(file, error)))
    {
        status_ = "Could not open " + file.getFileName() + ": " + error;
        repaint();
        return false;
    }
    opened();
    // The next Open dialog starts in this file's folder.
    settings_.getUserSettings()->setValue("lastFolder", file.getParentDirectory().getFullPathName());
    return true;
}

void MainComponent::exportJson()
{
    // Beside the project, named after it.
    const juce::File folder(settings_.getUserSettings()->getValue("lastFolder"));
    const juce::File start = model_.projectFile() != juce::File() ? model_.projectFile().withFileExtension(".json")
                                                                  : folder.getChildFile(juce::File::createLegalFileName(model_.name())).withFileExtension(".json");
    chooser_ = std::make_unique<juce::FileChooser>("Export the project as JSON", start, "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& c) {
                              if (c.getResult() == juce::File())
                                  return;
                              juce::String error;
                              const juce::File to = c.getResult().withFileExtension(".json");
                              status_ = model_.exportJson(to, error) ? "Exported " + to.getFileName() : error;
                              repaint();
                          });
}

juce::File MainComponent::exportTarget(const juce::String& extension)
{
    if (model_.projectFile() != juce::File())
        return model_.projectFile().withFileExtension(extension);
    const juce::File last(settings_.getUserSettings()->getValue("lastFolder"));
    const auto folder = last.isDirectory() ? last : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    return folder.getChildFile(juce::File::createLegalFileName(model_.title())).withFileExtension(extension);
}

void MainComponent::exportVgm()
{
    if (!model_.hasTune())
        return;
    chooser_ = std::make_unique<juce::FileChooser>("Export the song as VGM", exportTarget(".vgz"), "*.vgz;*.vgm");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& c) {
                              if (c.getResult() == juce::File())
                                  return;
                              // A .vgm stays as it is; any other name becomes a gzipped .vgz.
                              const juce::File to = c.getResult().hasFileExtension(".vgm") ? c.getResult() : c.getResult().withFileExtension(".vgz");
                              juce::String error;
                              status_ = model_.exportVgm(to, error) ? "Exported " + to.getFileName() : error;
                              repaint();
                          });
}

void MainComponent::exportMidi()
{
    if (!model_.hasTune())
        return;
    chooser_ = std::make_unique<juce::FileChooser>("Export the notes as MIDI", exportTarget(".mid"), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& c) {
                              if (c.getResult() == juce::File())
                                  return;
                              const juce::File to = c.getResult().withFileExtension(".mid");
                              juce::String error;
                              status_ = model_.exportMidi(to, error) ? "Exported " + to.getFileName() + ": the notes, without the FM sound" : error;
                              repaint();
                          });
}

void MainComponent::exportWav()
{
    if (!model_.hasTune() || model_.exporting())
        return;
    // First how to render it: the rate, the sample size, and the whole song
    // or only the loop, as chosen last time.
    auto& saved = *settings_.getUserSettings();
    auto* window = new juce::AlertWindow("Export WAV", "The song as it sounds here, from its start to the end of its releases.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addComboBox("rate", { "44100 Hz", "48000 Hz", "96000 Hz" }, "Sample rate");
    window->addComboBox("bits", { "16-bit", "24-bit" }, "Sample size");
    window->getComboBoxComponent("rate")->setSelectedItemIndex(saved.getIntValue("wavRate", 0));
    window->getComboBoxComponent("bits")->setSelectedItemIndex(saved.getIntValue("wavBits", 0));
    auto onlyLoop = std::make_shared<juce::ToggleButton>("Only the loop, once");
    onlyLoop->setSize(280, 28);
    onlyLoop->setEnabled(model_.loop().active());
    onlyLoop->setToggleState(model_.loop().active() && saved.getBoolValue("wavLoop", false), juce::dontSendNotification);
    window->addCustomComponent(onlyLoop.get());
    window->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([this, window, onlyLoop](int result) {
                                if (result != 1)
                                    return;
                                const int rate = window->getComboBoxComponent("rate")->getSelectedItemIndex();
                                const int bits = window->getComboBoxComponent("bits")->getSelectedItemIndex();
                                auto& settings = *settings_.getUserSettings();
                                settings.setValue("wavRate", rate);
                                settings.setValue("wavBits", bits);
                                if (onlyLoop->isEnabled())
                                    settings.setValue("wavLoop", onlyLoop->getToggleState());
                                AppModel::WavOptions options;
                                options.rate = rate == 1 ? 48000.0 : rate == 2 ? 96000.0 : 44100.0;
                                options.bits = bits == 1 ? 24 : 16;
                                options.loopOnly = onlyLoop->getToggleState();
                                chooser_ = std::make_unique<juce::FileChooser>("Export the song as WAV", exportTarget(".wav"), "*.wav");
                                chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                                          | juce::FileBrowserComponent::warnAboutOverwriting,
                                                      [this, options](const juce::FileChooser& c) {
                                                          if (c.getResult() == juce::File())
                                                              return;
                                                          model_.exportWav(c.getResult().withFileExtension(".wav"), options);
                                                          exportPercent_ = -1;
                                                          commands_.commandStatusChanged();
                                                          repaint();
                                                      });
                            }),
                            true);
}

void MainComponent::importJson()
{
    const juce::File last(settings_.getUserSettings()->getValue("lastFolder"));
    const auto folder = last.isDirectory() ? last : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    chooser_ = std::make_unique<juce::FileChooser>("Import a project's JSON", folder, "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& c) {
                              if (c.getResult().existsAsFile())
                                  open(c.getResult());
                          });
}

void MainComponent::newSong()
{
    model_.newProject();
    opened();
}

void MainComponent::opened()
{
    closePatch(); // its track belonged to the song before
    closeSample();
    view_.songOpened(); // so did the piano roll's
    // Start on the first bar, eight bars across.
    const double bar = model_.tune().barLength();
    const double width = std::max(200, view_.getWidth() - view_.headerWidth());
    view_.zoomAround(view_.samplesPerPixel() / (bar * 8 / width), static_cast<float>(view_.headerWidth()));
    view_.setViewStart(model_.tune().firstBeat() - bar * 0.25);
    tools_.songOpened();
    view_.grabKeyboardFocus();
    updateTitle();
}

void MainComponent::closePatch()
{
    if (patch_)
        settings_.getUserSettings()->setValue("patchWindow", juce::String(patch_->getX()) + " " + juce::String(patch_->getY()));
    patch_.reset();
}

void MainComponent::closeSample()
{
    if (sample_)
        settings_.getUserSettings()->setValue("sampleWindow", juce::String(sample_->getX()) + " " + juce::String(sample_->getY()));
    sample_.reset();
}

void MainComponent::openChip()
{
    // One chip window: asked again, it comes to the front. It opens where it was last.
    if (chip_)
    {
        chip_->toFront(true);
        return;
    }
    chip_ = std::make_unique<ChipWindow>(model_, look_.get(), commands_);
    const auto place = juce::StringArray::fromTokens(settings_.getUserSettings()->getValue("chipWindow"), " ", "");
    if (place.size() >= 2)
        chip_->setTopLeftPosition(place[0].getIntValue(), place[1].getIntValue());
    chip_->onClose = [this] { juce::MessageManager::callAsync([this] { closeChip(); }); };
    tools_.setChipOpen(true);
    commands_.commandStatusChanged();
}

void MainComponent::closeChip()
{
    if (chip_)
        settings_.getUserSettings()->setValue("chipWindow", juce::String(chip_->getX()) + " " + juce::String(chip_->getY()));
    chip_.reset();
    tools_.setChipOpen(false);
    commands_.commandStatusChanged();
}

void MainComponent::save(std::function<void()> then)
{
    if (!model_.hasTune())
        return;
    if (model_.projectFile() == juce::File())
        return saveAs(std::move(then));
    juce::String error;
    if (!model_.saveProject(model_.projectFile(), error))
    {
        status_ = error;
        repaint();
        return;
    }
    updateTitle();
    if (then)
        then();
}

void MainComponent::saveAs(std::function<void()> then)
{
    if (!model_.hasTune())
        return;
    // Beside the rip, named after it, the first time; else where the project
    // is. A song written from empty starts as Untitled, in the last folder.
    const juce::File folder(settings_.getUserSettings()->getValue("lastFolder"));
    const juce::File start = model_.projectFile() != juce::File() ? model_.projectFile()
                             : model_.imported() != juce::File() ? model_.imported().withFileExtension(project::kExtension)
                             : model_.file() != juce::File()      ? model_.file().withFileExtension(project::kExtension)
                                                                  : folder.getChildFile(juce::String("Untitled") + project::kExtension);
    chooser_ = std::make_unique<juce::FileChooser>("Save the project", start, juce::String("*") + project::kExtension);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, then](const juce::FileChooser& c) {
                              if (c.getResult() == juce::File())
                                  return;
                              juce::String error;
                              if (!model_.saveProject(c.getResult().withFileExtension(project::kExtension), error))
                              {
                                  status_ = error;
                                  repaint();
                                  return;
                              }
                              updateTitle();
                              if (then)
                                  then();
                          });
}

void MainComponent::whenSaved(std::function<void()> then)
{
    if (!model_.unsaved())
        return then();
    const juce::String name = model_.name();
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Save changes?")
                                     .withMessage("The changes to " + name + " are not saved.")
                                     .withButton("Save")
                                     .withButton("Don't Save")
                                     .withButton("Cancel")
                                     .withAssociatedComponent(this),
                                 [this, then](int result) {
                                     if (result == 1)
                                         save(then);
                                     else if (result == 2)
                                         then();
                                 });
}

void MainComponent::openDialog()
{
    // In the folder of the last file opened, or the home folder the first time.
    const juce::File last(settings_.getUserSettings()->getValue("lastFolder"));
    const auto folder = last.isDirectory() ? last : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    chooser_ = std::make_unique<juce::FileChooser>("Open a VGM rip or a project", folder, "*.vgz;*.vgm;*.opm86;*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& c) {
                              if (c.getResult().existsAsFile())
                                  open(c.getResult());
                          });
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    top_.setBounds(r.removeFromTop(kTopHeight));
    tools_.setBounds(r.removeFromTop(kToolHeight));
    r.removeFromBottom(kStatusHeight);
    overview_.setBounds(r.removeFromBottom(static_cast<int>(std::lround(overviewShown_))));
    piano_.setBounds(r.removeFromBottom(static_cast<int>(std::lround(pianoShown_))));
    view_.setBounds(r);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    // Status line: build state and conflicts.
    auto r = getLocalBounds().removeFromBottom(kStatusHeight);
    g.setColour(theme::panelLight);
    g.fillRect(r);
    r = r.reduced(14, 0);
    g.setFont(theme::mono(14.0f, true));
    juce::String left = status_;
    if (model_.exporting())
        left = "Rendering " + model_.exportFile().getFileName() + " " + juce::String(juce::roundToInt(model_.exportProgress() * 100.0)) + "%"
               + theme::dot() + "Cmd+. cancels";
    else if (left.isEmpty() && model_.hasTune())
    {
        size_t notes = 0;
        for (const auto& t : model_.tune().tracks)
            notes += t.notes.size();
        const size_t tracks = model_.tune().tracks.size();
        left = juce::String(tracks) + (tracks == 1 ? " track" : " tracks") + theme::dot() + juce::String(notes)
               + (notes == 1 ? " note" : " notes") + theme::dot() + (model_.ready() ? "exact replay ready" : "building...");
    }
    g.setColour(theme::text.withAlpha(0.85f));
    g.drawText(left, r, juce::Justification::centredLeft, true);
    if (model_.hasTune())
    {
        const size_t conflicts = model_.report().conflicts;
        g.setColour(conflicts ? theme::danger : theme::text.withAlpha(0.7f));
        const juce::String right = conflicts ? juce::String(conflicts) + " notes have no free voice (red)" : "8 FM voices" + theme::dot() + "16 PCM voices";
        g.drawText(right, r, juce::Justification::centredRight, true);

        // The pitch colours' legend in the middle, C to B, when it fits
        // between the two texts.
        constexpr int kSwatch = 26, kGap = 3, kWidth = 12 * kSwatch + 11 * kGap;
        const int leftEnd = r.getX() + juce::GlyphArrangement::getStringWidthInt(theme::mono(14.0f, true), left) + 16;
        const int rightStart = r.getRight() - juce::GlyphArrangement::getStringWidthInt(theme::mono(14.0f, true), right) - 16;
        const int x0 = std::clamp(r.getCentreX() - kWidth / 2, leftEnd, std::max(leftEnd, rightStart - kWidth));
        if (x0 + kWidth <= rightStart)
        {
            g.setFont(theme::mono(11.0f, true));
            for (int pc = 0; pc < 12; ++pc)
            {
                const auto swatch = juce::Rectangle<int>(x0 + pc * (kSwatch + kGap), r.getCentreY() - 8, kSwatch, 16).toFloat();
                g.setColour(theme::pitchColour(pc));
                g.fillRoundedRectangle(swatch, 3.0f);
                g.setColour(theme::background);
                g.drawText(theme::noteName(60 + pc).removeCharacters("-").dropLastCharacters(1), swatch, juce::Justification::centred);
            }
        }
    }
}

void MainComponent::paintOverChildren(juce::Graphics& g)
{
    if (!dragOver_)
        return;
    g.setColour(theme::magenta.withAlpha(0.15f));
    g.fillAll();
    g.setColour(theme::magenta);
    g.drawRect(getLocalBounds(), 3);
    g.setFont(theme::pixel(14.0f));
    g.drawText("DROP TO OPEN", getLocalBounds(), juce::Justification::centred);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // The commands take their keys first; one greyed out uses its key up
    // quietly, so the system does not beep.
    view_.wake();
    if (commands_.getKeyMappings()->findCommandForKeyPress(key) != 0)
        return true;
    // The arrows with modifiers no command takes act as the plain ones, as
    // the tracker has always taken them; only Shift on Up or Down means an
    // octave. Option on Up or Down fine tunes, and Cmd+Option sets the
    // velocity: those are commands of their own.
    const int code = key.getKeyCode();
    const bool octave = key.getModifiers().isShiftDown();
    const juce::CommandID id = code == juce::KeyPress::leftKey    ? command::back
                               : code == juce::KeyPress::rightKey ? command::forward
                               : code == juce::KeyPress::upKey    ? (octave ? command::octaveUp : command::up)
                               : code == juce::KeyPress::downKey  ? (octave ? command::octaveDown : command::down)
                                                                  : 0;
    if (id == 0)
        return false;
    commands_.invokeDirectly(id, false);
    return true;
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    const bool ok = files.size() == 1
                    && (files[0].endsWithIgnoreCase(".vgz") || files[0].endsWithIgnoreCase(".vgm") || files[0].endsWithIgnoreCase(project::kExtension)
                       || files[0].endsWithIgnoreCase(".json"));
    dragOver_ = ok;
    repaint();
    return ok;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver_ = false;
    repaint();
    const juce::File file(files[0]);
    whenSaved([this, file] { open(file); });
}

void MainComponent::updateTitle()
{
    // The project, or the rip opened alone, with a star while changes are not saved.
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        window->setName(model_.hasTune() ? model_.name() + (model_.unsaved() ? "*" : "") + juce::String::fromUTF8(" \xe2\x80\x94 OPM86 Tracker")
                                         : "OPM86 Tracker");
    }
}

void MainComponent::parentHierarchyChanged()
{
    updateTitle();
}

void MainComponent::fileDragExit(const juce::StringArray&)
{
    dragOver_ = false;
    repaint();
}

void MainComponent::matchHeaderWidths()
{
    piano_.setHeaderWidth(view_.headerWidth());
    overview_.setHeaderWidth(view_.headerWidth());
}

void MainComponent::frame(double timestamp)
{
    const double seconds = lastFrame_ > 0 ? std::clamp(timestamp - lastFrame_, 0.0, 0.1) : 1.0 / 60.0;
    lastFrame_ = timestamp;
    // Strips sliding open or closed ease to their height.
    bool sliding = false;
    using Slide = std::pair<SlidingStrip*, double*>;
    for (auto [strip, shown] : { Slide { &piano_, &pianoShown_ }, Slide { &overview_, &overviewShown_ } })
    {
        const double target = strip->isOpen() ? strip->openHeight : SlidingStrip::kClosedHeight;
        if (std::abs(*shown - target) < 0.01)
            continue;
        *shown += (target - *shown) * std::min(1.0, seconds * 16.0);
        if (std::abs(target - *shown) < 0.5)
            *shown = target;
        sliding = true;
    }
    if (sliding)
        resized();
    view_.followPlayhead();
    view_.updateMeters(seconds);
    // The song took over from a key held: it no longer sounds, so it goes up.
    if (model_.playing())
        piano_.letGo();
    matchHeaderWidths();
    tools_.syncZoom();
    // A WAV render under way shows its progress, and what came of it when it ends.
    if (model_.exporting())
    {
        if (const auto result = model_.takeExportResult())
        {
            status_ = *result;
            commands_.commandStatusChanged();
            repaint();
        }
        else if (const int percent = juce::roundToInt(model_.exportProgress() * 100.0); percent != exportPercent_)
        {
            exportPercent_ = percent;
            repaint(getLocalBounds().removeFromBottom(kStatusHeight));
        }
    }
    if (model_.playing() || model_.hasTune())
    {
        view_.repaint();
        top_.repaint();
        overview_.repaint();
        piano_.repaint();
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    tools_.refresh();
    commands_.commandStatusChanged(); // undo, save and the rest, greyed out or not
    // A removed track takes its patch window with it, and so does one that
    // a removal moved to another place.
    if (patch_ && (!model_.hasTune() || patchTrack_ >= static_cast<int>(model_.tune().tracks.size())
                   || model_.tune().tracks[static_cast<size_t>(patchTrack_)].name != patchName_))
        closePatch();
    if (sample_ && (!model_.hasTune() || sampleTrack_ >= static_cast<int>(model_.tune().tracks.size())
                    || model_.tune().tracks[static_cast<size_t>(sampleTrack_)].name != sampleName_))
        closeSample();
    status_.clear();
    updateTitle();
    repaint();
}

// Commands --------------------------------------------------------------------

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& ids)
{
    // In the order the menus and the shortcut sheet list them.
    ids.addArray({ command::newSong, command::open, command::save, command::saveAs, command::exportJson, command::importJson,
                   command::exportVgm, command::exportWav, command::exportMidi, command::cancelExport,
                   command::undo, command::redo, command::cut, command::copy, command::paste, command::deleteNotes,
                   command::duplicate, command::selectAll, command::up, command::down, command::octaveUp, command::octaveDown,
                   command::fineUp, command::fineDown, command::fineUpEighth, command::fineDownEighth, command::louder, command::quieter,
                   command::deselect, command::moveConflicts, command::setTempo, command::barOne,
                   command::play, command::rewind, command::back, command::forward, command::loopSelection, command::loop,
                   command::clearLoop, command::zoomIn, command::zoomOut, command::taller, command::shorter,
                   command::fitTracks, command::fitTime, command::fitBoth, command::defaultZoom, command::keepPlayhead,
                   command::showKeys, command::showOverview, command::showExpression, command::pianoRoll, command::ghostNotes,
                   command::chip, command::shortcuts });
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    constexpr int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier;
    const bool song = model_.hasTune(), selected = view_.hasSelection();
    // While a dialog is up, every command waits. The menu bar's items then
    // leave their keys to the dialog, as a file chooser's name box needs the
    // arrows and Space, and a second dialog cannot open over the first.
    const bool dialog = juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    switch (id)
    {
        case command::newSong:
            info.setInfo("New", "An empty song to write", "File", 0);
            info.addDefaultKeypress('n', cmd);
            break;
        case command::open:
            info.setInfo("Open...", "A VGM rip or a project", "File", 0);
            info.addDefaultKeypress('o', cmd);
            break;
        case command::save:
            info.setInfo("Save", "The project, where it was, or asked the first time", "File", 0);
            info.addDefaultKeypress('s', cmd);
            info.setActive(song);
            break;
        case command::saveAs:
            info.setInfo("Save As...", "The project, somewhere new", "File", 0);
            info.addDefaultKeypress('s', cmd | shift);
            info.setActive(song);
            break;
        case command::exportJson:
            info.setInfo("Export as JSON...", "The project as JSON", "File", 0);
            info.setActive(song);
            break;
        case command::importJson:
            info.setInfo("Import JSON...", "A project's JSON", "File", 0);
            break;
        case command::exportVgm:
            info.setInfo("Export VGM...", "The song as it sounds here, edits and mix in its writes, for any VGM player", "File", 0);
            info.setActive(song);
            break;
        case command::exportWav:
            info.setInfo("Export WAV...", "The song as it sounds here, rendered as audio", "File", 0);
            info.setActive(song && !model_.exporting());
            break;
        case command::exportMidi:
            info.setInfo("Export MIDI...", "The notes, for a music program; MIDI does not carry the FM sound", "File", 0);
            info.setActive(song);
            break;
        case command::cancelExport:
            info.setInfo("Cancel Export", "Stops the WAV render under way", "File", 0);
            info.addDefaultKeypress('.', cmd);
            info.setActive(model_.exporting());
            break;
        case command::undo:
            info.setInfo("Undo", "The last edit", "Edit", 0);
            info.addDefaultKeypress('z', cmd);
            info.setActive(model_.undo().canUndo());
            if (model_.undo().canUndo())
                info.shortName = "Undo " + model_.undo().getUndoDescription();
            break;
        case command::redo:
            info.setInfo("Redo", "The last edit undone", "Edit", 0);
            info.addDefaultKeypress('z', cmd | shift);
            info.setActive(model_.undo().canRedo());
            if (model_.undo().canRedo())
                info.shortName = "Redo " + model_.undo().getRedoDescription();
            break;
        case command::cut:
            info.setInfo("Cut", "The selected notes, to paste elsewhere", "Edit", 0);
            info.addDefaultKeypress('x', cmd);
            info.setActive(selected);
            break;
        case command::copy:
            info.setInfo("Copy", "The selected notes, with their tracks and voices", "Edit", 0);
            info.addDefaultKeypress('c', cmd);
            info.setActive(selected);
            break;
        case command::paste:
            info.setInfo("Paste", "The copied notes at the playhead, in their tracks or the row clicked", "Edit", 0);
            info.addDefaultKeypress('v', cmd);
            info.setActive(view_.canPaste());
            break;
        case command::deleteNotes:
            info.setInfo("Delete Notes", "The selected notes", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::deleteKey, 0);
            info.addDefaultKeypress(juce::KeyPress::backspaceKey, 0);
            info.setActive(selected);
            break;
        case command::duplicate:
            info.setInfo("Duplicate", "The selected notes, after them", "Edit", 0);
            info.addDefaultKeypress('d', cmd);
            info.setActive(selected);
            break;
        case command::selectAll:
            info.setInfo("Select All", "Every note", "Edit", 0);
            info.addDefaultKeypress('a', cmd);
            info.setActive(song);
            break;
        case command::up:
            info.setInfo("Transpose Up", "The selected notes, a semitone", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::upKey, 0);
            info.setActive(selected);
            break;
        case command::down:
            info.setInfo("Transpose Down", "The selected notes, a semitone", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::downKey, 0);
            info.setActive(selected);
            break;
        case command::octaveUp:
            info.setInfo("Transpose Up an Octave", "The selected notes", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::upKey, shift);
            info.setActive(selected);
            break;
        case command::octaveDown:
            info.setInfo("Transpose Down an Octave", "The selected notes", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::downKey, shift);
            info.setActive(selected);
            break;
        case command::fineUp:
        case command::fineDown:
        case command::fineUpEighth:
        case command::fineDownEighth:
        {
            const bool up = id == command::fineUp || id == command::fineUpEighth;
            const bool eighth = id == command::fineUpEighth || id == command::fineDownEighth;
            info.setInfo(juce::String("Fine Tune ") + (up ? "Up" : "Down") + (eighth ? " an Eighth" : ""),
                         eighth ? "The selected notes, an eighth of a semitone" : "The selected notes, a 64th of a semitone", "Edit", 0);
            info.addDefaultKeypress(up ? juce::KeyPress::upKey : juce::KeyPress::downKey, eighth ? alt | shift : alt);
            info.setActive(selected);
            break;
        }
        case command::louder:
            info.setInfo("Louder", "The selected notes' velocity, 0.75 dB up", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::upKey, cmd | alt);
            info.setActive(selected);
            break;
        case command::quieter:
            info.setInfo("Quieter", "The selected notes' velocity, 0.75 dB down", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::downKey, cmd | alt);
            info.setActive(selected);
            break;
        case command::deselect:
            // In the piano roll, Escape goes back to the arrangement.
            info.setInfo("Deselect", "No note selected; in the piano roll, back to the arrangement", "Edit", 0);
            info.addDefaultKeypress(juce::KeyPress::escapeKey, 0);
            if (view_.inPianoRoll())
                info.shortName = "Close Piano Roll";
            info.setActive(selected || view_.inPianoRoll());
            break;
        case command::moveConflicts:
            info.setInfo("Move Conflicts to Free Voices", "Each note whose voice is busy, to the free voice it plays on", "Edit", 0);
            info.setActive(song);
            break;
        case command::setTempo:
            info.setInfo("Set Tempo...", "The tempo and beats per bar, from the BPM counter", "Song", 0);
            info.setActive(song);
            break;
        case command::barOne:
            info.setInfo("Bar 1 Here", "Bar 1 starts at the playhead", "Song", 0);
            info.setActive(song);
            break;
        case command::play:
            info.setInfo("Play/Stop", "Plays from the playhead, or stops", "Transport", 0);
            info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
            info.setActive(song);
            break;
        case command::rewind:
            info.setInfo("Rewind", "Back to the song's start", "Transport", 0);
            info.addDefaultKeypress(juce::KeyPress::returnKey, 0);
            info.addDefaultKeypress(juce::KeyPress::homeKey, 0);
            info.setActive(song);
            break;
        case command::back:
        case command::forward:
        {
            // With notes selected, the arrows move them instead.
            const bool back = id == command::back;
            info.setInfo(back ? "Back a Step" : "Forward a Step", "The playhead one snap step; with notes selected, the notes", "Transport", 0);
            if (selected)
                info.shortName = back ? "Move Notes Back a Step" : "Move Notes Forward a Step";
            info.addDefaultKeypress(back ? juce::KeyPress::leftKey : juce::KeyPress::rightKey, 0);
            info.setActive(song);
            break;
        }
        case command::loopSelection:
            info.setInfo("Loop Selection", "The selected notes' span, out to the snap grid", "Transport", 0);
            info.addDefaultKeypress('l', cmd);
            info.setActive(selected);
            break;
        case command::loop:
            info.setInfo("Loop", "Playback repeats the loop; with none, the selection's or the bar's at the playhead", "Transport", 0);
            info.addDefaultKeypress('l', cmd | shift);
            info.setTicked(song && model_.loop().active());
            info.setActive(song);
            break;
        case command::clearLoop:
            info.setInfo("Clear Loop", "No loop", "Transport", 0);
            info.setActive(song && model_.loop().valid());
            break;
        case command::zoomIn:
            info.setInfo("Zoom In", "Time, around the middle", "View", 0);
            info.addDefaultKeypress('=', cmd);
            info.setActive(song);
            break;
        case command::zoomOut:
            info.setInfo("Zoom Out", "Time, around the middle", "View", 0);
            info.addDefaultKeypress('-', cmd);
            info.setActive(song);
            break;
        case command::taller:
            info.setInfo("Taller Tracks", "Every lane", "View", 0);
            info.addDefaultKeypress('=', cmd | alt);
            info.setActive(song);
            break;
        case command::shorter:
            info.setInfo("Shorter Tracks", "Every lane", "View", 0);
            info.addDefaultKeypress('-', cmd | alt);
            info.setActive(song);
            break;
        case command::fitTracks:
            info.setInfo("Fit Tracks", "Every track down", "View", 0);
            info.setActive(song);
            break;
        case command::fitTime:
            info.setInfo("Fit Time", "The whole song across", "View", 0);
            info.setActive(song);
            break;
        case command::fitBoth:
            info.setInfo("Fit Both", "Every track and the whole song", "View", 0);
            info.setActive(song);
            break;
        case command::defaultZoom:
            info.setInfo("Default Zoom", "Both zooms back to their homes", "View", 0);
            info.setActive(song);
            break;
        case command::keepPlayhead:
            info.setInfo("Keep Playhead", "The playhead never fades", "View", 0);
            info.setTicked(tools_.keepsPlayhead());
            break;
        case command::showKeys:
            info.setInfo("Show Keys", "The keyboard under the tracks", "View", 0);
            info.setTicked(piano_.isOpen());
            break;
        case command::showOverview:
            info.setInfo("Show Overview", "The whole song under the keys", "View", 0);
            info.setTicked(overview_.isOpen());
            break;
        case command::showExpression:
            info.setInfo("Show Note Expression", "Each ripped note's pitch line and level shading", "View", 0);
            info.setTicked(view_.showsExpression());
            break;
        case command::pianoRoll:
            info.setInfo("Piano Roll", "The selected notes' track, or the last clicked, by pitch", "View", 0);
            info.setTicked(view_.inPianoRoll());
            info.setActive(view_.pianoRollTarget() >= 0);
            break;
        case command::ghostNotes:
            info.setInfo("Ghost Notes", "The other tracks' notes, faint, in the piano roll", "View", 0);
            info.setTicked(view_.showsGhosts());
            info.setActive(song);
            break;
        case command::chip:
            info.setInfo("Chip", "The LFO and noise every voice shares", "View", 0);
            info.setTicked(chip_ != nullptr);
            info.setActive(song);
            break;
        case command::shortcuts:
            info.setInfo("Keyboard Shortcuts", "Every command and its keys", "Help", 0);
            info.addDefaultKeypress('?', shift);
            break;
        default:
            break;
    }
    if (dialog)
        info.setActive(false);
}

bool MainComponent::perform(const InvocationInfo& info)
{
    view_.wake();
    switch (info.commandID)
    {
        case command::newSong: whenSaved([this] { newSong(); }); break;
        case command::open: whenSaved([this] { openDialog(); }); break;
        case command::save: save(); break;
        case command::saveAs: saveAs(); break;
        case command::exportJson: exportJson(); break;
        case command::importJson: whenSaved([this] { importJson(); }); break;
        case command::exportVgm: exportVgm(); break;
        case command::exportWav: exportWav(); break;
        case command::exportMidi: exportMidi(); break;
        case command::cancelExport: model_.cancelExport(); break;
        case command::undo: model_.undoStep(); break;
        case command::redo: model_.undoStep(true); break;
        case command::cut: view_.cutSelection(); break;
        case command::copy: view_.copySelection(); break;
        case command::paste: view_.paste(); break;
        case command::deleteNotes: view_.deleteSelection(); break;
        case command::duplicate: view_.duplicateSelection(); break;
        case command::selectAll: view_.selectAll(); break;
        case command::up: view_.transposeSelection(1); break;
        case command::down: view_.transposeSelection(-1); break;
        case command::octaveUp: view_.transposeSelection(12); break;
        case command::octaveDown: view_.transposeSelection(-12); break;
        case command::fineUp: view_.fineTuneSelection(1); break;
        case command::fineDown: view_.fineTuneSelection(-1); break;
        case command::fineUpEighth: view_.fineTuneSelection(8); break;
        case command::fineDownEighth: view_.fineTuneSelection(-8); break;
        case command::louder: view_.changeVelocity(1); break;
        case command::quieter: view_.changeVelocity(-1); break;
        case command::deselect:
            if (view_.inPianoRoll())
                view_.setPianoRoll(-1);
            else
                view_.deselect();
            break;
        case command::moveConflicts: view_.moveConflicts(); break;
        case command::play:
            if (model_.playing())
                model_.stop();
            else
                model_.play();
            break;
        case command::rewind: top_.rewind(); break;
        case command::setTempo: top_.showTempo(); break;
        case command::barOne: model_.barOneHere(); break;
        case command::loopSelection:
            if (const LoopRegion loop = view_.selectionLoop(); loop.valid())
                model_.setLoop(loop);
            break;
        case command::loop:
            if (model_.hasTune())
                model_.setLoop(toggledLoop());
            break;
        case command::clearLoop:
            if (model_.hasTune())
                model_.setLoop({});
            break;
        case command::back: view_.step(-1); break;
        case command::forward: view_.step(1); break;
        case command::zoomIn: view_.zoomAround(1.5, view_.middle()); break;
        case command::zoomOut: view_.zoomAround(1 / 1.5, view_.middle()); break;
        case command::taller: view_.zoomLanes(1.3, TrackerView::kRulerHeight); break;
        case command::shorter: view_.zoomLanes(1 / 1.3, TrackerView::kRulerHeight); break;
        case command::fitTracks: view_.fitTracks(); break;
        case command::fitTime: view_.fitTime(); break;
        case command::fitBoth: view_.fitTracks(), view_.fitTime(); break;
        case command::defaultZoom: tools_.defaultZoom(); break;
        case command::keepPlayhead: tools_.setKeepPlayhead(!tools_.keepsPlayhead()); break;
        case command::showKeys: piano_.setOpen(!piano_.isOpen()); break;
        case command::showOverview: overview_.setOpen(!overview_.isOpen()); break;
        case command::showExpression: view_.setShowExpression(!view_.showsExpression()); break;
        case command::pianoRoll: view_.setPianoRoll(view_.inPianoRoll() ? -1 : view_.pianoRollTarget()); break;
        case command::ghostNotes: view_.setShowGhosts(!view_.showsGhosts()); break;
        case command::chip:
            if (chip_)
                closeChip();
            else if (model_.hasTune())
                openChip();
            break;
        case command::shortcuts: showShortcuts(); break;
        default: return false;
    }
    commands_.commandStatusChanged(); // a selection, a toggle or a strip changed
    return true;
}

LoopRegion MainComponent::toggledLoop() const
{
    LoopRegion loop = model_.loop();
    if (loop.valid())
    {
        loop.on = !loop.on;
        return loop;
    }
    // With none yet, the selection's span, else the bar the playhead is in.
    loop = view_.selectionLoop();
    if (loop.valid())
        return loop;
    const Tune& tune = model_.tune();
    const double bar = tune.barLength();
    const double start = tune.firstBeat() + std::floor((model_.position() - tune.firstBeat()) / bar) * bar;
    loop.start = std::max<int64_t>(0, std::llround(start));
    loop.end = std::llround(start + bar);
    loop.on = loop.valid();
    return loop;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Song", "Transport", "View", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    // Each menu its category's commands, in groups.
    using Group = std::vector<juce::CommandID>;
    static const std::vector<std::vector<Group>> menus = {
        { { command::newSong, command::open }, { command::save, command::saveAs }, { command::exportJson, command::importJson } },
        { { command::undo, command::redo },
          { command::cut, command::copy, command::paste },
          { command::deleteNotes, command::duplicate, command::selectAll, command::deselect },
          { command::up, command::down, command::octaveUp, command::octaveDown },
          { command::fineUp, command::fineDown, command::fineUpEighth, command::fineDownEighth, command::louder, command::quieter },
          { command::moveConflicts } },
        { { command::setTempo, command::barOne } },
        { { command::play, command::rewind }, { command::back, command::forward }, { command::loopSelection, command::loop, command::clearLoop } },
        { { command::zoomIn, command::zoomOut, command::taller, command::shorter },
          { command::fitTracks, command::fitTime, command::fitBoth, command::defaultZoom },
          { command::keepPlayhead, command::showKeys, command::showOverview, command::showExpression },
          { command::pianoRoll, command::ghostNotes },
          { command::chip } },
        { { command::shortcuts } },
    };
    juce::PopupMenu menu;
    if (index < 0 || index >= static_cast<int>(menus.size()))
        return menu;
    const auto& groups = menus[static_cast<size_t>(index)];
    for (size_t g = 0; g < groups.size(); ++g)
    {
        if (menu.getNumItems() > 0)
            menu.addSeparator();
        for (const juce::CommandID id : groups[g])
            menu.addCommandItem(&commands_, id);
        // The File menu's exports, in a menu of their own after saving.
        if (index == 0 && g == 1)
        {
            juce::PopupMenu exports;
            exports.addCommandItem(&commands_, command::exportVgm, "VGM...");
            exports.addCommandItem(&commands_, command::exportWav, "WAV...");
            exports.addCommandItem(&commands_, command::exportMidi, "MIDI...");
            exports.addSeparator();
            exports.addCommandItem(&commands_, command::cancelExport);
            menu.addSeparator();
            menu.addSubMenu("Export", exports, model_.hasTune());
        }
    }
    return menu;
}

void MainComponent::showShortcuts()
{
    // Made again each time, from the keys as they are now.
    if (shortcuts_ && shortcuts_->isVisible())
        return shortcuts_->toFront(true);
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Keyboard Shortcuts";
    options.dialogBackgroundColour = theme::panel;
    options.content.setOwned(new ShortcutSheet(commands_));
    options.componentToCentreAround = this;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    shortcuts_.reset(options.create());
    shortcuts_->setLookAndFeel(look_.get());
    shortcuts_->setVisible(true);
}

// ShortcutSheet ---------------------------------------------------------------

namespace
{
constexpr int kSheetPad = 20, kSheetColumn = 330, kSheetGap = 24;
constexpr int kSheetTitle = 30, kSheetRow = 24, kSheetSpace = 14; // a category's title, each command, and the gap after it
} // namespace

ShortcutSheet::ShortcutSheet(juce::ApplicationCommandManager& commands) : commands_(commands)
{
    // The categories in three columns of about one height: the longest first
    // into the shortest column, then each column in the commands' order.
    const auto categories = commands_.getCommandCategories();
    std::vector<int> order(static_cast<size_t>(categories.size()));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return categoryHeight(categories[a]) > categoryHeight(categories[b]); });
    columns_.resize(static_cast<size_t>(std::min(3, categories.size())));
    std::vector<std::vector<int>> placed(columns_.size());
    for (const int c : order)
    {
        auto shortest = std::min_element(columns_.begin(), columns_.end(), [](const Column& a, const Column& b) { return a.height < b.height; });
        shortest->height += categoryHeight(categories[c]);
        placed[static_cast<size_t>(shortest - columns_.begin())].push_back(c);
    }
    for (size_t k = 0; k < columns_.size(); ++k)
    {
        std::sort(placed[k].begin(), placed[k].end());
        for (const int c : placed[k])
            columns_[k].categories.add(categories[c]);
    }
    // Columns in the order of their first category.
    std::sort(columns_.begin(), columns_.end(), [&](const Column& a, const Column& b) {
        return categories.indexOf(a.categories[0]) < categories.indexOf(b.categories[0]);
    });
    int tallest = 0;
    for (const auto& column : columns_)
        tallest = std::max(tallest, column.height);
    const int count = static_cast<int>(columns_.size());
    setSize(2 * kSheetPad + count * kSheetColumn + (count - 1) * kSheetGap, 2 * kSheetPad + tallest - kSheetSpace);
}

int ShortcutSheet::categoryHeight(const juce::String& category) const
{
    return kSheetTitle + static_cast<int>(commands_.getCommandsInCategory(category).size()) * kSheetRow + kSheetSpace;
}

juce::String ShortcutSheet::describe(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    juce::String text;
    if (mods.isCommandDown())
        text << "Cmd+";
    if (mods.isCtrlDown() && !mods.isCommandDown())
        text << "Ctrl+";
    if (mods.isAltDown())
        text << "Option+";
    // Shift that only makes a sign, as for "?", goes without saying.
    const bool sign = code < 128 && juce::CharacterFunctions::isPrintable(static_cast<juce::juce_wchar>(code))
                      && !juce::CharacterFunctions::isLetterOrDigit(static_cast<juce::juce_wchar>(code));
    if (mods.isShiftDown() && !sign)
        text << "Shift+";
    using Named = std::pair<int, const char*>;
    for (const auto& [special, name] : { Named { juce::KeyPress::spaceKey, "Space" }, Named { juce::KeyPress::returnKey, "Return" },
                                         Named { juce::KeyPress::homeKey, "Home" }, Named { juce::KeyPress::deleteKey, "Delete" },
                                         Named { juce::KeyPress::backspaceKey, "Backspace" }, Named { juce::KeyPress::escapeKey, "Escape" },
                                         Named { juce::KeyPress::leftKey, "Left" }, Named { juce::KeyPress::rightKey, "Right" },
                                         Named { juce::KeyPress::upKey, "Up" }, Named { juce::KeyPress::downKey, "Down" } })
        if (code == special)
            return text + name;
    return text + juce::String::charToString(static_cast<juce::juce_wchar>(code)).toUpperCase();
}

void ShortcutSheet::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    static const juce::Colour accents[] = { theme::orange, theme::magenta, theme::cyan };
    int x = kSheetPad;
    int colour = 0;
    for (const auto& column : columns_)
    {
        int y = kSheetPad;
        for (const auto& category : column.categories)
        {
            // The category's title in the pixel font, a line under it, then
            // a row per command: its name, and its keys as key caps at the right.
            g.setColour(accents[colour++ % 3]);
            g.setFont(theme::pixel(9.0f));
            g.drawText(category.toUpperCase(), x, y, kSheetColumn, kSheetTitle - 10, juce::Justification::centredLeft, false);
            g.setColour(theme::grid);
            g.fillRect(x, y + kSheetTitle - 7, kSheetColumn, 1);
            y += kSheetTitle;
            for (const juce::CommandID id : commands_.getCommandsInCategory(category))
            {
                auto row = juce::Rectangle<int>(x, y, kSheetColumn, kSheetRow);
                const auto keys = commands_.getKeyMappings()->getKeyPressesAssignedToCommand(id);
                for (int k = keys.size(); --k >= 0;)
                {
                    const juce::String label = describe(keys[k]);
                    const int w = juce::GlyphArrangement::getStringWidthInt(theme::mono(12.0f, true), label) + 12;
                    const auto cap = row.removeFromRight(w).withSizeKeepingCentre(w, 18).toFloat();
                    row.removeFromRight(4);
                    g.setColour(theme::background);
                    g.fillRoundedRectangle(cap, 4.0f);
                    g.setColour(theme::gridBar);
                    g.drawRoundedRectangle(cap.reduced(0.5f), 4.0f, 1.0f);
                    g.setColour(theme::text);
                    g.setFont(theme::mono(12.0f, true));
                    g.drawText(label, cap, juce::Justification::centred, false);
                }
                g.setColour(keys.isEmpty() ? theme::dim : theme::text);
                g.setFont(theme::mono(13.0f, true));
                g.drawText(commands_.getNameOfCommand(id), row, juce::Justification::centredLeft, true);
                y += kSheetRow;
            }
            y += kSheetSpace;
        }
        x += kSheetColumn + kSheetGap;
    }
}

} // namespace opm86
