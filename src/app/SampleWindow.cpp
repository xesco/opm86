#include "SampleWindow.h"

#include "Theme.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace opm86
{

namespace
{

constexpr float kMarkerReach = 5.0f; // pixels either side of a marker that grab it

juce::String hex(int value, int digits)
{
    return "0x" + juce::String::toHexString(value).toUpperCase().paddedLeft('0', digits);
}

// The rates a sample can be made at, as the chip's steps, and the folder the
// last one came from.
constexpr uint8_t kSteps[3] = { 0x40, 0x80, 0xFF };
juce::File lastFolder;

double stepRate(uint8_t step)
{
    return step * SegaPcm::kSampleRate / 256.0;
}

} // namespace

// AudioSampleChooser ----------------------------------------------------------

void AudioSampleChooser::choose(juce::Component* parent, std::function<void(const PadSample&)> done)
{
    done_ = std::move(done);
    const juce::File folder = lastFolder.isDirectory() ? lastFolder : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    chooser_ = std::make_unique<juce::FileChooser>("Load a sample from an audio file", folder, "*.wav;*.aif;*.aiff");
    juce::Component::SafePointer<juce::Component> owner(parent);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, owner](const juce::FileChooser& c) {
        if (owner == nullptr || !c.getResult().existsAsFile())
            return;
        lastFolder = c.getResult().getParentDirectory();
        askEncoding(owner, c.getResult());
    });
}

void AudioSampleChooser::askEncoding(juce::Component* parent, const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Load sample", "Could not read " + file.getFullPathName());
        return;
    }
    // What the file holds, and how much of it each rate keeps.
    const double seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    juce::String fits;
    for (const uint8_t step : kSteps)
        fits << (fits.isEmpty() ? "" : ", ") << juce::String(static_cast<double>(kMaxSampleBytes) / stepRate(step), 1) << " s at "
             << juce::String(stepRate(step) / 1000.0, 1) << " kHz";
    options_ = std::make_unique<juce::AlertWindow>(
        "Load sample",
        file.getFileName() + ": " + juce::String(seconds, 2) + " s, " + (reader->numChannels == 1 ? "mono" : juce::String(reader->numChannels) + " channels")
            + ", " + juce::String(juce::roundToInt(reader->sampleRate)) + " Hz. The chip plays it once, in mono, from 8-bit samples: up to " + fits
            + ". The rest is cut.",
        juce::MessageBoxIconType::NoIcon, parent);
    juce::StringArray rates;
    for (const uint8_t step : kSteps)
        rates.add(juce::String(stepRate(step) / 1000.0, 1) + " kHz (step " + hex(step, 2) + ")");
    options_->addComboBox("rate", rates, "Rate");
    options_->getComboBoxComponent("rate")->setSelectedItemIndex(1);
    auto trim = std::make_shared<juce::ToggleButton>("Trim the silence at its start and end");
    auto normalise = std::make_shared<juce::ToggleButton>("Normalise to full scale");
    for (auto* toggle : { trim.get(), normalise.get() })
    {
        toggle->setSize(320, 28);
        toggle->setToggleState(true, juce::dontSendNotification);
        options_->addCustomComponent(toggle);
    }
    options_->addButton("Load", 1, juce::KeyPress(juce::KeyPress::returnKey));
    options_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<juce::AlertWindow> window(options_.get());
    options_->enterModalState(true, juce::ModalCallbackFunction::create([this, window, file, trim, normalise](int result) {
        // Gone with its owner: nothing more to do.
        if (window == nullptr)
            return;
        SampleEncoding encoding;
        encoding.step = kSteps[juce::jlimit(0, 2, window->getComboBoxComponent("rate")->getSelectedItemIndex())];
        encoding.trim = trim->getToggleState();
        encoding.normalise = normalise->getToggleState();
        options_.reset();
        if (result != 1)
            return;
        juce::AudioFormatManager manager;
        manager.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> in(manager.createReaderFor(file));
        if (in == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Load sample", "Could not read " + file.getFullPathName());
            return;
        }
        // Two minutes at most: more than any rate keeps, with silence to trim.
        const auto frames = static_cast<int>(std::min<juce::int64>(in->lengthInSamples, static_cast<juce::int64>(in->sampleRate * 120.0)));
        const auto channels = static_cast<int>(in->numChannels);
        juce::AudioBuffer<float> audio(channels, frames);
        in->read(&audio, 0, frames, 0, true, true);
        const PadSample sound = encodeSample(file.getFileNameWithoutExtension().toStdString(), audio.getArrayOfReadPointers(), channels,
                                             static_cast<size_t>(frames), in->sampleRate, encoding);
        if (sound.data.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Load sample", file.getFileName() + " has no sound");
            return;
        }
        if (done_)
            done_(sound);
    }));
}

