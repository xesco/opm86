#include "PatchWindow.h"

#include "Instruments.h"
#include "MainComponent.h"
#include "OpmBank.h"
#include "Theme.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace opm86
{

namespace
{

constexpr int kFftOrder = 12;               // the spectrum's FFT: 4096 chip samples
constexpr int kScope = 1 << kFftOrder;      // chip samples the live views read
constexpr float kFloor = -84.0f;            // dB at the spectrum's bottom
constexpr float kFall = 40.0f;              // dB a second the spectrum falls back at
constexpr float kArc = juce::MathConstants<float>::pi * 0.75f; // a dial turns 135 degrees each way

// The keys a keyboard has in black: C#, D#, F#, G# and A#.
bool isBlackKey(int key)
{
    const int pc = key % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

} // namespace

void drawDial(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& name, float fraction, const juce::String& value, juce::Colour colour,
              juce::Colour ink)
{
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText(name, r.removeFromTop(15), juce::Justification::centred, false);
    const auto text = r.removeFromBottom(16);
    const float size = static_cast<float>(std::min(r.getWidth(), r.getHeight())) - 2.0f;
    const auto box = r.toFloat().withSizeKeepingCentre(size, size);
    const auto c = box.getCentre();
    const float radius = box.getWidth() * 0.5f - 2.0f;
    juce::Path ring;
    ring.addCentredArc(c.x, c.y, radius, radius, 0.0f, -kArc, kArc, true);
    g.setColour(juce::Colour(0xff2a2f4a));
    g.strokePath(ring, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (fraction > 0.0f)
    {
        juce::Path lit;
        lit.addCentredArc(c.x, c.y, radius, radius, 0.0f, -kArc, -kArc + 2.0f * kArc * juce::jlimit(0.0f, 1.0f, fraction), true);
        g.setColour(colour);
        g.strokePath(lit, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour(ink);
    g.setFont(theme::mono(13.0f, true));
    g.drawText(value, text, juce::Justification::centred, false);
}

void drawWord(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& name, const juce::String& word, bool on, juce::Colour ink)
{
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText(name, r.removeFromTop(15), juce::Justification::centred, false);
    const auto box = r.withSizeKeepingCentre(std::min(r.getWidth(), 64), 24).toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(theme::gridBar);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
    g.setColour(on ? ink : theme::faint);
    g.setFont(theme::mono(13.0f, true));
    g.drawText(word, box, juce::Justification::centred, false);
}

void drawPanel(juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour(theme::panelLight);
    g.fillRoundedRectangle(r.toFloat(), 6.0f);
    g.setColour(theme::grid);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 6.0f, 1.0f);
}

bool forwardCommandKey(juce::ApplicationCommandManager& commands, const juce::KeyPress& key)
{
    // The note edits are left out, as they act on notes a floating window
    // does not show. Their keys, and a greyed-out command's, are used up quietly.
    const juce::CommandID id = commands.getKeyMappings()->findCommandForKeyPress(key);
    if (id == 0)
        return false;
    switch (id)
    {
        case command::deleteNotes:
        case command::duplicate:
        case command::selectAll:
        case command::up:
        case command::down:
        case command::octaveUp:
        case command::octaveDown:
        case command::fineUp:
        case command::fineDown:
        case command::fineUpEighth:
        case command::fineDownEighth:
        case command::louder:
        case command::quieter:
        case command::deselect:
        case command::back:
        case command::forward:
            break;
        default:
        {
            juce::ApplicationCommandTarget::InvocationInfo info(id);
            info.invocationMethod = juce::ApplicationCommandTarget::InvocationInfo::fromKeyPress;
            info.keyPress = key;
            info.isKeyDown = true;
            commands.invoke(info, false);
            break;
        }
    }
    return true;
}

// TestKeyboard ----------------------------------------------------------------

TestKeyboard::TestKeyboard()
{
    for (auto* b : { &down_, &up_ })
    {
        b->setColour(juce::TextButton::buttonOnColourId, theme::cyan);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    down_.setTooltip("An octave down");
    up_.setTooltip("An octave up");
    down_.onClick = [this] { setLowest(lowest_ - 12); };
    up_.onClick = [this] { setLowest(lowest_ + 12); };
}

void TestKeyboard::setLowest(int key)
{
    // From C0 up to the C whose two octaves end at C9.
    lowest_ = std::clamp(key / 12 * 12, 12, 96);
    down_.setEnabled(lowest_ > 12);
    up_.setEnabled(lowest_ < 96);
    repaint();
}

void TestKeyboard::resized()
{
    // The title, and under it the arrows round the lowest key's name, in the
    // same column as the live strip's voices.
    auto column = getLocalBounds().reduced(10).removeFromLeft(140).withTrimmedTop(20).removeFromTop(24);
    down_.setBounds(column.removeFromLeft(26));
    column.removeFromLeft(6);
    octave_ = column.removeFromLeft(52);
    column.removeFromLeft(6);
    up_.setBounds(column.removeFromLeft(26));
}

juce::Rectangle<float> TestKeyboard::keysArea() const
{
    return getLocalBounds().reduced(10).withTrimmedLeft(150).toFloat();
}

juce::Rectangle<float> TestKeyboard::keyBounds(int key) const
{
    const auto area = keysArea();
    const float keyWidth = area.getWidth() / 15.0f; // 15 white keys
    int whites = 0;
    for (int k = lowest_; k < key; ++k)
        whites += isBlackKey(k) ? 0 : 1;
    const float x = area.getX() + keyWidth * static_cast<float>(whites);
    return isBlackKey(key) ? juce::Rectangle<float>(x - keyWidth * 0.32f, area.getY(), keyWidth * 0.64f, area.getHeight() * 0.6f)
                        : juce::Rectangle<float>(x, area.getY(), keyWidth, area.getHeight());
}

int TestKeyboard::keyAt(juce::Point<float> p) const
{
    for (const bool black : { true, false })
        for (int k = lowest_; k < lowest_ + kKeys; ++k)
            if (isBlackKey(k) == black && keyBounds(k).contains(p))
                return k;
    return -1;
}

void TestKeyboard::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds());
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText("KEYS", getLocalBounds().reduced(10).removeFromLeft(140).removeFromTop(16), juce::Justification::centredLeft, false);
    g.setColour(theme::text);
    g.setFont(theme::mono(13.0f, true));
    g.drawText(theme::noteName(lowest_).removeCharacters("-"), octave_, juce::Justification::centred, false);

    // White keys, then black keys over them, as the main window's keys are
    // drawn; the key held in its pitch colour.
    for (int k = lowest_; k < lowest_ + kKeys; ++k)
    {
        if (isBlackKey(k))
            continue;
        const auto key = keyBounds(k).reduced(0.5f, 0.0f);
        const bool down = k == held_;
        g.setColour(down ? theme::pitchColour(k) : juce::Colour(0xffcfcde3));
        g.fillRoundedRectangle(key, 2.5f);
        if (k % 12 == 0)
        {
            g.setColour(down ? juce::Colours::white : juce::Colour(0xff6c6f8e));
            g.setFont(theme::mono(std::min(11.0f, key.getWidth() * 0.55f), true));
            g.drawText(theme::noteName(k).removeCharacters("-"), key.withTrimmedTop(key.getHeight() - 16.0f), juce::Justification::centred, false);
        }
    }
    for (int k = lowest_; k < lowest_ + kKeys; ++k)
    {
        if (!isBlackKey(k))
            continue;
        const auto key = keyBounds(k);
        const bool down = k == held_;
        g.setColour(down ? theme::pitchColour(k) : juce::Colour(0xff1b1e30));
        g.fillRoundedRectangle(key, 2.0f);
        g.setColour(down ? juce::Colour(0xffbcbcc6) : juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(key.reduced(0.5f), 2.0f, 1.0f);
    }
}

void TestKeyboard::hold(int key)
{
    if (key == held_)
        return;
    if (held_ >= 0 && onKey)
        onKey(held_, false);
    held_ = key >= 0 && onKey && onKey(key, true) ? key : -1;
    repaint();
}

void TestKeyboard::mouseDown(const juce::MouseEvent& e)
{
    hold(keyAt(e.position));
}

void TestKeyboard::mouseDrag(const juce::MouseEvent& e)
{
    // Off the keys, the key down stays down until the pointer reaches another.
    if (held_ >= 0)
        if (const int key = keyAt(e.position); key >= 0)
            hold(key);
}

void TestKeyboard::mouseUp(const juce::MouseEvent&)
{
    hold(-1);
}

// PatchView -------------------------------------------------------------------

PatchView::PatchView(AppModel& model, int track) : model_(model), track_(track)
{
    // The live views follow every voice the patch plays on, or one of them.
    voices_ = model_.tune().voices(model_.tune().tracks[static_cast<size_t>(track_)]);
    std::sort(voices_.begin(), voices_.end());
    // The levels the notes set, and the usual note.
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    std::vector<int> pitches;
    for (size_t op = 0; op < 4; ++op)
        levels_[op] = { 127, 0 };
    for (const TuneNote& n : tr.notes)
    {
        pitches.push_back(model_.tune().pitch(tr, n));
        for (size_t op = 0; op < 4; ++op)
        {
            const int level = model_.tune().noteLevel(tr, n, static_cast<int>(op));
            levels_[op] = { std::min(levels_[op].first, level), std::max(levels_[op].second, level) };
        }
    }
    if (!pitches.empty())
    {
        std::nth_element(pitches.begin(), pitches.begin() + static_cast<long>(pitches.size() / 2), pitches.end());
        pitch_ = pitches[pitches.size() / 2];
    }
    // The YM2151's key code: octaves from C#, four notes to a step of the top five bits.
    const int fromCs = pitch_ - 61, octave = std::clamp(4 + (fromCs >= 0 ? fromCs / 12 : (fromCs - 11) / 12), 0, 7);
    const int semitone = ((fromCs % 12) + 12) % 12;
    keycode_ = octave * 4 + (semitone + semitone / 3) / 4;
    for (int k = -1; k < static_cast<int>(voices_.size()); ++k)
    {
        auto* b = voiceButtons_.add(std::make_unique<juce::TextButton>(k < 0 ? juce::String("ALL") : "V" + juce::String(voices_[static_cast<size_t>(k)] + 1)));
        b->setColour(juce::TextButton::buttonOnColourId, theme::cyan);
        b->setClickingTogglesState(true);
        b->setRadioGroupId(1);
        b->setWantsKeyboardFocus(false);
        b->setTooltip(k < 0 ? "Watch all voices" : "Watch this voice");
        b->onClick = [this, k] { choose(k); };
        addAndMakeVisible(b);
        // Its solo, the same as the voice's S in the tracks.
        auto* s = soloButtons_.add(std::make_unique<juce::TextButton>("S"));
        s->setColour(juce::TextButton::buttonOnColourId, theme::cyan);
        s->setWantsKeyboardFocus(false);
        s->setTooltip(k < 0 ? "Solo all voices" : "Solo this voice");
        s->onClick = [this, k] { solo(k); };
        addAndMakeVisible(s);
        // Its output sides, which the chip sets per voice.
        auto* o = sideButtons_.add(std::make_unique<juce::TextButton>());
        o->setWantsKeyboardFocus(false);
        o->setTooltip(k < 0 ? "Output sides of all voices" : "Output sides of this voice");
        o->onClick = [this, k] { stepSides(k); };
        addAndMakeVisible(o);
    }
    voiceButtons_[0]->setToggleState(true, juce::dontSendNotification);
    choose(-1);

    revert_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    revert_.setWantsKeyboardFocus(false);
    revert_.setTooltip("Revert to the game's patch");
    revert_.onClick = [this] {
        model_.edit("Revert patch", [t = track_](Tune& tune) { tune.tracks[static_cast<size_t>(t)].patch.reset(); });
    };
    addAndMakeVisible(revert_);
    for (auto* b : { &save_, &load_ })
    {
        b->setColour(juce::TextButton::buttonOnColourId, theme::cyan);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    save_.setTooltip("Save this instrument, or export it to a .opm bank");
    save_.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem(1, "Save instrument...");
        menu.addItem(2, "Export to .opm...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&save_), [this](int chosen) {
            if (chosen == 1)
                saveInstrument();
            else if (chosen == 2)
                exportOpm();
        });
    };
    load_.setTooltip("Load a saved instrument, or import one from a .opm bank");
    load_.onClick = [this] { loadInstrument(); };
    // The LFO is edited in the chip window, as every voice shares it.
    chip_.setColour(juce::TextButton::buttonOnColourId, theme::magenta);
    chip_.setWantsKeyboardFocus(false);
    chip_.setTooltip("Edit the chip's LFO and noise, which every voice shares");
    chip_.onClick = [this] {
        if (onChip)
            onChip();
    };
    addAndMakeVisible(chip_);
    // The keyboard starts an octave under the instrument's usual note.
    keyboard_.setLowest(pitch_ - 12);
    keyboard_.onKey = [this](int key, bool on) {
        if (on && (model_.playing() || !model_.hasTune() || track_ >= static_cast<int>(model_.tune().tracks.size())))
            return false;
        if (on)
        {
            sent_ = patch();
            model_.previewPatch(sent_, track_);
        }
        model_.previewKey(key, on);
        return true;
    };
    addAndMakeVisible(keyboard_);
    // The box a value is typed in, in the look of the pitch callout's.
    entry_.setFont(theme::mono(13.0f, true));
    entry_.setJustification(juce::Justification::centred);
    entry_.setColour(juce::TextEditor::backgroundColourId, theme::background);
    entry_.setColour(juce::TextEditor::textColourId, theme::text);
    entry_.setColour(juce::TextEditor::outlineColourId, theme::gridBar);
    entry_.setColour(juce::TextEditor::focusedOutlineColourId, theme::cyan);
    entry_.onReturnKey = [this] { closeEntry(true); };
    entry_.onEscapeKey = [this] { closeEntry(false); };
    // The focus loss comes late, so a box opened again since then keeps its focus and stays open.
    entry_.onFocusLost = [this]
    {
        if (!entry_.hasKeyboardFocus(false))
            closeEntry(false);
    };
    addChildComponent(entry_);
    // Taller for an instrument on many voices, so the live strip holds a row for each.
    setSize(1080, 680 + kKeyboardHeight + 10 + liveExtra());
    startTimerHz(30);
}

PatchView::~PatchView()
{
    if (keyboard_.held() >= 0)
        model_.previewKey(keyboard_.held(), false);
    model_.setProbe(0, -1);
    // The audition mutes last only while the window shows the track.
    if (model_.audition(track_) != 0)
        model_.setAudition(-1, 0);
}

void PatchView::timerCallback()
{
    // The song took over from a key held: it no longer sounds, so it goes up.
    if (model_.playing())
        keyboard_.letGo();
    // A key held plays the patch as it is now, after an undo too.
    if (keyboard_.held() >= 0 && model_.hasTune() && track_ < static_cast<int>(model_.tune().tracks.size()))
        if (const FmPatch p = patch(); p != sent_)
        {
            sent_ = p;
            model_.previewPatch(sent_, track_);
        }
    repaint();
}

void PatchView::saveInstrument()
{
    // A name, the track's to start with; a name already saved is replaced.
    naming_ = std::make_unique<juce::AlertWindow>("Save instrument", "A name for this instrument. One saved before under the same name is replaced.",
                                                  juce::MessageBoxIconType::NoIcon, this);
    naming_->addTextEditor("name", juce::String(model_.tune().tracks[static_cast<size_t>(track_)].name));
    naming_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    naming_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    naming_->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
        if (result == 1)
        {
            // With the LFO the chip plays it with now.
            FmPatch p = patch();
            const ChipState chip = model_.chipNow();
            p.lfoFreq = chip.lfoRate, p.lfoWave = chip.lfoWave, p.pmd = chip.pmd, p.amd = chip.amd;
            juce::String error;
            if (!instruments::save(p, naming_->getTextEditorContents("name"), error))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Save instrument", error);
        }
        naming_.reset();
    }));
}

