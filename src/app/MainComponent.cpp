#include "MainComponent.h"

#include "Theme.h"

#include <cmath>

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
        g.setColour(b.getToggleState() ? theme::background : over ? theme::text : theme::dim);
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
};

// Song time as an LED readout: bar.beat.sixteenth and m:ss.cc.
juce::String barBeat(const VgmSong& song, double t)
{
    const double beats = (t - song.firstBeat) / song.beatLength;
    const auto sixteenths = static_cast<long>(std::floor(beats * 4));
    const long bar = (sixteenths >= 0 ? sixteenths / 16 : (sixteenths - 15) / 16) + 1;
    const long inBar = ((sixteenths % 16) + 16) % 16;
    return juce::String(bar).paddedLeft('0', 3) + "." + juce::String(inBar / 4 + 1) + "." + juce::String(inBar % 4 + 1);
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
        case Icon::Loop: // two arrows chasing each other round
        {
            const float x0 = box.getX(), x1 = box.getRight(), y0 = box.getY() + 2.5f, y1 = box.getBottom() - 2.5f;
            juce::Path round;
            round.startNewSubPath(x0, box.getCentreY() + 1.0f);
            round.lineTo(x0, y0 + 3.0f);
            round.quadraticTo(x0, y0, x0 + 3.0f, y0);
            round.lineTo(x1 - 4.0f, y0);
            round.startNewSubPath(x1, box.getCentreY() - 1.0f);
            round.lineTo(x1, y1 - 3.0f);
            round.quadraticTo(x1, y1, x1 - 3.0f, y1);
            round.lineTo(x0 + 4.0f, y1);
            juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath(p, round);
            p.addTriangle(x1 - 0.5f, y0, x1 - 4.5f, y0 - 3.0f, x1 - 4.5f, y0 + 3.0f);
            p.addTriangle(x0 + 0.5f, y1, x0 + 4.5f, y1 - 3.0f, x0 + 4.5f, y1 + 3.0f);
            break;
        }
        case Icon::Follow: // the playhead held between the view's two edges
        {
            const float x0 = box.getX() - 1.0f, x1 = box.getRight() + 1.0f, y0 = box.getY(), y1 = box.getBottom();
            for (const float x : { x0, x1 - 1.5f })
                p.addRectangle(x, y0, 1.5f, box.getHeight());
            for (const float y : { y0, y1 - 1.5f })
                p.addRectangle(x0, y, 4.0f, 1.5f), p.addRectangle(x1 - 4.0f, y, 4.0f, 1.5f);
            p.addTriangle(box.getCentreX() - 3.5f, y0 + 2.0f, box.getCentreX() + 3.5f, y0 + 2.0f, box.getCentreX(), y0 + 6.5f);
            p.addRectangle(box.getCentreX() - 0.75f, y0 + 2.0f, 1.5f, box.getHeight() - 4.0f);
            break;
        }
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
        case Icon::Invert: // two arrows, one each way
        {
            const auto c = box.getCentre();
            for (const float dir : { 1.0f, -1.0f })
            {
                const float y = c.y - dir * 3.5f;
                const float tip = c.x + dir * 7.0f;
                p.addRectangle(std::min(c.x - dir * 6.0f, tip - dir * 4.0f), y - 0.75f, 9.0f, 1.5f);
                p.addTriangle(tip, y, tip - dir * 4.5f, y - 3.5f, tip - dir * 4.5f, y + 3.5f);
            }
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
        case Icon::FewerKeys: // a keyboard of three wide keys
        case Icon::MoreKeys:  // and of six narrow ones
        {
            const bool more = icon_ == Icon::MoreKeys;
            const int whites = more ? 6 : 3;
            const auto board = box.withSizeKeepingCentre(15.0f, 12.0f);
            const float w = board.getWidth() / static_cast<float>(whites);
            juce::Path outline;
            outline.addRectangle(board);
            for (int k = 1; k < whites; ++k)
            {
                outline.startNewSubPath(board.getX() + w * static_cast<float>(k), board.getY());
                outline.lineTo(board.getX() + w * static_cast<float>(k), board.getBottom());
            }
            juce::PathStrokeType(1.0f).createStrokedPath(p, outline);
            // Black keys after C, D, F and G, as on a piano from C.
            for (const int k : more ? std::vector<int> { 1, 2, 4, 5 } : std::vector<int> { 1, 2 })
                p.addRectangle(board.getX() + w * static_cast<float>(k) - w * 0.3f, board.getY(), w * 0.6f, board.getHeight() * 0.6f);
            break;
        }
        case Icon::ZoomHome: // "1:1", the zoom as it was
        {
            juce::GlyphArrangement glyphs;
            glyphs.addFittedText(theme::mono(12.0f, true), "1:1", box.getX() - 4.0f, box.getY(), box.getWidth() + 8.0f, box.getHeight(),
                                 juce::Justification::centred, 1, 1.0f);
            glyphs.createPath(p);
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
    g.setColour((getToggleState() ? theme::background : over ? theme::text : theme::dim).withMultipliedAlpha(isEnabled() ? 1.0f : 0.35f));
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
    rewind_.setTooltip("Rewind (Return)");
    play_.setTooltip("Play (Space)");
    stop_.setTooltip("Stop (Space)");
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
    for (juce::Button* b : std::initializer_list<juce::Button*> { &rewind_, &play_, &stop_, &open_ })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
}

juce::String TopBar::getTooltip()
{
    return model_.hasTune() && fileArea_.contains(getMouseXYRelative()) ? model_.file().getFullPathName() : juce::String();
}

void TopBar::rewind()
{
    // To the selection's start if there is one, else the song's; the view a beat before.
    if (model_.hasSelection())
    {
        model_.setPosition(model_.selectionStart());
        view_.setViewStart(model_.selectionStart() - model_.tune().source().beatLength);
        return;
    }
    model_.setPosition(0);
    if (model_.hasTune())
        view_.setViewStart(model_.tune().source().firstBeat - model_.tune().source().beatLength);
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
    open_.setBounds(r.removeFromRight(70));
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
    g.drawText("PLAYER  YM2151+SEGAPCM", juce::Rectangle<int>(16, 38, 200, 12), juce::Justification::topLeft);

    play_.setToggleState(model_.playing(), juce::dontSendNotification);

    // Counters.
    auto c = counters_;
    if (model_.hasTune())
    {
        const VgmSong& song = model_.tune().source();
        const double p = model_.displayPosition();
        drawLed(g, c.removeFromLeft(130), "BAR.BEAT", barBeat(song, p), theme::orange);
        c.removeFromLeft(6);
        drawLed(g, c.removeFromLeft(120), "TIME", clock(p), theme::cyan);
        c.removeFromLeft(6);
        drawLed(g, c.removeFromLeft(80), "BPM", juce::String(song.tempo() * model_.speed(), 1), theme::magenta);
        c.removeFromLeft(10);
        g.setFont(theme::mono(13.0f, true));
        g.setColour(theme::text);
        fileArea_ = c;
        g.drawText(model_.file().getFileName(), c.withTrimmedBottom(c.getHeight() / 2), juce::Justification::bottomLeft, true);
        g.setFont(theme::mono(12.0f));
        g.setColour(theme::dim);
        g.drawText("length " + clock(song.length), c.withTrimmedTop(c.getHeight() / 2 + 2), juce::Justification::topLeft, true);
    }
    else
        drawLed(g, c.removeFromLeft(130), "BAR.BEAT", "---.-.-", theme::faint);
}

// ToolBar ---------------------------------------------------------------------

ToolBar::ToolBar(AppModel& model, TrackerView& view) : model_(model), view_(view)
{
    keepPlayhead_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    keepPlayhead_.setClickingTogglesState(true);
    keepPlayhead_.setTooltip("Keep playhead");
    keepPlayhead_.onClick = [this] { view_.setKeepPlayhead(keepPlayhead_.getToggleState()); };
    follow_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    follow_.setClickingTogglesState(true);
    follow_.setToggleState(true, juce::dontSendNotification);
    follow_.setTooltip("Follow playhead");
    follow_.onClick = [this] { view_.setFollow(follow_.getToggleState()); };
    loop_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    loop_.setClickingTogglesState(true);
    loop_.setTooltip("Loop the selection, or the song");
    loop_.onClick = [this] { model_.setLoop(loop_.getToggleState()); };
    reset_.setTooltip("Reset mix");
    reset_.onClick = [this] {
        if (model_.hasTune())
            model_.edit("Reset mix", [](Tune& tune) {
                // Every track as the game played it: heard, at 0 dB.
                for (auto& tr : tune.tracks)
                    tr.mute = tr.solo = false, tr.mutedVoices = 0, tr.volume = 0.0f;
            });
    };
    invert_.setTooltip("Invert solo, or mute");
    invert_.onClick = [this] {
        if (!model_.hasTune())
            return;
        const auto& tracks = model_.tune().tracks;
        if (std::any_of(tracks.begin(), tracks.end(), [](const TuneTrack& tr) { return tr.solo || tr.mute; }))
            model_.edit("Invert mute and solo", [](Tune& tune) {
                // With solos, the solos flip, so the rest is heard; else the
                // mutes flip. A second press goes back. Mutes stay with solos.
                const bool anySolo = std::any_of(tune.tracks.begin(), tune.tracks.end(), [](const TuneTrack& tr) { return tr.solo; });
                for (auto& tr : tune.tracks)
                    (anySolo ? tr.solo : tr.mute) = !(anySolo ? tr.solo : tr.mute);
            });
    };
    instruments_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    instruments_.setTooltip("Only instruments");
    instruments_.onClick = [this] { soloOnly(Only::Instruments); };
    drums_.setColour(juce::TextButton::buttonOnColourId, theme::orange);
    drums_.setTooltip("Only drums");
    drums_.onClick = [this] { soloOnly(Only::Drums); };
    fitTracks_.setTooltip("Fit tracks");
    fitTracks_.onClick = [this] { view_.fitTracks(); };
    fitTime_.setTooltip("Fit time");
    fitTime_.onClick = [this] { view_.fitTime(); };
    fitBoth_.setTooltip("Fit both");
    fitBoth_.onClick = [this] { view_.fitTracks(), view_.fitTime(); };
    zoomHome_.setTooltip("Default zoom");
    zoomHome_.onClick = [this] {
        for (auto* slider : { &heightZoom_, &timeZoom_ })
            if (const juce::var& home = slider->getProperties()["home"]; !home.isVoid())
                slider->setValue(home, juce::sendNotificationSync);
    };
    for (juce::Button* b : std::initializer_list<juce::Button*> { &keepPlayhead_, &follow_, &loop_, &reset_, &invert_, &instruments_, &drums_, &fitTracks_, &fitTime_, &fitBoth_, &zoomHome_ })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }

    for (auto* editor : { &selectionStart_, &selectionEnd_ })
    {
        editor->setFont(theme::mono(12.0f, true));
        editor->setJustification(juce::Justification::centred);
        editor->setColour(juce::TextEditor::backgroundColourId, theme::background);
        editor->setColour(juce::TextEditor::textColourId, theme::text);
        editor->setColour(juce::TextEditor::outlineColourId, theme::gridBar);
        editor->setColour(juce::TextEditor::focusedOutlineColourId, theme::cyan);
        editor->setTextToShowWhenEmpty("-:--.---", theme::faint);
        editor->setSelectAllWhenFocused(true);
        // Return or leaving the field sets the selection; Escape puts it back.
        editor->onReturnKey = [this] { applySelection(); view_.grabKeyboardFocus(); };
        editor->onFocusLost = [this] { applySelection(); };
        editor->onEscapeKey = [this] { view_.grabKeyboardFocus(); syncSelection(); };
        addAndMakeVisible(editor);
    }
    selectionStart_.setTooltip("Selection start, m:ss.mmm");
    selectionEnd_.setTooltip("Selection end, m:ss.mmm");

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
        view_.zoomLanes(heightZoom_.getValue() / view_.laneHeight(), TrackerView::kRulerHeight);
    };
}