// SampleView ------------------------------------------------------------------

SampleView::SampleView(AppModel& model, int track) : model_(model), track_(track)
{
    revert_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    revert_.setWantsKeyboardFocus(false);
    revert_.setTooltip("Revert to the game's sample");
    revert_.onClick = [this] {
        model_.edit("Revert sample", [t = track_](Tune& tune) { tune.tracks[static_cast<size_t>(t)].sample = {}; });
    };
    addAndMakeVisible(revert_);
    replace_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    replace_.setWantsKeyboardFocus(false);
    replace_.setTooltip("Load the sample's sound from a WAV or AIFF file, for every track that plays it");
    replace_.onClick = [this] {
        juce::Component::SafePointer<SampleView> self(this);
        chooser_.choose(this, [self](const PadSample& sound) {
            if (self != nullptr)
                self->replace(sound);
        });
    };
    addAndMakeVisible(replace_);
    // Middle C plays the sample at its pitch; a key up stops a sample that
    // loops, and a one-shot plays out.
    keyboard_.setLowest(48);
    keyboard_.onKey = [this](int key, bool on) {
        if (on && (model_.playing() || sample() == nullptr))
            return false;
        if (on || value(Field::Loop) != 0)
            model_.previewSample(track_, key, on);
        return true;
    };
    addAndMakeVisible(keyboard_);
    setSize(780, 14 + 64 + 10 + 220 + 10 + 130 + 10 + kKeyboardHeight + 14);
    startTimerHz(30);
}

SampleView::~SampleView()
{
    if (keyboard_.held() >= 0)
        model_.previewSample(track_, keyboard_.held(), false);
}

void SampleView::timerCallback()
{
    // The song took over from a key held: it no longer sounds, so it goes up.
    if (model_.playing())
        keyboard_.letGo();
    repaint();
}

const PadSample* SampleView::sample() const
{
    if (!model_.hasTune() || track_ >= static_cast<int>(model_.tune().tracks.size()))
        return nullptr;
    return model_.tune().sample(model_.tune().tracks[static_cast<size_t>(track_)]);
}

SampleEdit SampleView::edit() const
{
    const PadSample* pad = sample();
    return pad ? model_.tune().tracks[static_cast<size_t>(track_)].sample.fitted(*pad) : SampleEdit {};
}

bool SampleView::edited(const SampleEdit& e, Field f)
{
    switch (f)
    {
        case Field::Start: return e.start.has_value();
        case Field::End: return e.endPage.has_value();
        case Field::LoopStart: return e.loopStart.has_value();
        case Field::Loop: return e.loop.has_value();
        case Field::Step: return e.step.has_value();
        case Field::Left: return e.left.has_value();
        case Field::Right: return e.right.has_value();
    }
    return false;
}

int SampleView::value(Field f) const
{
    const PadSample* pad = sample();
    if (pad == nullptr)
        return 0;
    const SampleEdit e = edit();
    switch (f)
    {
        case Field::Start: return e.start ? *e.start : sampleFirst(*pad);
        case Field::End: return e.endPage ? *e.endPage : sampleLastPage(*pad);
        case Field::LoopStart:
            // The sample's own loop point stays between the start and the end, as the chip plays it.
            return e.loopStart ? *e.loopStart
                               : std::clamp(sampleFirst(*pad) + static_cast<int>(pad->loopStart), value(Field::Start), ((value(Field::End) + 1) << 8) - 1);
        case Field::Loop: return (e.loop ? *e.loop : pad->loop) ? 1 : 0;
        case Field::Step: return e.step ? *e.step : pad->step;
        case Field::Left: return e.left ? *e.left : pad->left;
        case Field::Right: return e.right ? *e.right : pad->right;
    }
    return 0;
}