void PatchView::loadInstrument()
{
    // The saved instruments by name, or a word that there are none yet, and
    // a voice from a .opm bank.
    constexpr int kImport = 100000;
    const auto files = instruments::list();
    juce::PopupMenu menu;
    if (files.empty())
        menu.addItem(-1, "No instruments saved yet", false);
    for (size_t i = 0; i < files.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, files[i].getFileNameWithoutExtension());
    menu.addSeparator();
    menu.addItem(kImport, "Import from .opm bank...");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&load_), [this, files](int chosen) {
        if (chosen == kImport)
            return importOpm();
        if (chosen <= 0)
            return;
        bool lfo = false;
        const auto saved = instruments::load(files[static_cast<size_t>(chosen) - 1], &lfo);
        if (!saved)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Load instrument",
                                                   "Could not read " + files[static_cast<size_t>(chosen) - 1].getFullPathName());
            return;
        }
        applyInstrument(*saved, lfo, files[static_cast<size_t>(chosen) - 1].getFileNameWithoutExtension());
    });
}

void PatchView::applyInstrument(const FmPatch& from, bool lfo, const juce::String& name)
{
    if (!model_.hasTune() || track_ >= static_cast<int>(model_.tune().tracks.size()))
        return;
    // The instrument's own settings; this song's output sides stay, and
    // its LFO too unless the one that came with it is put on the chip, in
    // the same step.
    FmPatch p = original();
    p.alg = from.alg, p.fb = from.fb, p.pms = from.pms, p.ams = from.ams, p.op = from.op;
    const bool plain = p == original();
    auto load = [&model = model_, t = track_, p, plain, from](bool withLfo) {
        model.edit("Load instrument", [t, p, plain, from, withLfo](Tune& tune) {
            tune.tracks[static_cast<size_t>(t)].patch = plain ? std::nullopt : std::optional<FmPatch>(p);
            if (withLfo)
                tune.chip.lfoRate = from.lfoFreq, tune.chip.lfoWave = from.lfoWave, tune.chip.pmd = from.pmd, tune.chip.amd = from.amd;
        });
    };
    const ChipState now = model_.chipNow();
    if (!lfo || (from.lfoFreq == now.lfoRate && from.lfoWave == now.lfoWave && from.pmd == now.pmd && from.amd == now.amd))
        return load(false);
    static const char* const kWaves[4] = { "saw", "square", "triangle", "noise" };
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Apply its LFO to the chip?")
                                     .withMessage(name + " comes with the LFO at rate " + juce::String(from.lfoFreq) + ", " + kWaves[from.lfoWave & 3]
                                                  + ", vibrato depth " + juce::String(from.pmd) + " and tremolo depth " + juce::String(from.amd)
                                                  + ". Every voice shares the chip's LFO, so it changes every instrument in the song.")
                                     .withButton("Apply")
                                     .withButton("Don't Apply")
                                     .withButton("Cancel")
                                     .withAssociatedComponent(this),
                                 [load](int result) {
                                     if (result == 1)
                                         load(true);
                                     else if (result == 2)
                                         load(false);
                                 });
}

namespace
{
// The folder of the last .opm bank imported or exported, for the next one, while the app runs.
juce::File& bankFolder()
{
    static juce::File folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    return folder;
}
} // namespace

void PatchView::importOpm()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import from .opm bank", bankFolder(), "*.opm");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& c) {
        const juce::File file = c.getResult();
        if (file == juce::File())
            return;
        bankFolder() = file.getParentDirectory();
        // The bank's voices by number and name, to pick one from.
        const std::vector<OpmVoice> voices = parseOpm(file.loadFileAsString().toStdString());
        if (voices.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import from .opm bank",
                                                   "No voices found in " + file.getFullPathName());
            return;
        }
        auto nameOf = [](const OpmVoice& v) {
            return v.patch.name.empty() ? "Voice " + juce::String(v.number) : juce::String(v.patch.name);
        };
        juce::PopupMenu menu;
        for (size_t i = 0; i < voices.size(); ++i)
            menu.addItem(static_cast<int>(i) + 1, juce::String(voices[i].number).paddedLeft('0', 3) + "  " + nameOf(voices[i]));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&load_), [this, voices, nameOf](int chosen) {
            if (chosen > 0)
            {
                const OpmVoice& v = voices[static_cast<size_t>(chosen) - 1];
                applyInstrument(v.patch, v.lfo, nameOf(v));
            }
        });
    });
}

void PatchView::exportOpm()
{
    // With the LFO the chip plays it with now, under the track's name.
    FmPatch p = patch();
    const ChipState chip = model_.chipNow();
    p.lfoFreq = chip.lfoRate, p.lfoWave = chip.lfoWave, p.pmd = chip.pmd, p.amd = chip.amd;
    const juce::String name(model_.tune().tracks[static_cast<size_t>(track_)].name);
    p.name = name.toStdString();
    chooser_ = std::make_unique<juce::FileChooser>("Export to .opm", bankFolder().getChildFile(juce::File::createLegalFileName(name)).withFileExtension(".opm"),
                                                   "*.opm");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [p](const juce::FileChooser& c) {
                              if (c.getResult() == juce::File())
                                  return;
                              const juce::File to = c.getResult().withFileExtension(".opm");
                              bankFolder() = to.getParentDirectory();
                              if (!to.replaceWithText(writeOpm(p)))
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export to .opm",
                                                                         "Could not write " + to.getFullPathName());
                          });
}

void PatchView::solo(int index)
{
    // One voice's solo flips; all of them go on, or off when every one is.
    uint16_t bits = 0;
    for (size_t k = 0; k < voices_.size(); ++k)
        if (index < 0 || static_cast<int>(k) == index)
            bits = static_cast<uint16_t>(bits | (1 << voices_[k]));
    model_.edit("Solo voice", [t = track_, bits, index](Tune& tune) {
        auto& tr = tune.tracks[static_cast<size_t>(t)];
        if (index >= 0)
            tr.soloedVoices ^= bits;
        else
            tr.soloedVoices = (tr.soloedVoices & bits) == bits ? static_cast<uint16_t>(tr.soloedVoices & ~bits) : static_cast<uint16_t>(tr.soloedVoices | bits);
    });
}

uint8_t PatchView::sides(int index) const
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    const int voice = voices_[static_cast<size_t>(index)];
    const uint8_t set = tr.voiceSides[static_cast<size_t>(voice)];
    return set != 0 ? set : model_.tune().voiceSides(tr, voice);
}

void PatchView::stepSides(int index)
{
    // Both, left, right, and round; the game's own sides store no edit. All
    // the voices step on from the first one's.
    auto next = [](uint8_t s) { return static_cast<uint8_t>(s == 0xC0 ? 0x40 : s == 0x40 ? 0x80 : 0xC0); };
    const uint8_t to = next(sides(std::max(0, index)));
    std::array<uint8_t, 8> game {};
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    for (const int v : voices_)
        game[static_cast<size_t>(v)] = model_.tune().voiceSides(tr, v);
    std::vector<int> chosen;
    for (size_t k = 0; k < voices_.size(); ++k)
        if (index < 0 || static_cast<int>(k) == index)
            chosen.push_back(voices_[k]);
    model_.edit("Voice output", [t = track_, chosen, to, game](Tune& tune) {
        for (const int v : chosen)
            tune.tracks[static_cast<size_t>(t)].voiceSides[static_cast<size_t>(v)] = to == game[static_cast<size_t>(v)] ? 0 : to;
    });
}