void ToolBar::songOpened()
{
    timeZoom_.setHome(-std::log2(view_.samplesPerPixel()));
    syncZoom();
    refresh();
}

namespace
{
// Song time as m:ss.mmm.
juce::String formatTime(double songTime)
{
    const double seconds = songTime / kVgmRate;
    const int minutes = static_cast<int>(seconds / 60.0);
    return juce::String(minutes) + ":" + juce::String(seconds - minutes * 60.0, 3).paddedLeft('0', 6);
}

// "1:23.5", "83.5" or "1:23" to song time: false for nonsense; empty, a
// blank field.
bool parseTime(const juce::String& text, double& songTime, bool& empty)
{
    const auto t = text.trim();
    empty = t.isEmpty();
    if (empty)
        return true;
    if (!t.containsOnly("0123456789.:") || t.indexOfChar(':') != t.lastIndexOfChar(':'))
        return false;
    const bool colon = t.containsChar(':');
    const double minutes = colon ? t.upToFirstOccurrenceOf(":", false, false).getDoubleValue() : 0.0;
    const double seconds = (colon ? t.fromFirstOccurrenceOf(":", false, false) : t).getDoubleValue();
    songTime = (minutes * 60.0 + seconds) * kVgmRate;
    return true;
}
} // namespace

void ToolBar::syncSelection()
{
    for (auto [editor, value] : { std::pair { &selectionStart_, model_.selectionStart() }, std::pair { &selectionEnd_, model_.selectionEnd() } })
        if (!editor->hasKeyboardFocus(true))
        {
            const auto text = model_.hasSelection() ? formatTime(value) : juce::String();
            if (editor->getText() != text)
                editor->setText(text, false);
        }
}

