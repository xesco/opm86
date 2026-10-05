#include "ChipWindow.h"

#include "PatchWindow.h"
#include "Theme.h"

namespace opm86
{

// ChipView --------------------------------------------------------------------

ChipView::ChipView(AppModel& model) : model_(model)
{
    for (auto* b : { &lfoRevert_, &noiseRevert_ })
    {
        b->setColour(juce::TextButton::buttonOnColourId, theme::magenta);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    lfoRevert_.setTooltip("Revert to the game's LFO");
    lfoRevert_.onClick = [this] {
        model_.edit("Edit chip", [](Tune& tune) {
            tune.chip.lfoRate.reset(), tune.chip.lfoWave.reset(), tune.chip.pmd.reset(), tune.chip.amd.reset();
        });
    };
    noiseRevert_.setTooltip("Revert to the game's noise");
    noiseRevert_.onClick = [this] {
        model_.edit("Edit chip", [](Tune& tune) { tune.chip.noise.reset(), tune.chip.noiseFreq.reset(); });
    };
    setSize(470, 298);
    startTimerHz(30);
}

int ChipView::get(const ChipState& c, Field f)
{
    switch (f)
    {
        case Field::Rate: return c.lfoRate;
        case Field::Wave: return c.lfoWave;
        case Field::Pmd: return c.pmd;
        case Field::Amd: return c.amd;
        case Field::Noise: return c.noise ? 1 : 0;
        case Field::NoiseFreq: return c.noiseFreq;
    }
    return 0;
}

std::optional<int> ChipView::edited(const ChipEdits& c, Field f)
{
    auto of = [](const std::optional<uint8_t>& v) { return v ? std::optional<int>(*v) : std::nullopt; };
    switch (f)
    {
        case Field::Rate: return of(c.lfoRate);
        case Field::Wave: return of(c.lfoWave);
        case Field::Pmd: return of(c.pmd);
        case Field::Amd: return of(c.amd);
        case Field::Noise: return c.noise ? std::optional<int>(*c.noise ? 1 : 0) : std::nullopt;
        case Field::NoiseFreq: return of(c.noiseFreq);
    }
    return std::nullopt;
}

void ChipView::set(ChipEdits& c, Field f, std::optional<int> v)
{
    const auto u = v ? std::optional<uint8_t>(static_cast<uint8_t>(*v)) : std::nullopt;
    switch (f)
    {
        case Field::Rate: c.lfoRate = u; break;
        case Field::Wave: c.lfoWave = u; break;
        case Field::Pmd: c.pmd = u; break;
        case Field::Amd: c.amd = u; break;
        case Field::Noise: c.noise = v ? std::optional<bool>(*v != 0) : std::nullopt; break;
        case Field::NoiseFreq: c.noiseFreq = u; break;
    }
}

juce::Range<int> ChipView::range(Field f)
{
    switch (f)
    {
        case Field::Rate: return { 0, 255 };
        case Field::Wave: return { 0, 3 };
        case Field::Pmd:
        case Field::Amd: return { 0, 127 };
        case Field::Noise: return { 0, 1 };
        case Field::NoiseFreq: return { 0, 31 };
    }
    return { 0, 0 };
}

juce::String ChipView::text(Field f, int v)
{
    static const char* const kWaves[4] = { "SAW", "SQUARE", "TRI", "NOISE" };
    if (f == Field::Wave)
        return kWaves[std::clamp(v, 0, 3)];
    if (f == Field::Noise)
        return v ? "ON" : "OFF";
    return juce::String(v);
}

juce::String ChipView::describe(Field f) const
{
    // The YM2151 datasheet's name and register, and the value as the
    // register holds it; then whether it is the game's or set here.
    struct Names
    {
        const char* plain;
        const char* sheet;
        int address;
    };
    static const auto names = [] {
        std::array<Names, 6> n {};
        n[static_cast<size_t>(Field::Rate)] = { "LFO rate", "LFRQ", 0x18 };
        n[static_cast<size_t>(Field::Wave)] = { "LFO wave", "W", 0x1B };
        n[static_cast<size_t>(Field::Pmd)] = { "Vibrato depth", "PMD", 0x19 };
        n[static_cast<size_t>(Field::Amd)] = { "Tremolo depth", "AMD", 0x19 };
        n[static_cast<size_t>(Field::Noise)] = { "Noise", "NE", 0x0F };
        n[static_cast<size_t>(Field::NoiseFreq)] = { "Noise frequency", "NFRQ", 0x0F };
        return n;
    }();
    static const char* const kWaves[4] = { "saw", "square", "triangle", "noise" };
    const Names& n = names[static_cast<size_t>(f)];
    const auto set = edited(model_.tune().chip, f);
    const int v = set ? *set : get(model_.gameChip(), f);
    const juce::String plain = f == Field::Wave ? juce::String(kWaves[std::clamp(v, 0, 3)]) : f == Field::Noise ? juce::String(v ? "on" : "off") : juce::String();
    return juce::String(n.plain) + ": " + n.sheet + ", register 0x" + juce::String::toHexString(n.address).toUpperCase().paddedLeft('0', 2) + ", raw "
           + juce::String(v) + (plain.isNotEmpty() ? " (" + plain + ")" : juce::String())
           + (set ? "; set for the whole song" : "; as the game has it now");
}

void ChipView::resized()
{
    auto r = getLocalBounds().reduced(14);
    lfoPanel_ = r.removeFromTop(130);
    r.removeFromTop(10);
    noisePanel_ = r.removeFromTop(130);

    // Each section: its title line with REVERT at the right, then a row of controls.
    controls_.clear();
    auto lfo = lfoPanel_.reduced(12, 10);
    lfoRevert_.setBounds(lfo.removeFromTop(24).removeFromRight(86));
    lfo.removeFromTop(8);
    const int cell = lfo.getWidth() / 4;
    controls_.push_back({ Field::Rate, Look::Dial, lfo.removeFromLeft(cell), "RATE" });
    controls_.push_back({ Field::Wave, Look::Word, lfo.removeFromLeft(cell), "WAVE" });
    controls_.push_back({ Field::Pmd, Look::Dial, lfo.removeFromLeft(cell), "PM DEPTH" });
    controls_.push_back({ Field::Amd, Look::Dial, lfo, "AM DEPTH" });
    auto noise = noisePanel_.reduced(12, 10);
    noiseRevert_.setBounds(noise.removeFromTop(24).removeFromRight(86));
    noise.removeFromTop(8);
    controls_.push_back({ Field::Noise, Look::Toggle, noise.removeFromLeft(cell), "NOISE" });
    controls_.push_back({ Field::NoiseFreq, Look::Dial, noise.removeFromLeft(cell), "FREQUENCY" });
}

void ChipView::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    if (!model_.hasTune())
        return;
    const ChipEdits& chip = model_.tune().chip;
    lfoRevert_.setEnabled(chip.lfoRate || chip.lfoWave || chip.pmd || chip.amd);
    noiseRevert_.setEnabled(chip.noise || chip.noiseFreq);

    // Each section's panel, its title and what it reaches.
    auto section = [&g](juce::Rectangle<int> panel, const juce::String& title, const juce::String& caption) {
        drawPanel(g, panel);
        auto line = panel.reduced(12, 10).removeFromTop(24).withTrimmedRight(92);
        g.setColour(theme::magenta);
        g.setFont(theme::pixel(12.0f));
        g.drawText(title, line.removeFromLeft(80), juce::Justification::centredLeft, false);
        g.setColour(theme::magenta.withAlpha(0.85f));
        g.setFont(theme::mono(11.0f, true));
        g.drawText(caption, line, juce::Justification::centredLeft, false);
    };
    section(lfoPanel_, "LFO", "EVERY VOICE SHARES IT");
    section(noisePanel_, "NOISE", "REPLACES VOICE V8'S C2 OPERATOR");
    for (const auto& c : controls_)
        paintControl(g, c);

    // What the two inks mean, in the noise row's free space.
    auto legend = noisePanel_.reduced(12, 10).withTrimmedTop(32);
    legend = legend.withTrimmedLeft(2 * (legend.getWidth() / 4) + 8).withSizeKeepingCentre(legend.getWidth() / 2 - 8, 36);
    g.setFont(theme::mono(11.0f, true));
    g.setColour(theme::dim);
    g.drawText("DIM: AS THE GAME, NOW", legend.removeFromTop(18), juce::Justification::centredLeft, false);
    g.setColour(theme::yellow);
    g.drawText("YELLOW: SET FOR THE SONG", legend, juce::Justification::centredLeft, false);
}

void ChipView::paintControl(juce::Graphics& g, const Control& c)
{
    // A setting left to the game, dimmed; one set here in the chip's colour, its value yellow.
    const auto set = edited(model_.tune().chip, c.field);
    const int v = set ? *set : get(model_.gameChip(), c.field);
    const juce::Colour colour = set ? theme::magenta : theme::magenta.withAlpha(0.4f);
    const juce::Colour ink = set ? theme::yellow : theme::dim;
    auto r = c.bounds;
    if (c.look == Look::Dial)
    {
        const auto span = range(c.field);
        drawDial(g, r, c.name, static_cast<float>(v - span.getStart()) / static_cast<float>(span.getLength()), text(c.field, v), colour, ink);
        return;
    }
    if (c.look == Look::Word)
    {
        drawWord(g, r, c.name, text(c.field, v), true, ink);
        return;
    }
    // A switch: its name over a pill, filled while on, as the patch window's.
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText(c.name, r.removeFromTop(15), juce::Justification::centred, false);
    const auto pill = r.toFloat().withSizeKeepingCentre(56.0f, 22.0f);
    g.setColour(v ? colour : theme::background);
    g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);
    g.setColour(v ? colour : set ? theme::yellow.withAlpha(0.6f) : theme::gridBar.brighter(0.3f));
    g.drawRoundedRectangle(pill.reduced(0.5f), pill.getHeight() * 0.5f, 1.0f);
    g.setColour(v ? theme::background : ink);
    g.setFont(theme::mono(11.0f, true));
    g.drawText(text(c.field, v), pill, juce::Justification::centred, false);
}