void PatchView::choose(int index)
{
    voice_ = index;
    int mask = 0;
    for (size_t k = 0; k < voices_.size(); ++k)
        if (index < 0 || static_cast<int>(k) == index)
            mask |= 1 << voices_[k];
    model_.setProbe(mask, track_);
}

int PatchView::liveExtra() const
{
    // ALL and each voice, 28 pixels a row under the strip's title; 180 holds five.
    return std::max(0, (static_cast<int>(voices_.size()) + 1) * 28 + 40 - 180);
}

bool PatchView::live() const
{
    return model_.playing() || model_.previewing();
}

int PatchView::soundingPitch() const
{
    if (!model_.playing())
        return model_.previewing() ? model_.previewPitch() : -1;
    if (voices_.empty())
        return -1;
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    const double now = model_.displayPosition();
    int lowest = -1;
    for (const TuneNote& n : tr.notes)
        if (static_cast<double>(n.start) <= now && now < static_cast<double>(n.start + n.length)
            && (voice_ < 0 || model_.tune().originalChannel(tr, n) == voices_[static_cast<size_t>(voice_)]))
        {
            const int p = model_.tune().pitch(tr, n);
            lowest = lowest < 0 ? p : std::min(lowest, p);
        }
    return lowest;
}

void PatchView::resized()
{
    auto r = getLocalBounds().reduced(14);
    header_ = r.removeFromTop(124);
    r.removeFromTop(10);
    live_ = r.removeFromBottom(180 + liveExtra());
    r.removeFromBottom(10);
    keyboard_.setBounds(r.removeFromBottom(kKeyboardHeight));
    r.removeFromBottom(10);
    operators_ = r;

    // The header's controls after the name, the LFO's group at the right.
    controls_.clear();
    auto top = header_.reduced(12, 10);
    auto left = top.removeFromLeft(330);
    auto actions = left.removeFromTop(24); // save, load and revert, at the top right of the name's column
    revert_.setBounds(actions.removeFromRight(86));
    actions.removeFromRight(6);
    load_.setBounds(actions.removeFromRight(62));
    actions.removeFromRight(6);
    save_.setBounds(actions.removeFromRight(62));
    // The algorithm's diagram under the name and the voices, a click steps it.
    controls_.push_back({ Field::Alg, -1, Look::Diagram, left.withTrimmedTop(30).withWidth(270), "" });
    const int cell = 78;
    auto lane = top.withTrimmedTop(16); // level with the dials in the LFO's group
    controls_.push_back({ Field::Alg, -1, Look::Dial, lane.removeFromLeft(cell), "ALGORITHM" });
    controls_.push_back({ Field::Fb, -1, Look::Dial, lane.removeFromLeft(cell), "FEEDBACK" });
    // The LFO's group: this instrument's sensitivity to the chip's LFO,
    // under a title line, and a note on where the LFO itself is set.
    lfoBox_ = top.withTrimmedLeft(2 * cell + 14);
    chip_.setBounds(lfoBox_.reduced(6, 3).removeFromTop(18).removeFromRight(50));
    auto inside = lfoBox_.reduced(8, 4).withTrimmedTop(26);
    const int small = (inside.getWidth() - 4) / 6; // the group's width, six cells
    lfoSens_ = inside.removeFromLeft(2 * small);
    inside.removeFromLeft(12);
    lfoNote_ = inside;
    auto sens = lfoSens_;
    controls_.push_back({ Field::Pms, -1, Look::Dial, sens.removeFromLeft(small), "PM SENS" });
    controls_.push_back({ Field::Ams, -1, Look::Dial, sens.removeFromLeft(small), "AM SENS" });

    // Each operator: its title, two rows of three, the envelope's curve and
    // its five rates and level under it.
    const int w = (operators_.getWidth() - 3 * 10) / 4;
    for (int op = 0; op < 4; ++op)
    {
        panels_[static_cast<size_t>(op)] = juce::Rectangle<int>(operators_.getX() + op * (w + 10), operators_.getY(), w, operators_.getHeight());
        auto p = panels_[static_cast<size_t>(op)].reduced(10);
        // The switch, after the operator's name, and the audition mute after it.
        const auto onOff = p.removeFromTop(20).withTrimmedLeft(34).withWidth(44).withSizeKeepingCentre(44, 18);
        controls_.push_back({ Field::On, op, Look::Toggle, onOff, "" });
        mutes_[static_cast<size_t>(op)] = onOff.withX(onOff.getRight() + 6).withWidth(20);
        p.removeFromTop(8);
        auto row = p.removeFromTop(78);
        const int third = row.getWidth() / 3;
        controls_.push_back({ Field::Tl, op, Look::Dial, row.removeFromLeft(third), "LEVEL" });
        controls_.push_back({ Field::Mul, op, Look::Dial, row.removeFromLeft(third), "MULT" });
        controls_.push_back({ Field::Dt1, op, Look::Dial, row, "DETUNE" });
        p.removeFromTop(4);
        row = p.removeFromTop(78);
        controls_.push_back({ Field::Dt2, op, Look::Dial, row.removeFromLeft(third), "COARSE" });
        controls_.push_back({ Field::Ks, op, Look::Dial, row.removeFromLeft(third), "KEY SCL" });
        controls_.push_back({ Field::AmEn, op, Look::Lfo, row, "LFO AM" });
        p.removeFromTop(8);
        auto boxes = p.removeFromBottom(38);
        envelopes_[static_cast<size_t>(op)] = p.withTrimmedBottom(6);
        const int fifth = boxes.getWidth() / 5;
        controls_.push_back({ Field::Ar, op, Look::Box, boxes.removeFromLeft(fifth), "AR" });
        controls_.push_back({ Field::D1r, op, Look::Box, boxes.removeFromLeft(fifth), "D1" });
        controls_.push_back({ Field::D1l, op, Look::Box, boxes.removeFromLeft(fifth), "SL" });
        controls_.push_back({ Field::D2r, op, Look::Box, boxes.removeFromLeft(fifth), "D2" });
        controls_.push_back({ Field::Rr, op, Look::Box, boxes, "RR" });
    }

    // The voice buttons in a column at the live strip's left, each with its solo.
    auto buttons = live_.reduced(10).removeFromLeft(140).withTrimmedTop(20);
    for (int k = 0; k < voiceButtons_.size(); ++k)
    {
        auto row = buttons.removeFromTop(24);
        voiceButtons_[k]->setBounds(row.removeFromLeft(52));
        row.removeFromLeft(6);
        soloButtons_[k]->setBounds(row.removeFromLeft(26));
        row.removeFromLeft(6);
        sideButtons_[k]->setBounds(row.removeFromLeft(50));
        buttons.removeFromTop(4);
    }
}

void PatchView::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    if (!model_.hasTune() || track_ >= static_cast<int>(model_.tune().tracks.size()))
        return;
    revert_.setEnabled(model_.tune().tracks[static_cast<size_t>(track_)].patch.has_value());
    paintHeader(g, header_);

    float levels[4] {};
    int stages[4] {};
    model_.probeEnvelopes(levels, stages);
    for (int op = 0; op < 4; ++op)
        paintOperator(g, panels_[static_cast<size_t>(op)], op, live() ? levels[op] : 0.0f, live() ? stages[op] : 0);
    for (const auto& c : controls_)
        paintControl(g, c);
    // An operator switched off or muted to audition: its panel dimmed under
    // its title. The mute's M, lit while on, as a track's M is.
    const FmPatch shown = patch();
    const uint8_t muted = model_.audition(track_);
    for (size_t op = 0; op < 4; ++op)
    {
        const bool mute = (muted >> op) & 1;
        if (!shown.op[op].on || mute)
        {
            g.setColour(theme::panel.withAlpha(0.6f));
            g.fillRoundedRectangle(panels_[op].withTrimmedTop(34).reduced(2).toFloat(), 4.0f);
        }
        const auto m = mutes_[op].toFloat();
        g.setColour(mute ? theme::orange : theme::background.withAlpha(0.7f));
        g.fillRoundedRectangle(m, 4.0f);
        g.setColour(mute ? theme::orange : theme::gridBar.brighter(0.3f));
        g.drawRoundedRectangle(m.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(mute ? theme::background : theme::text.withAlpha(0.85f));
        g.setFont(theme::mono(11.0f, true));
        g.drawText("M", m, juce::Justification::centred, false);
    }

    drawPanel(g, live_);
    auto r = live_.reduced(10);
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText("VOICE", r.removeFromLeft(140).removeFromTop(16), juce::Justification::centredLeft, false);
    // The solos as the tracks have them now.
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    uint16_t all = 0;
    for (size_t k = 0; k < voices_.size(); ++k)
    {
        const auto bit = static_cast<uint16_t>(1 << voices_[k]);
        all = static_cast<uint16_t>(all | bit);
        soloButtons_[static_cast<int>(k) + 1]->setToggleState((tr.soloedVoices & bit) != 0, juce::dontSendNotification);
    }
    soloButtons_[0]->setToggleState(all != 0 && (tr.soloedVoices & all) == all, juce::dontSendNotification);
    // The output sides, yellow where edited; for all the voices, theirs if they agree.
    auto word = [](uint8_t s) { return juce::String(s == 0x40 ? "L" : s == 0x80 ? "R" : s == 0xC0 ? "BOTH" : "-"); };
    uint8_t common = voices_.empty() ? 0 : sides(0);
    bool edited = false;
    for (size_t k = 0; k < voices_.size(); ++k)
    {
        const uint8_t s = sides(static_cast<int>(k));
        const bool changed = tr.voiceSides[static_cast<size_t>(voices_[k])] != 0;
        edited = edited || changed;
        common = s == common ? common : 0;
        sideButtons_[static_cast<int>(k) + 1]->setButtonText(word(s));
        sideButtons_[static_cast<int>(k) + 1]->setColour(juce::TextButton::textColourOffId, changed ? theme::yellow : theme::text);
    }
    sideButtons_[0]->setButtonText(word(common));
    sideButtons_[0]->setColour(juce::TextButton::textColourOffId, edited ? theme::yellow : theme::text);
    r.removeFromLeft(10);
    std::vector<float> samples(kScope);
    model_.probeOutput(samples.data(), kScope);
    if (!live())
        std::fill(samples.begin(), samples.end(), 0.0f);
    paintScope(g, r.removeFromLeft(r.getWidth() / 2 - 5), samples);
    r.removeFromLeft(10);
    paintSpectrum(g, r, samples);
}

