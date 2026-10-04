#include "MainComponent.h"

#include "Theme.h"

#include <cmath>

namespace opm86
{

namespace
{

constexpr int kTopHeight = 64;
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
    }
    g.setColour(getToggleState() ? theme::background : over ? theme::text : theme::dim);
    g.fillPath(p);
}

// TopBar ----------------------------------------------------------------------

TopBar::TopBar(AppModel& model, TrackerView& view) : model_(model), view_(view)
{
    rewind_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    play_.setColour(juce::TextButton::buttonOnColourId, theme::cyan);
    stop_.setColour(juce::TextButton::buttonOnColourId, theme::orange);
    open_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    rewind_.setTooltip("Back to the start (Return)");
    play_.setTooltip("Play from the playhead (Space)");
    stop_.setTooltip("Stop; again to go back to the start (Space)");
    open_.setTooltip("Open a VGM rip (Cmd+O)");
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
    // Zoom sliders. Time goes on a log scale, right zooming in.
    for (auto* slider : { &timeZoom_, &heightZoom_ })
    {
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        slider->setWantsKeyboardFocus(false);
        addAndMakeVisible(slider);
    }
    timeZoom_.setRange(-std::log2(20000.0), -std::log2(4.0));
    timeZoom_.setTooltip("Time zoom (pinch, Cmd+scroll, Cmd+= and Cmd+-)");
    timeZoom_.onValueChange = [this] {
        view_.zoomAround(view_.samplesPerPixel() / std::exp2(-timeZoom_.getValue()), view_.middle());
    };
    heightZoom_.setRange(TrackerView::kMinLaneHeight, TrackerView::kMaxLaneHeight, 1.0);
    heightZoom_.setTooltip("Track height (Shift+pinch, Option+scroll, Option+Cmd+= and -)");
    heightZoom_.onValueChange = [this] {
        view_.zoomLanes(heightZoom_.getValue() / view_.laneHeight(), TrackerView::kRulerHeight);
    };
    for (juce::Button* b : std::initializer_list<juce::Button*> { &rewind_, &play_, &stop_, &open_ })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }

    snap_.addItem("Snap off", 1);
    snap_.addItem("Snap 1 bar", 2);
    snap_.addItem("Snap 1/2", 3);
    snap_.addItem("Snap 1/4", 4);
    snap_.addItem("Snap 1/8", 5);
    snap_.addItem("Snap 1/16", 6);
    snap_.setSelectedId(6, juce::dontSendNotification);
    snap_.setWantsKeyboardFocus(false);
    snap_.setTooltip("Grid for moving and resizing notes");
    snap_.onChange = [this] {
        static constexpr int kDivision[] = { 0, 0, 1, 2, 4, 8, 16 };
        const int division = kDivision[snap_.getSelectedId()];
        const double bar = model_.hasTune() ? model_.tune().source().beatLength * 4 : 0;
        view_.setSnap(division > 0 ? bar / division : 0);
    };
    addAndMakeVisible(snap_);
}

juce::String TopBar::getTooltip()
{
    return model_.hasTune() && fileArea_.contains(getMouseXYRelative()) ? model_.file().getFullPathName() : juce::String();
}

void TopBar::syncZoom()
{
    if (!timeZoom_.isMouseButtonDown())
        timeZoom_.setValue(-std::log2(view_.samplesPerPixel()), juce::dontSendNotification);
    if (!heightZoom_.isMouseButtonDown())
        heightZoom_.setValue(view_.laneHeight(), juce::dontSendNotification);
}

void TopBar::rewind()
{
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
    counters_ = r.removeFromLeft(std::max(360, r.getWidth() - 500));
    open_.setBounds(r.removeFromRight(70));
    r.removeFromRight(14);
    // Two zoom sliders, each under its caption.
    auto group = [&](juce::Slider& slider, juce::Rectangle<int>& caption) {
        auto g = r.removeFromRight(130);
        caption = g.removeFromTop(12);
        slider.setBounds(g);
        r.removeFromRight(14);
    };
    group(timeZoom_, timeCaption_);
    group(heightZoom_, heightCaption_);
    syncZoom();
    snap_.setBounds(r.removeFromRight(120));
    snap_.onChange();
}

void TopBar::paint(juce::Graphics& g)
{
    g.setGradientFill(juce::ColourGradient(theme::panelLight, 0, 0, theme::panel, 0, static_cast<float>(getHeight()), false));
    g.fillAll();
    // The sunset stripe along the bottom edge.
    juce::ColourGradient stripe(theme::orange, 0, 0, theme::cyan, static_cast<float>(getWidth()), 0, false);
    stripe.addColour(0.5, theme::magenta);
    g.setGradientFill(stripe);
    g.fillRect(0, getHeight() - 2, getWidth(), 2);

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
        const VgmSong& song = model_.tune().source();
        const double p = model_.displayPosition();
        drawLed(g, c.removeFromLeft(130), "BAR.BEAT", barBeat(song, p), theme::orange);
        c.removeFromLeft(6);
        drawLed(g, c.removeFromLeft(120), "TIME", clock(p), theme::cyan);
        c.removeFromLeft(6);
        drawLed(g, c.removeFromLeft(80), "BPM", juce::String(song.tempo(), 1), theme::magenta);
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

    g.setFont(theme::mono(11.0f, true));
    g.setColour(theme::dim);
    g.drawText("TIME", timeCaption_, juce::Justification::centred);
    g.drawText("HEIGHT", heightCaption_, juce::Justification::centred);
}