void ToolBar::applySelection()
{
    if (!model_.hasTune())
        return;
    double start = 0, end = 0;
    bool noStart = false, noEnd = false;
    if (parseTime(selectionStart_.getText(), start, noStart) && parseTime(selectionEnd_.getText(), end, noEnd))
    {
        // An empty start is the song's start, an empty end its end; both empty, no selection.
        if (noStart && noEnd)
            model_.clearSelection();
        else
        {
            const double from = noStart ? 0.0 : start;
            const double to = noEnd ? static_cast<double>(model_.tune().source().length) : end;
            if (to > from)
                model_.setSelection(from, to);
        }
    }
    syncSelection();
}

void ToolBar::syncZoom()
{
    if (!timeZoom_.isMouseButtonDown())
        timeZoom_.setValue(-std::log2(view_.samplesPerPixel()), juce::dontSendNotification);
    if (!heightZoom_.isMouseButtonDown())
        heightZoom_.setValue(view_.laneHeight(), juce::dontSendNotification);
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
            tr.solo = !off && (tr.kind == TuneTrack::Kind::Drum) == (chosen == Only::Drums);
    });
}

void ToolBar::refresh()
{
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
    button(follow_);
    button(loop_);
    r.removeFromLeft(10);
    button(reset_);
    button(invert_);
    button(instruments_);
    button(drums_);
    r.removeFromLeft(10);
    group(speed_, speedCaption_, r.removeFromLeft(140));
    r.removeFromLeft(14);
    for (auto [editor, caption] : { std::pair { &selectionStart_, &startCaption_ }, std::pair { &selectionEnd_, &endCaption_ } })
    {
        auto g = r.removeFromLeft(84).withSizeKeepingCentre(84, 36);
        *caption = g.removeFromTop(12);
        editor->setBounds(g.withTrimmedTop(1));
        r.removeFromLeft(6);
    }

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
    g.drawText("START", startCaption_, juce::Justification::centred);
    g.drawText("END", endCaption_, juce::Justification::centred);
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
    if (!resizing_ && open_)
        contentUp(e);
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
    return model_.hasTune() ? std::max(1.0, static_cast<double>(model_.tune().source().length)) : 1.0;
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
    // Following the playhead, playing pauses while the window is held, and
    // its moves scrub; not following, the window moves the view alone.
    held_ = view_.follow();
    resume_ = held_ && model_.playing();
    if (resume_)
        model_.stop();
    // Grab the window where it was clicked, or centre it on the click.
    if (t >= view_.viewStart() && t <= view_.viewStart() + view_.visibleSamples())
        grabOffset_ = t - view_.viewStart();
    else
    {
        grabOffset_ = view_.visibleSamples() * 0.5;
        view_.setViewStart(t - grabOffset_);
    }
    if (held_)
        centrePlayhead();
}