void PatchView::paintHeader(juce::Graphics& g, juce::Rectangle<int> r)
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    const FmPatch p = patch();
    drawPanel(g, r);
    r = r.reduced(12, 10);

    // The name and the voices; the algorithm's diagram under them is a control.
    auto left = r.removeFromLeft(330);
    g.setColour(theme::orange);
    g.setFont(theme::pixel(16.0f));
    g.drawText(juce::String(tr.name).toUpperCase() + (tr.patch ? "*" : ""), left.removeFromTop(24), juce::Justification::centredLeft, false);
    left.removeFromTop(6);
    juce::String voices;
    for (const int v : voices_)
        voices += (voices.isEmpty() ? "" : " ") + juce::String("V") + juce::String(v + 1);
    g.setColour(theme::dim);
    g.setFont(theme::mono(14.0f, true));
    g.drawText("PLAYS ON " + voices, left.removeFromTop(20), juce::Justification::centredLeft, false);

    // The LFO's group, in its colour while the chip's LFO reaches this
    // instrument, by vibrato or by tremolo on an operator with LFO AM on,
    // else dimmed. The LFO itself belongs to the chip window.
    const ChipState chip = model_.chipNow();
    // Lit as soon as an operator takes the tremolo or the patch the vibrato;
    // the caption says what is still missing to hear it.
    const bool routed = std::any_of(p.op.begin(), p.op.end(), [](const FmOperator& o) { return o.on && o.amEn; });
    const bool used = routed || p.pms > 0;
    juce::String caption = "NOT USED BY THIS INSTRUMENT";
    if (used)
        caption = routed && p.ams == 0 ? "LFO AM IS ON: TURN AM SENS UP TO HEAR IT"
                  : (routed && chip.amd == 0) || (!routed && chip.pmd == 0) ? "ITS DEPTH IS 0 NOW: SET IT IN CHIP"
                                                                     : "VIBRATO AND TREMOLO";
    // The frame, its title and what is missing, and a caption over this instrument's side.
    const auto frame = lfoBox_.toFloat().reduced(0.5f);
    g.setColour(used ? theme::magenta.withAlpha(0.6f) : theme::grid);
    g.drawRoundedRectangle(frame, 5.0f, 1.0f);
    auto title = lfoBox_.reduced(8, 4).removeFromTop(14).withTrimmedRight(chip_.getWidth() + 4);
    g.setColour(used ? theme::magenta : theme::faint);
    g.setFont(theme::pixel(9.0f));
    g.drawText("LFO", title.removeFromLeft(36), juce::Justification::centredLeft, false);
    g.setFont(theme::mono(11.0f, true));
    g.setColour(used ? theme::magenta.withAlpha(0.85f) : theme::faint);
    g.drawText(caption, title, juce::Justification::centredLeft, false);
    g.setColour(theme::grid);
    g.fillRect(lfoNote_.getX() - 7, lfoNote_.getY(), 1, lfoNote_.getHeight() - 4);
    g.setColour(theme::dim);
    g.setFont(theme::mono(11.0f));
    g.drawFittedText("How much of the chip's LFO this instrument takes: PM SENS for vibrato, AM SENS for tremolo on the operators with LFO AM on.\n"
                     "Its rate, wave and depths are the chip's, shared by every voice: set them in CHIP.",
                     lfoNote_, juce::Justification::centredLeft, 5, 1.0f);
}

void PatchView::paintDiagram(juce::Graphics& g, juce::Rectangle<int> r, const FmPatch& p)
{
    // Per algorithm, each operator's column and row (0 top, 1 middle, 2
    // bottom), and which operator modulates which. A chain runs left to
    // right; two modulators into one sit above and below the line.
    struct Place
    {
        int column, row;
    };
    struct Layout
    {
        std::array<Place, 4> at;
        std::vector<std::pair<int, int>> wires; // modulator, then the operator it modulates
    };
    static const std::array<Layout, 8> kLayouts { {
        { { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } } }, { { 0, 1 }, { 1, 2 }, { 2, 3 } } },
        { { { { 0, 0 }, { 0, 2 }, { 1, 1 }, { 2, 1 } } }, { { 0, 2 }, { 1, 2 }, { 2, 3 } } },
        { { { { 1, 0 }, { 0, 2 }, { 1, 2 }, { 2, 1 } } }, { { 1, 2 }, { 0, 3 }, { 2, 3 } } },
        { { { { 0, 0 }, { 1, 0 }, { 1, 2 }, { 2, 1 } } }, { { 0, 1 }, { 1, 3 }, { 2, 3 } } },
        { { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } } }, { { 0, 1 }, { 2, 3 } } },
        { { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } } }, { { 0, 1 }, { 0, 2 }, { 0, 3 } } },
        { { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } } }, { { 0, 1 } } },
        { { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } } }, {} },
    } };
    const Layout& layout = kLayouts[static_cast<size_t>(p.alg & 7)];
    const auto area = r.toFloat();
    constexpr float kWidth = 30.0f, kHeight = 15.0f, kPitch = 46.0f, kGap = kPitch - kWidth;
    const float left = area.getX() + 32.0f; // room for the feedback's value before M1
    const std::array<float, 3> rowTop { area.getY() + 6.0f, area.getY() + 17.5f, area.getY() + 29.0f };
    const float bus = area.getBottom() - 3.0f;
    auto box = [&](int op) {
        const Place& at = layout.at[static_cast<size_t>(op)];
        return juce::Rectangle<float>(left + kPitch * static_cast<float>(at.column), rowTop[static_cast<size_t>(at.row)], kWidth, kHeight);
    };
    // A small arrowhead at a wire's end, pointing right or down.
    auto head = [&g](juce::Point<float> tip, bool down) {
        juce::Path a;
        if (down)
            a.addTriangle(tip.x - 2.5f, tip.y - 4.0f, tip.x + 2.5f, tip.y - 4.0f, tip.x, tip.y);
        else
            a.addTriangle(tip.x - 4.0f, tip.y - 2.5f, tip.x - 4.0f, tip.y + 2.5f, tip.x, tip.y);
        g.fillPath(a);
    };
    const juce::PathStrokeType stroke(1.2f);
    g.setColour(theme::dim);

    // The modulation: straight along a row, round a corner between rows, and
    // over the top to an operator further along the same row.
    for (const auto& [from, to] : layout.wires)
    {
        const auto a = box(from), b = box(to);
        juce::Path wire;
        wire.startNewSubPath(a.getRight(), a.getCentreY());
        const float turn = a.getRight() + kGap * 0.5f;
        if (std::abs(a.getCentreY() - b.getCentreY()) > 0.5f)
        {
            const float x = b.getX() - kGap * 0.5f;
            wire.lineTo(x, a.getCentreY());
            wire.lineTo(x, b.getCentreY());
            wire.lineTo(b.getX(), b.getCentreY());
            g.strokePath(wire, stroke);
            head({ b.getX(), b.getCentreY() }, false);
        }
        else if (b.getX() - a.getRight() > kGap + 0.5f)
        {
            const float over = rowTop[1] - 7.0f;
            wire.lineTo(turn, a.getCentreY());
            wire.lineTo(turn, over);
            wire.lineTo(b.getCentreX(), over);
            wire.lineTo(b.getCentreX(), b.getY());
            g.strokePath(wire, stroke);
            head({ b.getCentreX(), b.getY() }, true);
        }
        else
        {
            wire.lineTo(b.getX(), b.getCentreY());
            g.strokePath(wire, stroke);
            head({ b.getX(), b.getCentreY() }, false);
        }
    }

    // The carriers down to a line that runs on to the output.
    const float out = left + 3.0f * kPitch + kWidth + 16.0f;
    float first = out;
    for (int op = 0; op < 4; ++op)
        if ((kCarrierMask[p.alg & 7] >> op) & 1)
        {
            const auto b = box(op);
            g.drawLine(b.getCentreX(), b.getBottom(), b.getCentreX(), bus, 1.2f);
            first = std::min(first, b.getCentreX());
        }
    g.drawLine(first, bus, out, bus, 1.2f);
    head({ out, bus }, false);
    g.setFont(theme::mono(10.0f, true));
    g.drawText("OUT", juce::Rectangle<float>(out + 4.0f, bus - 6.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);

    // M1's feedback: a loop from its output back over it to its input,
    // faint at 0, and its value before it.
    const auto m1 = box(0);
    juce::Path loop;
    loop.startNewSubPath(m1.getRight(), m1.getCentreY());
    loop.lineTo(m1.getRight() + 4.0f, m1.getCentreY());
    loop.lineTo(m1.getRight() + 4.0f, m1.getY() - 4.0f);
    loop.lineTo(m1.getX() - 4.0f, m1.getY() - 4.0f);
    loop.lineTo(m1.getX() - 4.0f, m1.getCentreY());
    loop.lineTo(m1.getX(), m1.getCentreY());
    g.setColour(p.fb > 0 ? theme::dim : theme::faint);
    g.strokePath(loop, stroke);
    g.drawText("FB" + juce::String(p.fb), juce::Rectangle<float>(m1.getX() - 32.0f, m1.getY(), 26.0f, kHeight), juce::Justification::centredRight, false);

    // The operators over the wires, as their panels have them: carriers and
    // modulators in their colours, faint while switched off or muted.
    const uint8_t muted = model_.audition(track_);
    for (int op = 0; op < 4; ++op)
    {
        const auto b = box(op);
        const bool carrier = (kCarrierMask[p.alg & 7] >> op) & 1;
        const bool heard = p.op[static_cast<size_t>(op)].on && ((muted >> op) & 1) == 0;
        const juce::Colour colour = (carrier ? theme::orange : theme::cyan).withAlpha(heard ? 1.0f : 0.35f);
        g.setColour(theme::background);
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(colour);
        g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.2f);
        g.drawText(kOperatorNames[op], b, juce::Justification::centred, false);
    }
}