void SampleView::set(SampleEdit& e, Field f, std::optional<int> v)
{
    auto u8 = [&v] { return v ? std::optional<uint8_t>(static_cast<uint8_t>(*v)) : std::nullopt; };
    auto u16 = [&v] { return v ? std::optional<uint16_t>(static_cast<uint16_t>(*v)) : std::nullopt; };
    switch (f)
    {
        case Field::Start: e.start = u16(); break;
        case Field::End: e.endPage = u8(); break;
        case Field::LoopStart: e.loopStart = u16(); break;
        case Field::Loop: e.loop = v ? std::optional<bool>(*v != 0) : std::nullopt; break;
        case Field::Step: e.step = u8(); break;
        case Field::Left: e.left = u8(); break;
        case Field::Right: e.right = u8(); break;
    }
}

juce::Range<int> SampleView::range(Field f) const
{
    // Inside the sample's range, the start before the end and the loop between them.
    const PadSample* pad = sample();
    if (pad == nullptr)
        return { 0, 0 };
    const int start = value(Field::Start), past = (value(Field::End) + 1) << 8;
    switch (f)
    {
        case Field::Start: return { first(), past - 1 };
        case Field::End: return { start >> 8, sampleLastPage(*pad) };
        case Field::LoopStart: return { start, past - 1 };
        case Field::Loop: return { 0, 1 };
        case Field::Step: return { 1, 255 };
        case Field::Left:
        case Field::Right: return { 0, 127 };
    }
    return { 0, 0 };
}

juce::String SampleView::describe(Field f) const
{
    // The chip's register for each channel, and the value as it holds it;
    // then whether it is the sample's own or set here.
    const int v = value(f);
    const double hz = static_cast<double>(value(Field::Step)) / 256.0 * model_.tune().source().pcmRate;
    juce::String what;
    switch (f)
    {
        case Field::Start: what = "Start: registers 0x84-0x85, raw " + hex(v, 4); break;
        case Field::End: what = "End page: register 0x06, raw " + hex(v, 2) + " (plays up to " + hex(((v + 1) << 8) - 1, 4) + ")"; break;
        case Field::LoopStart: what = "Loop start: registers 0x04-0x05, raw " + hex(v, 4); break;
        case Field::Loop: what = "Loop: register 0x86 bit 1, raw " + juce::String(v ? 0 : 1) + (v ? " (loops)" : " (plays once)"); break;
        case Field::Step: what = "Pitch: step, register 0x07, raw " + hex(v, 2) + " (" + juce::String(juce::roundToInt(hz)) + " Hz)"; break;
        case Field::Left: what = "Left volume: register 0x02, raw " + juce::String(v); break;
        case Field::Right: what = "Right volume: register 0x03, raw " + juce::String(v); break;
    }
    return what + (edited(edit(), f) ? "; set for every hit of this track" : "; the sample's own, as the game plays it");
}

void SampleView::commit(Field f, std::optional<int> v, bool newStep)
{
    const PadSample* pad = sample();
    if (pad == nullptr)
        return;
    const SampleEdit before = edit();
    SampleEdit after = before;
    set(after, f, v);
    after = after.fitted(*pad);
    if (after == before)
        return;
    if (model_.edit("Edit sample", [t = track_, after](Tune& tune) { tune.tracks[static_cast<size_t>(t)].sample = after; }, newStep))
        dragStep_ = false;
}