juce::String ChipView::getTooltip()
{
    if (!model_.hasTune())
        return {};
    if (const int i = controlAt(getMouseXYRelative()); i >= 0)
        return describe(controls_[static_cast<size_t>(i)].field);
    return {};
}

int ChipView::controlAt(juce::Point<int> p) const
{
    for (size_t i = 0; i < controls_.size(); ++i)
        if (controls_[i].bounds.contains(p))
            return static_cast<int>(i);
    return -1;
}

void ChipView::commit(Field f, std::optional<int> value, bool newStep)
{
    if (edited(model_.tune().chip, f) == value)
        return;
    model_.edit("Edit chip", [f, value](Tune& tune) { set(tune.chip, f, value); }, newStep);
}

void ChipView::mouseDown(const juce::MouseEvent& e)
{
    const int i = controlAt(e.getPosition());
    if (i < 0 || !model_.hasTune())
        return;
    const Control& c = controls_[static_cast<size_t>(i)];
    // Option-click: back to the game's.
    if (e.mods.isAltDown())
    {
        commit(c.field, std::nullopt, true);
        return;
    }
    const auto set = edited(model_.tune().chip, c.field);
    const int v = set ? *set : get(model_.gameChip(), c.field);
    if (c.look == Look::Word || c.look == Look::Toggle)
    {
        // A click moves on to the next word.
        const auto span = range(c.field);
        commit(c.field, v >= span.getEnd() ? span.getStart() : v + 1, true);
        return;
    }
    dragging_ = i;
    dragFrom_ = v;
    dragY_ = e.position.y;
    dragStep_ = true;
}