// OverviewStrip ---------------------------------------------------------------

OverviewStrip::OverviewStrip(AppModel& model, TrackerView& view) : model_(model), view_(view)
{
    model_.addChangeListener(this);
    view_.addChangeListener(this);
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
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
    return getLocalBounds().toFloat().withTrimmedLeft(TrackerView::kHeaderWidth).reduced(8.0f, 7.0f);
}

void OverviewStrip::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    g.setColour(theme::gridBar);
    g.fillRect(0, 0, getWidth(), 1);
    // A grip in the top edge, which drags the strip taller.
    g.setColour(theme::dim);
    for (int line = 0; line < 2; ++line)
        g.fillRoundedRectangle(static_cast<float>(getWidth()) * 0.5f - 16.0f, 2.0f + static_cast<float>(line) * 3.0f, 32.0f, 1.5f, 0.75f);
    g.setFont(theme::pixel(8.0f));
    g.setColour(theme::cyan);
    g.drawText("OVERVIEW", juce::Rectangle<int>(14, 0, TrackerView::kHeaderWidth, getHeight()), juce::Justification::centredLeft);
    if (!model_.hasTune())
        return;

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
}

void OverviewStrip::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(e.y < kEdge ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::DraggingHandCursor);
}

void OverviewStrip::mouseDown(const juce::MouseEvent& e)
{
    resizing_ = e.y < kEdge;
    if (resizing_)
    {
        resizeFromY_ = e.getScreenY();
        resizeFromHeight_ = getHeight();
        return;
    }
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

void OverviewStrip::mouseDrag(const juce::MouseEvent& e)
{
    if (resizing_)
    {
        // By screen position: the strip moves as it grows.
        if (onHeight)
            onHeight(resizeFromHeight_ + resizeFromY_ - e.getScreenY());
        return;
    }
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

void PianoStrip::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    g.setColour(theme::gridBar);
    g.fillRect(0, 0, getWidth(), 1);
    g.setFont(theme::pixel(8.0f));
    g.setColour(theme::magenta);
    g.drawText("KEYS", juce::Rectangle<int>(14, 0, TrackerView::kHeaderWidth, getHeight()), juce::Justification::centredLeft);
    if (!model_.hasTune())
        return;
    updateRange();

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

    auto isBlack = [](int key) { const int pc = key % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; };
    int whites = 0;
    for (int k = low_; k <= high_; ++k)
        whites += isBlack(k) ? 0 : 1;
    const auto area = getLocalBounds().withTrimmedLeft(TrackerView::kHeaderWidth).reduced(8, 6).toFloat();
    const float keyWidth = area.getWidth() / static_cast<float>(whites);

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
            g.setColour(juce::Colours::lightgrey.withAlpha(0.8f));
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
        if (!isBlack(k))
        {
            x += keyWidth;
            continue;
        }
        const auto key = juce::Rectangle<float>(x - keyWidth * 0.32f, area.getY(), keyWidth * 0.64f, area.getHeight() * 0.6f);
        const bool down = on[static_cast<size_t>(k)];
        g.setColour(down ? lit[static_cast<size_t>(k)] : juce::Colour(0xff1b1e30));
        g.fillRoundedRectangle(key, 2.0f);
        g.setColour(down ? juce::Colours::lightgrey.withAlpha(0.8f) : juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(key.reduced(0.5f), 2.0f, 1.0f);
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
}

// MainComponent ---------------------------------------------------------------

MainComponent::MainComponent() : look_(std::make_unique<TrackerLook>())
{
    setLookAndFeel(look_.get());
    addAndMakeVisible(top_);
    addAndMakeVisible(view_);
    addAndMakeVisible(overview_);
    addAndMakeVisible(piano_);
    top_.onOpen = [this] { openDialog(); };
    overview_.onHeight = [this](int height) {
        overviewHeight_ = std::clamp(height, kOverviewHeight, 2 * kOverviewHeight);
        resized();
    };
    model_.addChangeListener(this);
    setWantsKeyboardFocus(true);
    setSize(1400, 820);
}

MainComponent::~MainComponent()
{
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
    const double width = std::max(200, view_.getWidth() - TrackerView::kHeaderWidth);
    view_.zoomAround(view_.samplesPerPixel() / (bar * 8 / width), TrackerView::kHeaderWidth);
    view_.setViewStart(model_.tune().source().firstBeat - bar * 0.25);
    top_.resized();
    view_.grabKeyboardFocus();
    updateTitle();
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
    r.removeFromBottom(kStatusHeight);
    overview_.setBounds(r.removeFromBottom(overviewHeight_));
    piano_.setBounds(r.removeFromBottom(kPianoHeight));
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
        g.drawText(conflicts ? juce::String(conflicts) + " notes have no free voice (red)" : "8 FM voices" + theme::dot() + "16 PCM voices",
                   r, juce::Justification::centredRight, true);
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
        window->setName(model_.hasTune() ? model_.file().getFileName() + juce::String::fromUTF8(" \xe2\x80\x94 OPM86 Tracker") : "OPM86 Tracker");
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
    view_.followPlayhead();
    view_.updateMeters(seconds);
    top_.syncZoom();
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
    status_.clear();
    repaint();
}

} // namespace opm86