void SampleView::replace(const PadSample& sound)
{
    if (sample() == nullptr)
        return;
    const int index = model_.tune().tracks[static_cast<size_t>(track_)].instrument;
    // Tried on a copy first: with no room left in the ROM, nothing changes.
    Tune trial = model_.tune();
    if (trial.loadSample(index, sound) < 0)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Replace sample",
                                               "No bank of the song's ROM has room for " + juce::String(sound.data.size()) + " bytes more");
        return;
    }
    model_.edit("Replace sample", [index, sound](Tune& tune) {
        tune.loadSample(index, sound);
        // An edit of the old sample's place and pitch does not fit the new
        // one: every track that plays it starts from the new sample's own.
        for (TuneTrack& t : tune.tracks)
            if (t.kind == TuneTrack::Kind::Drum && t.instrument == index)
            {
                t.sample.start.reset();
                t.sample.endPage.reset();
                t.sample.loop.reset();
                t.sample.loopStart.reset();
                t.sample.step.reset();
            }
    });
}

int SampleView::first() const
{
    const PadSample* pad = sample();
    return pad ? sampleFirst(*pad) : 0;
}

int SampleView::end() const
{
    const PadSample* pad = sample();
    return pad ? (sampleLastPage(*pad) + 1) << 8 : 1;
}

float SampleView::addressToX(int address) const
{
    const auto box = waveBox_.toFloat();
    return box.getX() + box.getWidth() * static_cast<float>(address - first()) / static_cast<float>(std::max(1, end() - first()));
}

int SampleView::xToAddress(float x) const
{
    const auto box = waveBox_.toFloat();
    const float fraction = juce::jlimit(0.0f, 1.0f, (x - box.getX()) / std::max(1.0f, box.getWidth()));
    return first() + juce::roundToInt(fraction * static_cast<float>(end() - first()));
}

int SampleView::markerAt(juce::Point<float> p) const
{
    if (sample() == nullptr || !waveBox_.toFloat().expanded(kMarkerReach, 0.0f).contains(p))
        return -1;
    // The loop's marker over the others, as it sits between them; only while the sample loops.
    std::vector<std::pair<Field, float>> markers { { Field::Start, addressToX(value(Field::Start)) },
                                                   { Field::End, addressToX((value(Field::End) + 1) << 8) } };
    if (value(Field::Loop) != 0)
        markers.insert(markers.begin(), { Field::LoopStart, addressToX(value(Field::LoopStart)) });
    for (const auto& [field, x] : markers)
        if (std::abs(p.x - x) <= kMarkerReach)
            return static_cast<int>(field);
    return -1;
}

void SampleView::resized()
{
    auto r = getLocalBounds().reduced(14);
    header_ = r.removeFromTop(64);
    r.removeFromTop(10);
    wave_ = r.removeFromTop(220);
    r.removeFromTop(10);
    controlsPanel_ = r.removeFromTop(130);
    r.removeFromTop(10);
    keyboard_.setBounds(r.removeFromBottom(kKeyboardHeight));
    auto buttons = header_.reduced(12, 10).removeFromTop(24);
    revert_.setBounds(buttons.removeFromRight(86));
    buttons.removeFromRight(8);
    replace_.setBounds(buttons.removeFromRight(100));
    waveBox_ = wave_.reduced(12, 10).withTrimmedTop(24);

    // The playback row: the loop's switch, the pitch as a dial and as a
    // note, the two volumes, then what the two inks mean.
    controls_.clear();
    auto row = controlsPanel_.reduced(12, 10);
    row.removeFromTop(32);
    const int cell = row.getWidth() / 6;
    controls_.push_back({ Field::Loop, row.removeFromLeft(cell), "LOOP" });
    controls_.push_back({ Field::Step, row.removeFromLeft(cell), "PITCH" });
    controls_.push_back({ Field::Step, row.removeFromLeft(cell), "NOTE", true });
    controls_.push_back({ Field::Left, row.removeFromLeft(cell), "LEFT" });
    controls_.push_back({ Field::Right, row.removeFromLeft(cell), "RIGHT" });
}