void OverviewStrip::contentDrag(const juce::MouseEvent& e)
{
    if (!model_.hasTune())
        return;
    const auto area = lanes();
    const double t = (e.position.x - area.getX()) / area.getWidth() * songLength();
    view_.setViewStart(t - grabOffset_);
    if (!held_)
        return;
    // Taken from playback, each place it moves to sounds for a moment.
    const double middle = view_.viewStart() + view_.visibleSamples() * 0.5;
    if (resume_)
        model_.scrub(middle);
    else
        model_.setPosition(middle);
}

void OverviewStrip::contentUp(const juce::MouseEvent&)
{
    if (resume_)
        model_.play();
    resume_ = held_ = false;
}

void OverviewStrip::centrePlayhead()
{
    // The playhead goes to the middle of the view; playing resumes from there on release.
    model_.setPosition(view_.viewStart() + view_.visibleSamples() * 0.5);
}

// PianoStrip ------------------------------------------------------------------

PianoStrip::PianoStrip(AppModel& model) : SlidingStrip("KEYS"), model_(model)
{
    fewer_.setTooltip("Fewer octaves");
    more_.setTooltip("More octaves");
    fewer_.onClick = [this] { setOctaves(octaves_ - 1); };
    more_.onClick = [this] { setOctaves(octaves_ + 1); };
    for (auto* b : { &fewer_, &more_ })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    setOctaves(octaves_);
}