void PatchView::paintOperator(juce::Graphics& g, juce::Rectangle<int> r, int op, float level, int stage)
{
    const FmPatch p = patch();
    const FmOperator& o = p.op[static_cast<size_t>(op)];
    const bool carrier = (kCarrierMask[p.alg] >> op) & 1;
    const juce::Colour colour = carrier ? theme::orange : theme::cyan;
    drawPanel(g, r);
    r = r.reduced(10);

    // The operator's name and its role in the algorithm.
    auto title = r.removeFromTop(20);
    g.setColour(colour);
    g.setFont(theme::pixel(12.0f));
    g.drawText(kOperatorNames[op], title, juce::Justification::centredLeft, false);
    g.setFont(theme::mono(13.0f, true));
    g.drawText(carrier ? "CARRIER" : "MODULATOR", title, juce::Justification::centredRight, false);
    // While the chip's noise is on, voice V8's C2 plays it instead of its
    // sine: marked, in the chip's colour, for an instrument on V8.
    if (op == 3)
    {
        noiseMark_ = {};
        if (model_.chipNow().noise && std::find(voices_.begin(), voices_.end(), 7) != voices_.end())
        {
            noiseMark_ = title.withTrimmedLeft(112).withWidth(60).withSizeKeepingCentre(60, 18);
            const auto pill = noiseMark_.toFloat();
            g.setColour(theme::magenta.withAlpha(0.25f));
            g.fillRoundedRectangle(pill, 9.0f);
            g.setColour(theme::magenta);
            g.drawRoundedRectangle(pill.reduced(0.5f), 9.0f, 1.0f);
            g.setFont(theme::mono(11.0f, true));
            g.drawText("NOISE", pill, juce::Justification::centred, false);
        }
    }

    // The envelope as the chip runs it for the instrument's usual note, on a real time axis:
    // the attack, the first decay to the sustain level, the second while the
    // key is held, and the release.
    const auto box = envelopes_[static_cast<size_t>(op)].toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(box, 4.0f);
    const auto area = box.reduced(8.0f, 8.0f).withTrimmedBottom(8.0f);
    auto at = [&](float x, float y) { return juce::Point<float>(area.getX() + x * area.getWidth(), area.getBottom() - y * area.getHeight()); };
    g.setFont(theme::mono(9.0f, true));
    for (const auto& [t, name] : { std::pair { 0.01, "10ms" }, std::pair { 0.1, "100ms" }, std::pair { 1.0, "1s" } })
    {
        const float x = area.getX() + xOf(t) * area.getWidth();
        g.setColour(theme::grid.withAlpha(0.5f));
        g.fillRect(x, area.getY(), 1.0f, area.getHeight());
        g.setColour(theme::faint);
        g.drawText(name, juce::Rectangle<float>(x - 20.0f, area.getBottom() + 1.0f, 40.0f, 9.0f), juce::Justification::centred, false);
    }
    const Envelope env = envelopeOf(o);
    g.setColour(theme::faint);
    g.setFont(theme::mono(9.0f, true));
    g.drawText("FOR " + theme::noteName(pitch_).removeCharacters("-"), area.withHeight(10.0f), juce::Justification::topRight, false);
    juce::Path curve;
    constexpr int kPoints = 160;
    for (int i = 0; i <= kPoints; ++i)
    {
        const float x = static_cast<float>(i) / kPoints;
        const auto pt = at(x, heightOf(env.at(secondsAt(x))));
        i == 0 ? curve.startNewSubPath(pt) : curve.lineTo(pt);
    }
    juce::Path fill(curve);
    fill.lineTo(at(1.0f, 0.0f));
    fill.lineTo(at(0.0f, 0.0f));
    fill.closeSubPath();
    g.setColour(colour.withAlpha(0.18f));
    g.fillPath(fill);
    g.setColour(colour);
    g.strokePath(curve, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The handles that drag the shape: filled while dragged or under the
    // pointer; handles at one place drawn once, with how many they are.
    const auto mouse = getMouseXYRelative().toFloat();
    std::array<juce::Point<float>, 4> points;
    for (int which = 0; which < 4; ++which)
        points[static_cast<size_t>(which)] = handle(op, which);
    for (int which = 0; which < 4; ++which)
    {
        const auto h = points[static_cast<size_t>(which)];
        int together = 0;
        bool drawn = false;
        for (int other = 0; other < 4; ++other)
            if (points[static_cast<size_t>(other)].getDistanceFrom(h) < 1.0f)
                ++together, drawn = drawn || other < which;
        if (drawn)
            continue;
        const auto hit = handleAt(mouse);
        bool active = false;
        for (int other = 0; other < 4; ++other)
            if (points[static_cast<size_t>(other)].getDistanceFrom(h) < 1.0f)
                active = active || (handleOp_ == op && handleWhich_ == other) || (isMouseOver() && hit == std::make_pair(op, other));
        const auto dot = juce::Rectangle<float>(active ? 8.0f : 6.0f, active ? 8.0f : 6.0f).withCentre(h);
        g.setColour(active ? colour : theme::background);
        g.fillEllipse(dot);
        g.setColour(colour);
        g.drawEllipse(dot, 1.5f);
        if (together > 1)
        {
            g.setFont(theme::mono(10.0f, true));
            g.drawText(juce::String(together), juce::Rectangle<float>(dot.getRight() + 1.0f, dot.getY() - 9.0f, 12.0f, 10.0f), juce::Justification::centredLeft, false);
        }
    }
    // Where the playing voice's operator is now, over the handles: the time
    // its level is reached in the stage it is in, so it rides the curve.
    if (stage > 0 && level > 0.0f && !env.silent)
    {
        const double steps = (1.0 - level) * 1023.0;
        double t = 0.0;
        if (stage == 1)
            t = env.attackK > 0 ? std::clamp(-std::log((steps + 1.0) / 1024.0) / env.attackK, 0.0, env.attack) : 0.0;
        else if (stage == 2)
            t = env.attack + (env.decaySpeed > 0 ? std::clamp(steps / env.decaySpeed, 0.0, env.decay) : 0.0);
        else if (stage == 3)
            t = env.attack + env.decay + (env.sustainSpeed > 0 ? std::clamp((steps - env.sustain) / env.sustainSpeed, 0.0, kHold) : 0.0);
        else
            t = env.keyOff() + (env.releaseSpeed > 0 ? std::max(0.0, (steps - env.at(env.keyOff())) / env.releaseSpeed) : 0.0);
        const float x = xOf(t);
        if (x <= 1.0f)
        {
            g.setColour(juce::Colours::white);
            g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(at(x, heightOf(env.at(t)))));
        }
    }
}

namespace
{
// The chip's envelope clock, from ymfm's YM2151: every 3 chip samples the
// counter ticks, and a rate moves the attenuation on the ticks its top bits
// pick, by the steps this table gives (ymfm's attenuation_increment table).
constexpr uint32_t kIncrements[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110, 0x10101010, 0x10111010, 0x11101110,
    0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010,
    0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010,
    0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242,
    0x44424442, 0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888
};
constexpr double kAxisStart = 0.01, kAxisSpan = 4.0; // the time axis: log-like from 0, its knee at 10 ms, to 4 s

// Steps a tick, on average, per effective rate.
const std::array<double, 64>& stepsPerTick()
{
    static const auto table = [] {
        std::array<double, 64> out {};
        for (uint32_t rate = 0; rate < 64; ++rate)
        {
            const uint32_t shift = rate >> 2;
            double sum = 0;
            constexpr uint32_t kTicks = 1 << 13;
            for (uint32_t n = 0; n < kTicks; ++n)
            {
                const uint32_t counter = (n << 2) << shift;
                if ((counter & 0x7FF) != 0)
                    continue;
                const uint32_t index = (counter >> (shift <= 11 ? 11 : shift)) & 7;
                sum += static_cast<double>((kIncrements[rate] >> (4 * index)) & 15);
            }
            out[rate] = sum / kTicks;
        }
        return out;
    }();
    return table;
}

int effectiveRate(int raw, int keyScale, int keycode)
{
    return raw == 0 ? 0 : std::min(raw + (keycode >> (keyScale ^ 3)), 63);
}
} // namespace

PatchView::Envelope PatchView::envelopeOf(const FmOperator& o) const
{
    const double ticks = model_.tune().source().fmRate / 3.0; // envelope ticks a second
    auto speed = [&](int raw) { return stepsPerTick()[static_cast<size_t>(effectiveRate(raw, o.ks, keycode_))] * ticks; };
    Envelope e;
    e.silent = o.ar == 0;
    if (!e.silent && effectiveRate(o.ar * 2, o.ks, keycode_) < 62)
    {
        e.attackK = speed(o.ar * 2) / 16.0;
        e.attack = e.attackK > 0 ? std::log(1024.0) / e.attackK : 0.0;
    }
    e.sustain = o.d1l == 15 ? 992.0 : o.d1l * 32.0;
    e.decaySpeed = speed(o.d1r * 2);
    e.decay = e.decaySpeed > 0 ? e.sustain / e.decaySpeed : kAxisSpan * 2;
    e.sustainSpeed = speed(o.d2r * 2);
    e.releaseSpeed = speed(o.rr * 4 + 2);
    return e;
}

double PatchView::Envelope::release() const
{
    return releaseSpeed > 0 ? std::max(0.0, 1023.0 - at(keyOff())) / releaseSpeed : kAxisSpan * 2;
}

double PatchView::Envelope::at(double t) const
{
    if (silent)
        return 1023.0;
    if (t < attack)
        return std::max(0.0, 1024.0 * std::exp(-attackK * t) - 1.0);
    if (t < attack + decay)
        return std::min(sustain, (t - attack) * decaySpeed);
    if (t < keyOff())
        return std::min(1023.0, sustain + (t - attack - decay) * sustainSpeed);
    const double held = std::min(1023.0, sustain + kHold * sustainSpeed);
    return std::min(1023.0, held + (t - keyOff()) * releaseSpeed);
}

float PatchView::xOf(double seconds)
{
    return static_cast<float>(std::log1p(std::max(0.0, seconds) / kAxisStart) / std::log1p(kAxisSpan / kAxisStart));
}

double PatchView::secondsAt(float x)
{
    return kAxisStart * std::expm1(static_cast<double>(x) * std::log1p(kAxisSpan / kAxisStart));
}

float PatchView::heightOf(double attenuation)
{
    return juce::jlimit(0.0f, 1.0f, 1.0f - static_cast<float>(attenuation * 96.0 / 1024.0) / kEnvelopeRange);
}

std::pair<int, int> PatchView::handleAt(juce::Point<float> p) const
{
    // The nearest within 6 pixels, so handles close together each stay
    // reachable; of handles at one place, the later stage, which pulls them apart.
    std::pair<int, int> best { -1, -1 };
    float nearest = 6.0f;
    for (int op = 0; op < 4; ++op)
        for (int which = 0; which < 4; ++which)
            if (const float d = handle(op, which).getDistanceFrom(p); d <= nearest)
                nearest = d, best = { op, which }; // at one place, the later stage
    return best;
}

juce::Point<float> PatchView::handle(int op, int which) const
{
    const auto area = envelopes_[static_cast<size_t>(op)].toFloat().reduced(8.0f, 8.0f).withTrimmedBottom(8.0f);
    const Envelope e = envelopeOf(patch().op[static_cast<size_t>(op)]);
    const double t = which == 0 ? e.attack : which == 1 ? e.attack + e.decay : which == 2 ? e.keyOff() : e.keyOff() + e.release();
    // A point past the axis's end shows at the end, on the curve, at the level it has there.
    const double end = secondsAt(1.0f), shown = std::min(t, end);
    const double steps = which == 0 ? 0.0 : which == 3 && t <= end ? 1023.0 : e.at(std::max(0.0, shown - 1e-6));
    return { area.getX() + xOf(shown) * area.getWidth(), area.getBottom() - heightOf(steps) * area.getHeight() };
}

// Editing ---------------------------------------------------------------------

const FmPatch& PatchView::original() const
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    return model_.tune().basePatch(tr);
}

FmPatch PatchView::patch() const
{
    const TuneTrack& tr = model_.tune().tracks[static_cast<size_t>(track_)];
    return tr.patch ? *tr.patch : original();
}

int PatchView::get(const FmPatch& p, Field f, int op)
{
    const FmOperator& o = p.op[static_cast<size_t>(std::max(0, op))];
    switch (f)
    {
        case Field::Alg: return p.alg;
        case Field::Fb: return p.fb;
        case Field::Pms: return p.pms;
        case Field::Ams: return p.ams;
        case Field::Tl: return o.tl;
        case Field::Mul: return o.mul;
        case Field::Dt1: return o.dt1;
        case Field::Dt2: return o.dt2;
        case Field::Ks: return o.ks;
        case Field::AmEn: return o.amEn ? 1 : 0;
        case Field::Ar: return o.ar;
        case Field::D1r: return o.d1r;
        case Field::D1l: return o.d1l;
        case Field::D2r: return o.d2r;
        case Field::Rr: return o.rr;
        case Field::On: return o.on ? 1 : 0;
    }
    return 0;
}

void PatchView::set(FmPatch& p, Field f, int op, int v)
{
    FmOperator& o = p.op[static_cast<size_t>(std::max(0, op))];
    const auto u = static_cast<uint8_t>(v);
    switch (f)
    {
        case Field::Alg: p.alg = u; break;
        case Field::Fb: p.fb = u; break;
        case Field::Pms: p.pms = u; break;
        case Field::Ams: p.ams = u; break;
        case Field::Tl: o.tl = u; break;
        case Field::Mul: o.mul = u; break;
        case Field::Dt1: o.dt1 = static_cast<int8_t>(v); break;
        case Field::Dt2: o.dt2 = u; break;
        case Field::Ks: o.ks = u; break;
        case Field::AmEn: o.amEn = v != 0; break;
        case Field::Ar: o.ar = u; break;
        case Field::D1r: o.d1r = u; break;
        case Field::D1l: o.d1l = u; break;
        case Field::D2r: o.d2r = u; break;
        case Field::Rr: o.rr = u; break;
        case Field::On: o.on = v != 0; break;
    }
}