void SampleView::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    const PadSample* pad = sample();
    if (pad == nullptr)
        return;
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    const SampleEdit e = edit();
    revert_.setEnabled(e.any());

    // The name, then the sample: its number, length, bank and range in the ROM.
    drawPanel(g, header_);
    auto top = header_.reduced(12, 10);
    g.setColour(theme::orange);
    g.setFont(theme::pixel(16.0f));
    g.drawText(juce::String(tr.name).toUpperCase() + (e.any() ? "*" : ""), top.removeFromTop(24).withTrimmedRight(204), juce::Justification::centredLeft,
               false);
    top.removeFromTop(4);
    const size_t bytes = pad->data.size();
    g.setColour(theme::dim);
    g.setFont(theme::mono(14.0f, true));
    g.drawText("PCM " + juce::String(tr.instrument + 1) + theme::dot() + juce::String(bytes) + " BYTES" + theme::dot() + "BANK " + hex(pad->bank, 2)
                   + theme::dot() + hex(first(), 4) + "-" + hex(end() - 1, 4),
               top.removeFromTop(20), juce::Justification::centredLeft, false);

    paintWave(g);

    drawPanel(g, controlsPanel_);
    auto title = controlsPanel_.reduced(12, 10).removeFromTop(24);
    g.setColour(theme::magenta);
    g.setFont(theme::pixel(12.0f));
    g.drawText("PLAYBACK", title.removeFromLeft(110), juce::Justification::centredLeft, false);
    g.setColour(theme::magenta.withAlpha(0.85f));
    g.setFont(theme::mono(11.0f, true));
    g.drawText("EVERY HIT OF THE TRACK, AT ITS TRANSPOSITION", title, juce::Justification::centredLeft, false);
    for (const auto& c : controls_)
        paintControl(g, c);
    auto legend = controlsPanel_.reduced(12, 10).withTrimmedTop(32);
    legend = legend.withTrimmedLeft(5 * (legend.getWidth() / 6)).withSizeKeepingCentre(legend.getWidth() / 6, 36);
    g.setFont(theme::mono(11.0f, true));
    g.setColour(theme::dim);
    g.drawText("DIM: THE GAME'S", legend.removeFromTop(18), juce::Justification::centredLeft, false);
    g.setColour(theme::yellow);
    g.drawText("YELLOW: EDITED", legend, juce::Justification::centredLeft, false);
}