void PianoStrip::resized()
{
    SlidingStrip::resized();
    // Right-aligned with the chevron, level with the keys' bottom edge.
    const int bottom = content().reduced(8, 6).getBottom();
    const int right = headerWidth() - 12; // the chevron's right edge
    more_.setBounds(right - 26, bottom - 22, 26, 22);
    fewer_.setBounds(right - 26 - 2 - 26, bottom - 22, 26, 22);
}

void PianoStrip::setOctaves(int octaves)
{
    octaves_ = std::clamp(octaves, kMinOctaves, kMaxOctaves);
    fewer_.setEnabled(octaves_ > kMinOctaves);
    more_.setEnabled(octaves_ < kMaxOctaves);
    revision_ = -1;
    repaint();
}

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
    // As many whole octaves as chosen, centred on the octaves the notes use;
    // a note outside them does not light.
    const double middle = (low / 12 + high / 12 + 1) * 0.5;
    const int first = std::clamp(static_cast<int>(std::lround(middle - octaves_ * 0.5)), 0, 10 - octaves_);
    low_ = first * 12;
    high_ = low_ + octaves_ * 12 - 1;
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
    std::array<double, 128> struck {}; // when the latest note on each key began
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
                    if (p >= 0 && p < 128)
                    {
                        const auto key = static_cast<size_t>(p);
                        struck[key] = on[key] ? std::max(struck[key], static_cast<double>(it->start)) : static_cast<double>(it->start);
                        on[key] = true;
                    }
                }
            }
        }

    // Each note flashes its key white as it begins, fading to its colour in
    // 150 ms, so the same note played again shows as a new strike.
    for (size_t k = 0; k < 128; ++k)
        if (on[k])
        {
            const double age = (now - struck[k]) / kVgmRate;
            const float flash = juce::jlimit(0.0f, 1.0f, 1.0f - static_cast<float>(age / 0.15));
            lit[k] = theme::pitchColour(static_cast<int>(k)).interpolatedWith(juce::Colours::white, flash * 0.75f);
        }

    auto isBlack = [](int key) { const int pc = key % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; };
    int whites = 0;
    for (int k = low_; k <= high_; ++k)
        whites += isBlack(k) ? 0 : 1;
    const auto area = content().withTrimmedLeft(headerWidth()).reduced(8, 6).withTrimmedTop(3).toFloat();
    const float keyWidth = area.getWidth() / static_cast<float>(whites);
    const float blackWidth = keyWidth * 0.57f, blackHeight = area.getHeight() * 0.58f; // a black key, against a white one
    // Names grow with the keys: the octave on each C, and the notes sounding.
    const float octaveSize = juce::jlimit(7.0f, 24.0f, std::min(keyWidth * 0.5f, area.getHeight() * 0.32f));
    const float nameSize = juce::jlimit(13.0f, 32.0f, std::min(keyWidth * 0.6f, area.getHeight() * 0.42f));
    const float blackNameSize = std::max(11.0f, nameSize * 0.75f); // the black keys are smaller

    // White keys, then black keys over them.
    float x = area.getX();
    for (int k = low_; k <= high_; ++k)
    {
        if (isBlack(k))
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
            g.setFont(theme::mono(octaveSize, true));
            g.drawText(theme::noteName(k).removeCharacters("-"), key.withTrimmedTop(key.getHeight() - octaveSize - 5.0f), juce::Justification::centred, false);
        }
        x += keyWidth;
    }
    x = area.getX();
    for (int k = low_; k <= high_; ++k)
    {
        if (!isBlack(k))
        {
            x += keyWidth;
            continue;
        }
        const auto key = juce::Rectangle<float>(x - blackWidth * 0.5f, area.getY(), blackWidth, blackHeight);
        const bool down = on[static_cast<size_t>(k)];
        g.setColour(down ? lit[static_cast<size_t>(k)] : juce::Colour(0xff1b1e30));
        g.fillRoundedRectangle(key, 2.0f);
        // Played, a black outline, as a played white key has a light one.
        g.setColour(down ? juce::Colours::black : juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(key.reduced(0.5f), 2.0f, down ? 1.5f : 1.0f);
    }

    // The notes sounding, named under their keys: white with a dark outline,
    // drawn last so no key covers them.
    x = area.getX();
    for (int k = low_; k <= high_; ++k)
    {
        const bool black = isBlack(k);
        const float centre = black ? x : x + keyWidth * 0.5f;
        if (!black)
            x += keyWidth;
        if (!on[static_cast<size_t>(k)])
            continue;
        const float bottom = black ? area.getY() + blackHeight - 2.0f : area.getBottom() - 3.0f;
        const float size = black ? blackNameSize : nameSize;
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(theme::mono(size, true), theme::noteName(k).removeCharacters("-"), 0.0f, 0.0f);
        const auto bounds = glyphs.getBoundingBox(0, -1, true);
        glyphs.moveRangeOfGlyphs(0, -1, centre - bounds.getCentreX(), bottom - bounds.getBottom());
        juce::Path text;
        glyphs.createPath(text);
        g.setColour(theme::background.withAlpha(0.85f));
        g.strokePath(text, juce::PathStrokeType(std::max(3.0f, size * 0.2f), juce::PathStrokeType::curved));
        g.setColour(juce::Colours::white);
        g.fillPath(text);
    }
    if (alpha < 1.0f)
        g.endTransparencyLayer();
}