juce::Range<int> PatchView::range(Field f)
{
    switch (f)
    {
        case Field::Alg:
        case Field::Fb:
        case Field::Pms: return { 0, 7 };
        case Field::Ams:
        case Field::Dt2:
        case Field::Ks: return { 0, 3 };
        case Field::Tl: return { 0, 127 };
        case Field::Mul:
        case Field::D1l:
        case Field::Rr: return { 0, 15 };
        case Field::Dt1: return { -3, 3 };
        case Field::AmEn:
        case Field::On: return { 0, 1 };
        case Field::Ar:
        case Field::D1r:
        case Field::D2r: return { 0, 31 };
    }
    return { 0, 0 };
}

juce::String PatchView::text(Field f, int v)
{
    // A level reads as its attenuation, 0.75 dB a step; a coarse as its
    // frequency ratio.
    if (f == Field::Tl)
        return v == 0 ? juce::String("0 dB") : juce::String(-0.75 * static_cast<double>(v), 1) + " dB";
    if (f == Field::Dt2)
    {
        static const char* const ratios[] = { "x1", "x1.41", "x1.57", "x1.73" };
        return ratios[static_cast<size_t>(std::clamp(v, 0, 3))];
    }
    if (f == Field::AmEn || f == Field::On)
        return v ? "ON" : "OFF";
    if (f == Field::Mul)
        return v == 0 ? juce::String("x0.5") : "x" + juce::String(v);
    if (f == Field::Dt1)
        return (v > 0 ? "+" : "") + juce::String(v);
    return juce::String(v);
}

void PatchView::paintControl(juce::Graphics& g, const Control& c)
{
    // In the operator's colour, or cyan along the top; a value changed from
    // the game's is yellow.
    const FmPatch p = patch();
    const int v = get(p, c.field, c.op), was = get(original(), c.field, c.op);
    const bool carrier = c.op >= 0 && ((kCarrierMask[p.alg] >> c.op) & 1);
    juce::Colour colour = c.op < 0 ? theme::cyan : carrier ? theme::orange : theme::cyan;
    // AM SENS in the LFO's colour while an operator takes the tremolo, the setting that lets it be heard.
    if (c.field == Field::Ams && std::any_of(p.op.begin(), p.op.end(), [](const FmOperator& o) { return o.on && o.amEn; }))
        colour = theme::magenta;
    const juce::Colour ink = v != was ? theme::yellow : theme::text;
    auto r = c.bounds;
    if (c.look == Look::Diagram)
    {
        paintDiagram(g, r, p);
        return;
    }
    if (c.look == Look::Lfo)
    {
        // A small pill with the LFO's wave, magenta while on, as the LFO is drawn.
        g.setColour(theme::dim);
        g.setFont(theme::mono(12.0f, true));
        g.drawText(c.name, r.removeFromTop(15), juce::Justification::centred, false);
        const auto pill = r.toFloat().withSizeKeepingCentre(56.0f, 20.0f);
        g.setColour(v ? theme::magenta : theme::background);
        g.fillRoundedRectangle(pill, 10.0f);
        g.setColour(v ? theme::magenta : v != was ? theme::yellow : theme::gridBar.brighter(0.3f));
        g.drawRoundedRectangle(pill.reduced(0.5f), 10.0f, 1.0f);
        // A wave, then the state.
        juce::Path wave;
        const float x0 = pill.getX() + 8.0f, cy = pill.getCentreY();
        for (int i = 0; i <= 12; ++i)
        {
            const float x = x0 + static_cast<float>(i);
            const float y = cy - 3.0f * std::sin(static_cast<float>(i) / 12.0f * juce::MathConstants<float>::twoPi);
            i == 0 ? wave.startNewSubPath(x, y) : wave.lineTo(x, y);
        }
        g.setColour(v ? theme::background : v != was ? theme::yellow : theme::dim);
        g.strokePath(wave, juce::PathStrokeType(1.4f));
        g.setFont(theme::mono(11.0f, true));
        g.drawText(text(c.field, v), pill.withTrimmedLeft(22.0f), juce::Justification::centred, false);
        return;
    }
    if (c.look == Look::Toggle)
    {
        // A pill, filled in the operator's colour while on.
        const auto pill = r.toFloat();
        g.setColour(v ? colour : theme::background);
        g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);
        g.setColour(v ? colour : theme::gridBar.brighter(0.3f));
        g.drawRoundedRectangle(pill.reduced(0.5f), pill.getHeight() * 0.5f, 1.0f);
        g.setColour(v ? theme::background : v != was ? theme::yellow : theme::dim);
        g.setFont(theme::mono(11.0f, true));
        g.drawText(text(c.field, v), pill, juce::Justification::centred, false);
        return;
    }
    if (c.look == Look::Dial)
    {
        const auto span = range(c.field);
        // A level turns up as it attenuates less.
        const float fraction = c.field == Field::Tl ? static_cast<float>(127 - v) / 127.0f
                                                    : static_cast<float>(v - span.getStart()) / static_cast<float>(span.getLength());
        // A level the notes vary: their range, shifted by the edit, as a
        // lighter arc and as loudest-quietest under the dial.
        juce::String shown = text(c.field, v);
        if (c.field == Field::Tl)
        {
            const auto [loud, quiet] = levels_[static_cast<size_t>(c.op)];
            const int shift = v - was;
            const int from = std::clamp(loud + shift, 0, 127), to = std::clamp(quiet + shift, 0, 127);
            if (to > from)
            {
                // In whole dB, so both ends fit under the dial.
                const auto decibels = [](int tl) { return juce::String(-juce::roundToInt(0.75 * static_cast<double>(tl))); };
                shown = decibels(from) + juce::String::fromUTF8("\xe2\x80\x93") + decibels(to) + " dB";
                auto ring = r.withTrimmedTop(15).withTrimmedBottom(16).toFloat();
                const float size = std::min(ring.getWidth(), ring.getHeight()) - 2.0f;
                const auto box = ring.withSizeKeepingCentre(size, size);
                const float radius = box.getWidth() * 0.5f - 2.0f;
                juce::Path range;
                range.addCentredArc(box.getCentreX(), box.getCentreY(), radius, radius, 0.0f, -kArc + 2.0f * kArc * static_cast<float>(127 - to) / 127.0f,
                                    -kArc + 2.0f * kArc * static_cast<float>(127 - from) / 127.0f, true);
                drawDial(g, r, c.name, fraction, shown, colour);
                g.setColour(colour.withAlpha(0.35f));
                g.strokePath(range, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
                if (v != was)
                {
                    g.setColour(ink);
                    g.setFont(theme::mono(13.0f, true));
                    g.drawText(shown, r.removeFromBottom(16), juce::Justification::centred, false);
                }
                return;
            }
        }
        drawDial(g, r, c.name, fraction, shown, colour);
        if (v != was)
        {
            g.setColour(ink);
            g.setFont(theme::mono(13.0f, true));
            g.drawText(shown, r.removeFromBottom(16), juce::Justification::centred, false);
        }
        return;
    }
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText(c.name, r.removeFromTop(15), juce::Justification::centred, false);
    const auto box = r.withSizeKeepingCentre(std::min(r.getWidth() - 4, c.look == Look::Word ? 64 : 40), c.look == Look::Word ? 24 : 20).toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(v != was ? theme::yellow.withAlpha(0.6f) : theme::gridBar);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
    g.setColour(c.field == Field::AmEn && v == 0 && v == was ? theme::faint : ink);
    g.setFont(theme::mono(13.0f, true));
    g.drawText(text(c.field, v), box, juce::Justification::centred, false);
}

juce::String PatchView::describe(Field f, int v)
{
    // The YM2151 datasheet's name and registers (a channel's eight, or an
    // operator's 32, one for each slot), and the value as the register holds
    // it: a negative detune is 4 and up.
    struct Names
    {
        const char* plain;
        const char* sheet;
        int address;
        int count; // the registers in the range: 1, 8 or 32
    };
    static const auto names = [] {
        std::array<Names, 16> n {};
        n[static_cast<size_t>(Field::Alg)] = { "Algorithm", "CON", 0x20, 8 };
        n[static_cast<size_t>(Field::Fb)] = { "Feedback", "FB", 0x20, 8 };
        n[static_cast<size_t>(Field::Pms)] = { "Vibrato sensitivity", "PMS", 0x38, 8 };
        n[static_cast<size_t>(Field::Ams)] = { "Tremolo sensitivity", "AMS", 0x38, 8 };
        n[static_cast<size_t>(Field::Tl)] = { "Level", "TL", 0x60, 32 };
        n[static_cast<size_t>(Field::Mul)] = { "Frequency multiple", "MUL", 0x40, 32 };
        n[static_cast<size_t>(Field::Dt1)] = { "Fine detune", "DT1", 0x40, 32 };
        n[static_cast<size_t>(Field::Dt2)] = { "Coarse detune", "DT2", 0xC0, 32 };
        n[static_cast<size_t>(Field::Ks)] = { "Key scaling", "KS", 0x80, 32 };
        n[static_cast<size_t>(Field::AmEn)] = { "LFO tremolo", "AMS-EN", 0xA0, 32 };
        n[static_cast<size_t>(Field::Ar)] = { "Attack rate", "AR", 0x80, 32 };
        n[static_cast<size_t>(Field::D1r)] = { "First decay rate", "D1R", 0xA0, 32 };
        n[static_cast<size_t>(Field::D1l)] = { "Sustain level", "D1L", 0xE0, 32 };
        n[static_cast<size_t>(Field::D2r)] = { "Second decay rate", "D2R", 0xC0, 32 };
        n[static_cast<size_t>(Field::Rr)] = { "Release rate", "RR", 0xE0, 32 };
        n[static_cast<size_t>(Field::On)] = { "Operator on", "KON slot bit", 0x08, 1 };
        return n;
    }();
    const Names& n = names[static_cast<size_t>(f)];
    const int raw = f == Field::Dt1 && v < 0 ? 4 - v : v;
    // What the value means, when the register's number does not say it.
    const juce::String plain = f == Field::Alg   ? juce::String(kAlgorithmDiagrams[std::clamp(v, 0, 7)])
                               : f == Field::D1l ? (v == 0 ? juce::String("0 dB") : juce::String(-3 * (v == 15 ? 31 : v)) + " dB")
                                                 : text(f, v);
    const auto hex = [](int address) { return "0x" + juce::String::toHexString(address).toUpperCase().paddedLeft('0', 2); };
    const juce::String registers = n.count > 1 ? "registers " + hex(n.address) + "-" + hex(n.address + n.count - 1) : "register " + hex(n.address);
    return juce::String(n.plain) + ": " + n.sheet + ", " + registers + ", raw " + juce::String(raw)
           + (plain != juce::String(raw) ? " (" + plain + ")" : juce::String());
}