void SampleView::paintWave(juce::Graphics& g)
{
    drawPanel(g, wave_);
    const PadSample* pad = sample();
    const SampleEdit e = edit();
    const int start = value(Field::Start), past = (value(Field::End) + 1) << 8, loopStart = value(Field::LoopStart);
    const bool loops = value(Field::Loop) != 0;

    // Its title, and the markers' places in their colours, yellow where edited.
    auto line = wave_.reduced(12, 10).removeFromTop(18);
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText("WAVE", line.removeFromLeft(60), juce::Justification::centredLeft, false);
    auto readout = [&](const juce::String& name, int address, bool set, juce::Colour colour) {
        g.setColour(colour);
        g.drawText(name, line.removeFromLeft(48), juce::Justification::centredLeft, false);
        g.setColour(set ? theme::yellow : theme::dim);
        g.drawText(hex(address, 4), line.removeFromLeft(78), juce::Justification::centredLeft, false);
    };
    readout("START", start, e.start.has_value(), theme::cyan);
    readout("END", past - 1, e.endPage.has_value(), theme::orange);
    if (loops)
        readout("LOOP", loopStart, e.loopStart.has_value(), theme::magenta);

    // The ground, the loop's span tinted, and the zero line.
    const auto box = waveBox_.toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(box, 4.0f);
    if (loops)
    {
        g.setColour(theme::magenta.withAlpha(0.1f));
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(addressToX(loopStart), box.getY(), addressToX(past), box.getBottom()));
    }
    g.setColour(theme::grid);
    g.fillRect(box.getX(), box.getCentreY(), box.getWidth(), 1.0f);

    // Each pixel column's lowest and highest sample, unsigned 8-bit about
    // 0x80, from the ROM as the chip reads it: bright between the start
    // and the end, faint outside them.
    const Tune& tune = model_.tune();
    const int columns = juce::jmax(1, waveBox_.getWidth());
    for (int x = 0; x < columns; ++x)
    {
        const int from = first() + (end() - first()) * x / columns;
        const int to = juce::jmax(from + 1, first() + (end() - first()) * (x + 1) / columns);
        int low = 127, high = -128;
        for (int a = from; a < to; ++a)
        {
            const int v = tune.romByte(pad->romOffset + static_cast<size_t>(a)) - 0x80;
            low = std::min(low, v), high = std::max(high, v);
        }
        const float y0 = box.getCentreY() - static_cast<float>(high) / 128.0f * (box.getHeight() * 0.5f - 2.0f);
        const float y1 = box.getCentreY() - static_cast<float>(low) / 128.0f * (box.getHeight() * 0.5f - 2.0f);
        g.setColour(from >= start && from < past ? theme::cyan : theme::faint);
        g.fillRect(box.getX() + static_cast<float>(x), y0, 1.0f, std::max(1.0f, y1 - y0));
    }

    // The markers: a line with a flag naming it, full where edited.
    auto marker = [&](float x, const juce::String& name, juce::Colour colour, bool set, bool flagRight) {
        const auto c = colour.withAlpha(set ? 1.0f : 0.6f);
        g.setColour(c);
        g.fillRect(x - 0.75f, box.getY(), 1.5f, box.getHeight());
        const juce::Rectangle<float> flag(flagRight ? x : x - 14.0f, box.getY(), 14.0f, 14.0f);
        g.fillRoundedRectangle(flag, 2.0f);
        g.setColour(theme::background);
        g.setFont(theme::mono(10.0f, true));
        g.drawText(name, flag, juce::Justification::centred, false);
    };
    marker(addressToX(start), "S", theme::cyan, e.start.has_value(), true);
    marker(addressToX(past), "E", theme::orange, e.endPage.has_value(), false);
    if (loops)
        marker(addressToX(loopStart), "L", theme::magenta, e.loopStart.has_value(), true);
}

void SampleView::paintControl(juce::Graphics& g, const Control& c)
{
    // A setting left to the game, dimmed; one set here in the chip's colour, its value yellow.
    const bool set = edited(edit(), c.field);
    const int v = value(c.field);
    const juce::Colour colour = set ? theme::magenta : theme::magenta.withAlpha(0.4f);
    const juce::Colour ink = set ? theme::yellow : theme::dim;
    auto r = c.bounds;
    if (c.field == Field::Loop)
    {
        // A switch: its name over a pill, filled while on, as the chip window's.
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
        g.drawText(v ? "ON" : "OFF", pill, juce::Justification::centred, false);
        return;
    }
    if (c.asNote)
    {
        // The pitch as a note: middle C is step 0x80, the chip's half rate,
        // with the cents to the nearest note.
        const double note = 60.0 + 12.0 * std::log2(static_cast<double>(v) / 128.0);
        const int nearest = juce::roundToInt(note);
        const int cents = juce::roundToInt((note - nearest) * 100.0);
        drawWord(g, r, c.name, theme::noteName(nearest).removeCharacters("-") + (cents == 0 ? "" : (cents > 0 ? "+" : "") + juce::String(cents)), true, ink);
        return;
    }
    const auto span = range(c.field);
    const juce::String text = c.field == Field::Step ? juce::String(juce::roundToInt(v / 256.0 * model_.tune().source().pcmRate)) + " HZ" : juce::String(v);
    drawDial(g, r, c.name, static_cast<float>(v - span.getStart()) / static_cast<float>(std::max(1, span.getLength())), text, colour, ink);
}

juce::String SampleView::getTooltip()
{
    if (sample() == nullptr)
        return {};
    const auto mouse = getMouseXYRelative();
    if (const int m = markerAt(mouse.toFloat()); m >= 0)
        return describe(static_cast<Field>(m)) + ". Drag to move it";
    if (const int i = controlAt(mouse); i >= 0)
    {
        const Control& c = controls_[static_cast<size_t>(i)];
        if (c.asNote)
            return describe(c.field) + ". As a note, with step 0x80 at middle C; the keys play the sample's own pitch at middle C";
        return describe(c.field);
    }
    if (keyboard_.getBounds().contains(mouse))
        return "Play the sample with the song stopped: middle C at its pitch";
    return {};
}