// MainComponent ---------------------------------------------------------------

MainComponent::MainComponent() : look_(std::make_unique<TrackerLook>())
{
    setLookAndFeel(look_.get());
    addAndMakeVisible(top_);
    addAndMakeVisible(tools_);
    addAndMakeVisible(view_);
    addAndMakeVisible(overview_);
    addAndMakeVisible(piano_);
    top_.onOpen = [this] { openDialog(); };
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
    model_.addChangeListener(this);
    setWantsKeyboardFocus(true);
    setSize(1400, 820);
}

MainComponent::~MainComponent()
{
    juce::Desktop::getInstance().removeGlobalMouseListener(&activity_);
    model_.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

bool MainComponent::open(const juce::File& file)
{
    juce::String error;
    if (!model_.open(file, error))
    {
        status_ = "Could not open " + file.getFileName() + ": " + error;
        repaint();
        return false;
    }
    // Start on the first bar, eight bars across.
    const double bar = model_.tune().source().beatLength * 4;
    const double width = std::max(200, view_.getWidth() - view_.headerWidth());
    view_.zoomAround(view_.samplesPerPixel() / (bar * 8 / width), static_cast<float>(view_.headerWidth()));
    view_.setViewStart(model_.tune().source().firstBeat - bar * 0.25);
    tools_.songOpened();
    view_.grabKeyboardFocus();
    updateTitle();
    if (onOpened)
        onOpened(file);
    return true;
}

void MainComponent::openDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open a VGM rip", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                                                   "*.vgz;*.vgm");
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
    if (left.isEmpty() && model_.hasTune())
    {
        size_t notes = 0;
        for (const auto& t : model_.tune().tracks)
            notes += t.notes.size();
        left = juce::String(model_.tune().tracks.size()) + " tracks" + theme::dot() + juce::String(notes) + " notes" + theme::dot()
               + (model_.ready() ? "exact replay ready" : "building...");
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
    view_.wake();
    if (key == juce::KeyPress::spaceKey)
    {
        if (model_.playing())
            model_.stop();
        else
            model_.play();
        return true;
    }
    if (key == juce::KeyPress('o', juce::ModifierKeys::commandModifier, 0))
        return openDialog(), true;
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::homeKey)
        return top_.rewind(), true;
    if (key == juce::KeyPress('=', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0))
        return view_.zoomLanes(1.3, TrackerView::kRulerHeight), true;
    if (key == juce::KeyPress('-', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0))
        return view_.zoomLanes(1 / 1.3, TrackerView::kRulerHeight), true;
    if (key == juce::KeyPress('=', juce::ModifierKeys::commandModifier, 0))
        return view_.zoomAround(1.5, view_.middle()), true;
    if (key == juce::KeyPress('-', juce::ModifierKeys::commandModifier, 0))
        return view_.zoomAround(1 / 1.5, view_.middle()), true;
    return false;
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    const bool ok = files.size() == 1 && (files[0].endsWithIgnoreCase(".vgz") || files[0].endsWithIgnoreCase(".vgm"));
    dragOver_ = ok;
    repaint();
    return ok;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver_ = false;
    repaint();
    open(juce::File(files[0]));
}

void MainComponent::updateTitle()
{
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName(model_.hasTune() ? model_.file().getFileName() + juce::String::fromUTF8(" \xe2\x80\x94 OPM86 Player") : "OPM86 Player");
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
    piano_.setHeaderWidth(view_.headerWidth());
    overview_.setHeaderWidth(view_.headerWidth());
    tools_.syncZoom();
    tools_.syncSelection();
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
    status_.clear();
    repaint();
}

} // namespace opm86