void ChipView::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_ < 0 || !model_.hasTune())
        return;
    // Up turns it up; a short range takes more pixels a step.
    const Control& c = controls_[static_cast<size_t>(dragging_)];
    const auto span = range(c.field);
    const float pixels = juce::jlimit(2.0f, 14.0f, 140.0f / static_cast<float>(span.getLength()));
    const int steps = static_cast<int>((dragY_ - e.position.y) / pixels);
    const int v = juce::jlimit(span.getStart(), span.getEnd(), dragFrom_ + steps);
    // Only a real change sets the field: unset, the dial starts at the game's value.
    const auto set = edited(model_.tune().chip, c.field);
    if (v == (set ? *set : dragFrom_))
        return;
    commit(c.field, v, dragStep_);
    dragStep_ = false;
}

void ChipView::mouseUp(const juce::MouseEvent&)
{
    dragging_ = -1;
}

void ChipView::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Back to the game's value.
    if (const int i = controlAt(e.getPosition()); i >= 0 && model_.hasTune())
        commit(controls_[static_cast<size_t>(i)].field, std::nullopt, true);
}

// ChipWindow ------------------------------------------------------------------

ChipWindow::ChipWindow(AppModel& model, juce::LookAndFeel* look, juce::ApplicationCommandManager& commands)
    : DocumentWindow("Chip", theme::panel, DocumentWindow::closeButton), commands_(commands)
{
    setLookAndFeel(look);
    setUsingNativeTitleBar(true);
    setContentOwned(new ChipView(model), true);
    setResizable(false, false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

ChipWindow::~ChipWindow()
{
    setLookAndFeel(nullptr);
}

bool ChipWindow::keyPressed(const juce::KeyPress& key)
{
    return forwardCommandKey(commands_, key);
}

void ChipWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace opm86