juce::String PatchView::getTooltip()
{
    if (!model_.hasTune() || track_ >= static_cast<int>(model_.tune().tracks.size()))
        return {};
    const auto mouse = getMouseXYRelative();
    for (size_t op = 0; op < 4; ++op)
        if (mutes_[op].contains(mouse))
            return juce::String("Audition mute: silences ") + kOperatorNames[op]
                   + " in playback and in the previews, to hear the other operators alone. The instrument stays as it is: the mute is not saved "
                     "and not a step of undo, and it ends when this window closes or shows another track";
    if (const int i = controlAt(mouse); i >= 0)
    {
        const Control& c = controls_[static_cast<size_t>(i)];
        const juce::String what = describe(c.field, get(patch(), c.field, c.op));
        if (c.look == Look::Diagram)
            return what + ". Click, or turn the mouse wheel over it, to step it; right-click to type it";
        return what + ". Right-click to type a value";
    }
    if (!noiseMark_.isEmpty() && noiseMark_.contains(mouse))
        return "Noise: NE, register 0x0F, raw 1 (on); on voice V8 the C2 operator plays the chip's noise, at its frequency, NFRQ "
               + juce::String(model_.chipNow().noiseFreq);
    return {};
}

int PatchView::controlAt(juce::Point<int> p) const
{
    for (size_t i = 0; i < controls_.size(); ++i)
        if (controls_[i].bounds.contains(p))
            return static_cast<int>(i);
    return -1;
}

void PatchView::commit(const FmPatch& p, bool newStep)
{
    // The same as imported is no edit at all.
    const bool plain = p == original();
    model_.edit("Edit patch", [t = track_, p, plain](Tune& tune) {
        auto& tr = tune.tracks[static_cast<size_t>(t)];
        tr.patch = plain ? std::nullopt : std::optional<FmPatch>(p);
    }, newStep);
}

std::optional<int> PatchView::parse(Field f, const juce::String& typed)
{
    const juce::String t = typed.trim().toLowerCase();
    if (f == Field::On || f == Field::AmEn)
    {
        if (t == "on" || t == "1")
            return 1;
        if (t == "off" || t == "0")
            return 0;
        return std::nullopt;
    }
    // The number without its unit or the ratio's x.
    const juce::String number = t.replace("db", "").replace(",", ".").removeCharacters("x ");
    if (!number.containsAnyOf("0123456789") || !number.containsOnly("+-.0123456789"))
        return std::nullopt;
    const double n = number.getDoubleValue();
    int v = juce::roundToInt(n);
    if (f == Field::Tl)
        v = juce::roundToInt(std::abs(n) / 0.75); // dB of attenuation, with or without its minus
    else if (f == Field::Mul)
        v = n < 0.75 ? 0 : juce::roundToInt(n); // x0.5 is raw 0
    else if (f == Field::Dt2 && (t.containsChar('x') || t.containsChar('.')))
    {
        // A ratio: the nearest of the four.
        static constexpr double kRatios[4] = { 1.0, 1.41, 1.57, 1.73 };
        v = 0;
        for (int i = 1; i < 4; ++i)
            if (std::abs(n - kRatios[i]) < std::abs(n - kRatios[v]))
                v = i;
    }
    const auto span = range(f);
    return std::clamp(v, span.getStart(), span.getEnd());
}

void PatchView::reset(int control)
{
    const Control& c = controls_[static_cast<size_t>(control)];
    FmPatch p = patch();
    set(p, c.field, c.op, get(original(), c.field, c.op));
    if (p != patch())
        commit(p, true);
}

void PatchView::resetHandle(int op, int which)
{
    FmPatch p = patch();
    FmOperator& o = p.op[static_cast<size_t>(op)];
    const FmOperator& was = original().op[static_cast<size_t>(op)];
    if (which == 0)
        o.ar = was.ar;
    else if (which == 1)
        o.d1r = was.d1r, o.d1l = was.d1l;
    else if (which == 2)
        o.d2r = was.d2r;
    else
        o.rr = was.rr;
    if (p != patch())
        commit(p, true);
}

bool PatchView::step(int control, int by, bool newStep)
{
    // Up turns it up: a level attenuates less.
    const Control& c = controls_[static_cast<size_t>(control)];
    const auto span = range(c.field);
    FmPatch p = patch();
    const int v = get(p, c.field, c.op);
    const int to = juce::jlimit(span.getStart(), span.getEnd(), c.field == Field::Tl ? v - by : v + by);
    if (to == v)
        return false;
    set(p, c.field, c.op, to);
    commit(p, newStep);
    return true;
}

void PatchView::openEntry(int control)
{
    closeEntry(false);
    const Control& c = controls_[static_cast<size_t>(control)];
    // Over the value as the control shows it: under a dial, in a box or a
    // pill, or in the middle of the diagram.
    auto value = c.bounds;
    if (c.look == Look::Dial)
        value = value.removeFromBottom(18);
    else if (c.look == Look::Box || c.look == Look::Word || c.look == Look::Lfo)
        value = value.withTrimmedTop(15);
    entering_ = control;
    const int v = get(patch(), c.field, c.op);
    entry_.setColour(juce::TextEditor::outlineColourId, theme::gridBar);
    entry_.setColour(juce::TextEditor::focusedOutlineColourId, theme::cyan);
    entry_.setText(c.field == Field::Alg ? juce::String(v) : text(c.field, v), false);
    entry_.setBounds(value.withSizeKeepingCentre(std::min(c.bounds.getWidth() - 2, 76), 22));
    entry_.setVisible(true);
    entry_.grabKeyboardFocus();
    entry_.selectAll();
}

void PatchView::closeEntry(bool apply)
{
    if (entering_ < 0)
        return;
    const Control& c = controls_[static_cast<size_t>(entering_)];
    if (apply)
    {
        // Text that is no value keeps the box open, outlined in red.
        const auto v = parse(c.field, entry_.getText());
        if (!v)
        {
            entry_.setColour(juce::TextEditor::outlineColourId, theme::danger);
            entry_.setColour(juce::TextEditor::focusedOutlineColourId, theme::danger);
            entry_.repaint();
            return;
        }
        FmPatch p = patch();
        set(p, c.field, c.op, *v);
        if (p != patch())
            commit(p, true);
    }
    // Hidden after it is closed, as hiding it takes its focus and calls back here.
    entering_ = -1;
    entry_.setVisible(false);
}

void PatchView::mouseDown(const juce::MouseEvent& e)
{
    closeEntry(false);
    wheelControl_ = -1;
    // An operator's audition mute: on or off at once, with no undo step.
    for (int op = 0; op < 4; ++op)
        if (mutes_[static_cast<size_t>(op)].contains(e.getPosition()))
        {
            model_.setAudition(track_, static_cast<uint8_t>(model_.audition(track_) ^ (1 << op)));
            return;
        }
    // An envelope's handle first; Option-click puts back the game's values it sets.
    if (const auto [op, which] = handleAt(e.position); op >= 0)
    {
        if (e.mods.isPopupMenu())
            return;
        if (e.mods.isAltDown())
        {
            resetHandle(op, which);
            return;
        }
        handleOp_ = op;
        handleWhich_ = which;
        dragStep_ = true;
        return;
    }
    const int i = controlAt(e.getPosition());
    if (i < 0)
        return;
    // A right-click or Ctrl-click types a value; Option-click puts back the game's.
    if (e.mods.isPopupMenu())
    {
        openEntry(i);
        return;
    }
    if (e.mods.isAltDown())
    {
        reset(i);
        return;
    }
    const Control& c = controls_[static_cast<size_t>(i)];
    FmPatch p = patch();
    const int v = get(p, c.field, c.op);
    if (c.look == Look::Word || c.look == Look::Toggle || c.look == Look::Lfo || c.look == Look::Diagram)
    {
        // A click moves on to the next word; the second click of a
        // double-click leaves it for the double-click to put back. Each
        // click on the diagram steps it, however quick.
        if (e.getNumberOfClicks() > 1 && c.look != Look::Diagram)
            return;
        const auto span = range(c.field);
        set(p, c.field, c.op, v >= span.getEnd() ? span.getStart() : v + 1);
        commit(p, true);
        return;
    }
    dragging_ = i;
    dragFrom_ = v;
    dragY_ = e.position.y;
    dragSteps_ = 0.0f;
    dragStep_ = true;
}

void PatchView::mouseDrag(const juce::MouseEvent& e)
{
    if (handleOp_ >= 0)
    {
        // The handle's place, back to the chip's value whose time or level comes nearest.
        const auto area = envelopes_[static_cast<size_t>(handleOp_)].toFloat().reduced(8.0f, 8.0f).withTrimmedBottom(8.0f);
        const float x = juce::jlimit(0.0f, 1.0f, (e.position.x - area.getX()) / area.getWidth());
        const float y = juce::jlimit(0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight());
        const double t = secondsAt(x);
        FmPatch p = patch();
        FmOperator& o = p.op[static_cast<size_t>(handleOp_)];
        const Envelope now = envelopeOf(o);
        auto nearest = [](int from, int to, auto measure, double want) {
            int best = from;
            double gap = std::numeric_limits<double>::max();
            for (int v = from; v <= to; ++v)
                if (const double d = std::abs(measure(v) - want); d < gap)
                    gap = d, best = v;
            return static_cast<uint8_t>(best);
        };
        auto with = [&](auto change) {
            FmOperator trial = o;
            change(trial);
            return envelopeOf(trial);
        };
        if (handleWhich_ == 0)
            o.ar = nearest(1, 31, [&](int v) { return with([v](FmOperator& c) { c.ar = static_cast<uint8_t>(v); }).attack; }, t);
        else if (handleWhich_ == 1)
        {
            // Down 3 dB a sustain step; along, the first decay's time.
            o.d1l = static_cast<uint8_t>(juce::jlimit(0, 15, juce::roundToInt((1.0f - y) * kEnvelopeRange / 3.0f)));
            const uint8_t level = o.d1l;
            o.d1r = nearest(0, 31, [&](int v) { return with([level, v](FmOperator& c) { c.d1l = level, c.d1r = static_cast<uint8_t>(v); }).decay; },
                            t - now.attack);
        }
        else if (handleWhich_ == 2)
        {
            // The level at the key off: how far the second decay has fallen while held.
            const double want = (1.0 - static_cast<double>(y)) * kEnvelopeRange * 1024.0 / 96.0;
            o.d2r = nearest(0, 31, [&](int v) { return with([v](FmOperator& c) { c.d2r = static_cast<uint8_t>(v); }).at(now.keyOff() - 1e-6); }, want);
        }
        else
            o.rr = nearest(0, 15, [&](int v) { return with([v](FmOperator& c) { c.rr = static_cast<uint8_t>(v); }).release(); }, t - now.keyOff());
        if (p != patch())
        {
            commit(p, dragStep_);
            dragStep_ = false;
        }
        return;
    }
    if (dragging_ < 0)
        return;
    // Up turns it up; a short range takes more pixels a step, and Shift four
    // times as many, from where the pointer is when it is pressed.
    const Control& c = controls_[static_cast<size_t>(dragging_)];
    const auto span = range(c.field);
    const float pixels = juce::jlimit(2.0f, 14.0f, 140.0f / static_cast<float>(span.getLength()));
    dragSteps_ += (dragY_ - e.position.y) / pixels * (e.mods.isShiftDown() ? 0.25f : 1.0f);
    dragY_ = e.position.y;
    const int steps = static_cast<int>(dragSteps_);
    const int v = juce::jlimit(span.getStart(), span.getEnd(), c.field == Field::Tl ? dragFrom_ - steps : dragFrom_ + steps);
    FmPatch p = patch();
    if (v == get(p, c.field, c.op))
        return;
    set(p, c.field, c.op, v);
    commit(p, dragStep_);
    dragStep_ = false;
}

void PatchView::mouseUp(const juce::MouseEvent&)
{
    dragging_ = -1;
    handleOp_ = handleWhich_ = -1;
}