int SampleView::controlAt(juce::Point<int> p) const
{
    for (size_t i = 0; i < controls_.size(); ++i)
        if (controls_[i].bounds.contains(p))
            return static_cast<int>(i);
    return -1;
}

void SampleView::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(markerAt(e.position) >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void SampleView::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = -1;
    dragStep_ = true;
    if (sample() == nullptr)
        return;
    // A marker: Option-click puts it back, else it drags.
    if (const int m = markerAt(e.position); m >= 0)
    {
        if (e.mods.isAltDown())
            commit(static_cast<Field>(m), std::nullopt, true);
        else
            dragging_ = m;
        return;
    }
    const int i = controlAt(e.getPosition());
    if (i < 0)
        return;
    const Control& c = controls_[static_cast<size_t>(i)];
    if (e.mods.isAltDown())
    {
        commit(c.field, std::nullopt, true);
        return;
    }
    if (c.field == Field::Loop)
    {
        commit(Field::Loop, value(Field::Loop) ? 0 : 1, true);
        return;
    }
    dragging_ = static_cast<int>(c.field);
    dragFrom_ = value(c.field);
    dragY_ = e.position.y;
}

void SampleView::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_ < 0 || sample() == nullptr)
        return;
    const auto f = static_cast<Field>(dragging_);
    if (f == Field::Start || f == Field::End || f == Field::LoopStart)
    {
        // A marker follows the pointer; the end, a page boundary, to the nearest.
        const int address = xToAddress(e.position.x);
        const auto span = range(f);
        const int v = f == Field::End ? juce::roundToInt(static_cast<float>(address) / 256.0f) - 1 : address;
        commit(f, juce::jlimit(span.getStart(), span.getEnd(), v), dragStep_);
        return;
    }
    // Up turns it up; a short range takes more pixels a step.
    const auto span = range(f);
    const float pixels = juce::jlimit(1.0f, 14.0f, 140.0f / static_cast<float>(std::max(1, span.getLength())));
    const int steps = static_cast<int>((dragY_ - e.position.y) / pixels);
    const int v = juce::jlimit(span.getStart(), span.getEnd(), dragFrom_ + steps);
    // Only a real change sets the field: unset, the dial starts at the sample's value.
    if (v == value(f))
        return;
    commit(f, v, dragStep_);
}

void SampleView::mouseUp(const juce::MouseEvent&)
{
    dragging_ = -1;
}

void SampleView::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Back to the game's value.
    if (sample() == nullptr)
        return;
    if (const int m = markerAt(e.position); m >= 0)
        commit(static_cast<Field>(m), std::nullopt, true);
    else if (const int i = controlAt(e.getPosition()); i >= 0 && controls_[static_cast<size_t>(i)].field != Field::Loop)
        commit(controls_[static_cast<size_t>(i)].field, std::nullopt, true);
}

// SampleWindow ----------------------------------------------------------------

SampleWindow::SampleWindow(AppModel& model, int track, juce::LookAndFeel* look, juce::ApplicationCommandManager& commands)
    : DocumentWindow(juce::String(model.tune().tracks[static_cast<size_t>(track)].name) + juce::String::fromUTF8(" \xe2\x80\x94 sample"),
                     theme::panel, DocumentWindow::closeButton),
      commands_(commands)
{
    setLookAndFeel(look);
    setUsingNativeTitleBar(true);
    setContentOwned(new SampleView(model, track), true);
    setResizable(false, false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

SampleWindow::~SampleWindow()
{
    setLookAndFeel(nullptr);
}

bool SampleWindow::keyPressed(const juce::KeyPress& key)
{
    return forwardCommandKey(commands_, key);
}

void SampleWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace opm86