void PatchView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    // A handle: back to the game's values it sets.
    if (const auto [op, which] = handleAt(e.position); op >= 0)
    {
        resetHandle(op, which);
        return;
    }
    // Back to the game's value, but for the diagram, whose quick clicks step it.
    if (const int i = controlAt(e.getPosition()); i >= 0 && controls_[static_cast<size_t>(i)].look != Look::Diagram)
        reset(i);
}

void PatchView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int i = controlAt(e.getPosition());
    if (i < 0 || !model_.hasTune() || track_ >= static_cast<int>(model_.tune().tracks.size()))
    {
        Component::mouseWheelMove(e, w);
        return;
    }
    // A step a notch, or a step for each stretch of a trackpad's scroll. A
    // run of notches on one control is one undo step, until a pause.
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (i != wheelControl_ || now - wheelAt_ > 600.0)
        wheelSum_ = 0.0f, wheelStep_ = true;
    wheelControl_ = i;
    wheelAt_ = now;
    const float delta = (std::abs(w.deltaX) > std::abs(w.deltaY) ? -w.deltaX : w.deltaY) * (w.isReversed ? -1.0f : 1.0f);
    int steps = delta > 0.0f ? 1 : delta < 0.0f ? -1 : 0;
    if (w.isSmooth)
    {
        wheelSum_ += delta;
        steps = static_cast<int>(wheelSum_ / 0.12f);
        wheelSum_ -= static_cast<float>(steps) * 0.12f;
    }
    if (steps != 0 && step(i, steps, wheelStep_))
        wheelStep_ = false;
}

void PatchView::paintScope(juce::Graphics& g, juce::Rectangle<int> r, const std::vector<float>& s)
{
    // As synth plugins draw one: two cycles of the note playing, from the
    // same point of the wave each time so a held note stands still, scaled
    // by a level that follows the wave slowly.
    const int pitch = soundingPitch();
    const double rate = model_.tune().source().fmRate;
    auto head = r.removeFromTop(17);
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText("WAVE", head, juce::Justification::centredLeft, false);
    const auto box = r.toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(theme::grid);
    g.fillRect(box.getX() + 4.0f, box.getCentreY(), box.getWidth() - 8.0f, 1.0f);
    g.setColour(theme::grid.withAlpha(0.4f));
    for (const float f : { 0.25f, 0.75f })
        g.fillRect(box.getX() + 4.0f, box.getY() + box.getHeight() * f, box.getWidth() - 8.0f, 1.0f);
    if (pitch < 0)
    {
        scopePeak_ = std::max(0.05f, scopePeak_ * 0.9f);
        return;
    }

    // The period: the note's, or twice it when an operator runs at half its
    // frequency (a multiple of 0), which repeats every two of the note's.
    const double f0 = 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
    const FmPatch edited = patch();
    const bool half = std::any_of(edited.op.begin(), edited.op.end(), [](const FmOperator& o) { return o.mul == 0; });
    const double period = rate / f0 * (half ? 2.0 : 1.0);
    const int shown = juce::jlimit(64, kScope / 2, static_cast<int>(std::lround(2.0 * period)));
    g.setColour(theme::dim);
    g.drawText(theme::noteName(pitch).removeCharacters("-") + theme::dot() + juce::String(juce::roundToInt(f0)) + " Hz", head,
               juce::Justification::centredRight, false);

    // The start: the steepest rising zero crossing in the period before the
    // latest window that fits, the same point of every cycle.
    const int last = kScope - shown;
    int start = last, steepest = 0;
    float slope = 0.0f;
    for (int i = std::max(1, last - static_cast<int>(period)); i <= last; ++i)
        if (s[static_cast<size_t>(i - 1)] < 0.0f && s[static_cast<size_t>(i)] >= 0.0f && s[static_cast<size_t>(i)] - s[static_cast<size_t>(i - 1)] > slope)
            slope = s[static_cast<size_t>(i)] - s[static_cast<size_t>(i - 1)], steepest = i;
    if (steepest > 0)
        start = steepest;
    float peak = 0.0f;
    for (int i = 0; i < shown; ++i)
        peak = std::max(peak, std::abs(s[static_cast<size_t>(start + i)]));
    scopePeak_ = std::max({ 0.02f, peak, scopePeak_ * 0.97f });

    const auto area = box.reduced(4.0f, 6.0f);
    juce::Path wave, fill;
    for (int i = 0; i < shown; ++i)
    {
        const float x = area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(shown - 1);
        const float y = area.getCentreY() - s[static_cast<size_t>(start + i)] / scopePeak_ * area.getHeight() * 0.48f;
        if (i == 0)
            wave.startNewSubPath(x, y), fill.startNewSubPath(x, area.getCentreY()), fill.lineTo(x, y);
        else
            wave.lineTo(x, y), fill.lineTo(x, y);
    }
    fill.lineTo(area.getRight(), area.getCentreY());
    fill.closeSubPath();
    g.setColour(theme::cyan.withAlpha(0.10f));
    g.fillPath(fill);
    g.setColour(theme::cyan.withAlpha(0.22f));
    g.strokePath(wave, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(theme::cyan);
    g.strokePath(wave, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void PatchView::paintSpectrum(juce::Graphics& g, juce::Rectangle<int> r, const std::vector<float>& s)
{
    // As DAW analysers draw one: a 4096-point FFT through a Blackman-Harris
    // window, on a log frequency axis from 30Hz to 16kHz and a dB axis down
    // to kFloor, as a filled curve that rises at once and falls back at kFall.
    g.setColour(theme::dim);
    g.setFont(theme::mono(12.0f, true));
    g.drawText("SPECTRUM", r.removeFromTop(17), juce::Justification::centredLeft, false);
    const auto box = r.toFloat();
    g.setColour(theme::background);
    g.fillRoundedRectangle(box, 4.0f);
    const auto area = box.reduced(6.0f, 6.0f).withTrimmedBottom(12.0f);
    const double rate = model_.tune().source().fmRate;
    constexpr double kLow = 30.0, kHigh = 16000.0;
    auto xOf = [&](double f) { return area.getX() + area.getWidth() * static_cast<float>(std::log(f / kLow) / std::log(kHigh / kLow)); };
    auto yOf = [&](float db) { return area.getY() + area.getHeight() * juce::jlimit(0.0f, 1.0f, db / kFloor); };

    // Grid: octaves of 100Hz and 1kHz labelled, and every 12 dB.
    g.setFont(theme::mono(10.0f, true));
    for (const double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const float x = xOf(f);
        g.setColour(theme::grid.withAlpha(0.6f));
        g.fillRect(x, area.getY(), 1.0f, area.getHeight());
        g.setColour(theme::faint);
        g.drawText(f >= 1000.0 ? juce::String(f / 1000.0, 0) + "k" : juce::String(f, 0), juce::Rectangle<float>(x - 20.0f, area.getBottom() + 1.0f, 40.0f, 11.0f),
                   juce::Justification::centred, false);
    }
    for (float db = -12.0f; db > kFloor; db -= 12.0f)
    {
        g.setColour(theme::grid.withAlpha(0.4f));
        g.fillRect(area.getX(), yOf(db), area.getWidth(), 1.0f);
    }

    // The FFT, each bin as a level against full scale.
    static juce::dsp::FFT fft(kFftOrder);
    static const std::vector<float> window = [] {
        std::vector<float> w(kScope);
        for (int i = 0; i < kScope; ++i)
        {
            const double t = juce::MathConstants<double>::twoPi * i / (kScope - 1);
            w[static_cast<size_t>(i)] = static_cast<float>(0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t));
        }
        return w;
    }();
    // Without the signal's offset from zero, whose leakage would rise over the low end.
    float mean = 0.0f;
    for (const float v : s)
        mean += v;
    mean /= static_cast<float>(kScope);
    std::vector<float> data(2 * kScope, 0.0f);
    float windowSum = 0.0f;
    for (int i = 0; i < kScope; ++i)
        data[static_cast<size_t>(i)] = (s[static_cast<size_t>(i)] - mean) * window[static_cast<size_t>(i)], windowSum += window[static_cast<size_t>(i)];
    fft.performFrequencyOnlyForwardTransform(data.data());
    auto binDb = [&](int k) { return juce::Decibels::gainToDecibels(data[static_cast<size_t>(k)] * 2.0f / windowSum, kFloor); };

    // Per pixel column, the loudest bin in its band, or between two bins
    // where a column is narrower than one; then the fall back.
    const int columns = std::max(1, static_cast<int>(area.getWidth()));
    spectrum_.resize(static_cast<size_t>(columns), kFloor);
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float fall = lastPaint_ > 0 ? static_cast<float>((now - lastPaint_) / 1000.0) * kFall : 0.0f;
    lastPaint_ = now;
    const double binWidth = rate / kScope;
    for (int c = 0; c < columns; ++c)
    {
        const double f0 = kLow * std::pow(kHigh / kLow, static_cast<double>(c) / columns);
        const double f1 = kLow * std::pow(kHigh / kLow, static_cast<double>(c + 1) / columns);
        const double k0 = f0 / binWidth, k1 = f1 / binWidth;
        float db = kFloor;
        if (k1 - k0 >= 1.0)
            for (int k = static_cast<int>(k0); k <= static_cast<int>(k1) && k < kScope / 2; ++k)
                db = std::max(db, binDb(k));
        else
        {
            const double k = (k0 + k1) * 0.5;
            const int lo = static_cast<int>(k);
            const float t = static_cast<float>(k - lo);
            db = binDb(lo) * (1.0f - t) + binDb(std::min(lo + 1, kScope / 2 - 1)) * t;
        }
        if (!live())
            db = kFloor;
        auto& shown = spectrum_[static_cast<size_t>(c)];
        shown = std::max(db, shown - fall);
    }

    juce::Path curve, fill;
    for (int c = 0; c < columns; ++c)
    {
        const float x = area.getX() + static_cast<float>(c);
        const float y = yOf(spectrum_[static_cast<size_t>(c)]);
        if (c == 0)
            curve.startNewSubPath(x, y), fill.startNewSubPath(x, area.getBottom()), fill.lineTo(x, y);
        else
            curve.lineTo(x, y), fill.lineTo(x, y);
    }
    fill.lineTo(area.getRight(), area.getBottom());
    fill.closeSubPath();
    g.setGradientFill(juce::ColourGradient(theme::cyan.withAlpha(0.35f), 0.0f, area.getY(), theme::cyan.withAlpha(0.03f), 0.0f, area.getBottom(), false));
    g.fillPath(fill);
    g.setColour(theme::cyan);
    g.strokePath(curve, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

PatchWindow::PatchWindow(AppModel& model, int track, juce::LookAndFeel* look, juce::ApplicationCommandManager& commands)
    : DocumentWindow(juce::String(model.tune().tracks[static_cast<size_t>(track)].name) + juce::String::fromUTF8(" \xe2\x80\x94 patch"),
                     theme::panel, DocumentWindow::closeButton),
      commands_(commands)
{
    setLookAndFeel(look);
    setUsingNativeTitleBar(true);
    auto* view = new PatchView(model, track);
    view->onChip = [this] {
        if (onChip)
            onChip();
    };
    setContentOwned(view, true);
    setResizable(false, false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

PatchWindow::~PatchWindow()
{
    setLookAndFeel(nullptr);
}

bool PatchWindow::keyPressed(const juce::KeyPress& key)
{
    // The app's command keys, which the main window handles; the instrument
    // name's box is a window of its own, so typing in it is not taken.
    return forwardCommandKey(commands_, key);
}

void PatchWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace opm86
