#include "PluginEditor.h"

namespace opm86
{

namespace
{

// Out Run sunset palette.
const juce::Colour kBackground { 0xff11132e };
const juce::Colour kPanel { 0xff1c1f4a };
const juce::Colour kPanelEdge { 0xff2e3270 };
const juce::Colour kOrange { 0xffff8a3d };
const juce::Colour kPink { 0xffff4f9a };
const juce::Colour kCyan { 0xff3de0ff };
const juce::Colour kText { 0xffe8e6ff };
const juce::Colour kDim { 0xff8d8fc4 };

constexpr int kWidth = 1200;
constexpr int kHeight = 846;
constexpr int kHeaderHeight = 80;
constexpr int kMargin = 12;

// Labels for operator knobs, in kOperatorFields order (AM is a toggle).
constexpr const char* kFieldLabels[] = { "Level", "Attack", "Decay 1", "Decay 2", "Release",
                                         "Sustain", "Mult", "Detune", "Coarse", "Key Scl" };

// An imported name without its song prefix, for use inside the song's submenu.
juce::String shortName(const std::string& name, const std::string& group)
{
    const juce::String full(name);
    return group.empty() ? full : full.fromFirstOccurrenceOf(juce::String(group) + " ", false, false);
}

void drawPanel(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(kPanel);
    g.fillRoundedRectangle(area.toFloat(), 6.0f);
    g.setColour(kPanelEdge);
    g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 6.0f, 1.0f);
}

} // namespace

OpmLookAndFeel::OpmLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, kBackground);
    setColour(juce::Slider::rotarySliderFillColourId, kOrange);
    setColour(juce::Slider::rotarySliderOutlineColourId, kPanelEdge);
    setColour(juce::Slider::thumbColourId, kPink);
    setColour(juce::Slider::textBoxTextColourId, kText);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textColourId, kDim);
    setColour(juce::ComboBox::backgroundColourId, kBackground);
    setColour(juce::ComboBox::outlineColourId, kPanelEdge);
    setColour(juce::ComboBox::textColourId, kText);
    setColour(juce::ComboBox::arrowColourId, kOrange);
    setColour(juce::PopupMenu::backgroundColourId, kPanel);
    setColour(juce::PopupMenu::textColourId, kText);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, kPink.withAlpha(0.6f));
    setColour(juce::TextButton::buttonColourId, kPanelEdge);
    setColour(juce::TextButton::textColourOffId, kText);
    setColour(juce::ToggleButton::textColourId, kDim);
    setColour(juce::ToggleButton::tickColourId, kOrange);
    setColour(juce::ToggleButton::tickDisabledColourId, kDim);
}

Knob::Knob(juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& label,
           bool compact)
    : attachment_(state, id, slider_)
{
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    if (compact)
    {
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider_.setPopupDisplayEnabled(true, true, nullptr);
    }
    else
        slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 14);
    addAndMakeVisible(slider_);

    label_.setText(label, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centred);
    label_.setFont(juce::FontOptions(11.0f));
    label_.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(label_);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(14));
    slider_.setBounds(area);
}

ChoiceBox::ChoiceBox(juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& label)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(state.getParameter(id)))
        box_.addItemList(choice->choices, 1);
    addAndMakeVisible(box_);
    attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(state, id, box_);

    label_.setText(label, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centred);
    label_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(label_);
}

void ChoiceBox::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(14));
    box_.setBounds(area.withSizeKeepingCentre(area.getWidth() - 4, 24));
}

OperatorPanel::OperatorPanel(juce::AudioProcessorValueTreeState& state, int op)
    : op_(op), amAttachment_(state, operatorParamId(op, "am"), am_)
{
    for (int f = 0; f < kOperatorFieldCount - 1; ++f)
        addAndMakeVisible(knobs_.add(new Knob(state, operatorParamId(op, kOperatorFields[f]), kFieldLabels[f])));
    addAndMakeVisible(am_);
}

void OperatorPanel::setCarrier(bool carrier)
{
    if (carrier == carrier_)
        return;
    carrier_ = carrier;
    repaint();
}

void OperatorPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds());
    auto header = getLocalBounds().removeFromTop(26).reduced(10, 0);
    g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    g.setColour(carrier_ ? kOrange : kCyan);
    g.drawText(kOperatorNames[op_], header, juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(12.0f));
    g.setColour(kDim);
    g.drawText(carrier_ ? "carrier" : "modulator", header, juce::Justification::centredRight);
}

void OperatorPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    area.removeFromTop(20);
    am_.setBounds(area.removeFromBottom(22).reduced(4, 0));
    const int rowHeight = area.getHeight() / 2;
    for (int row = 0; row < 2; ++row)
    {
        auto line = area.removeFromTop(rowHeight);
        const int width = line.getWidth() / 5;
        for (int i = 0; i < 5; ++i)
            knobs_[row * 5 + i]->setBounds(line.removeFromLeft(width));
    }
}

PadView::PadView(OpmProcessor& processor, int pad)
    : processor_(processor),
      pad_(pad),
      tune_(processor.state(), padParamId(pad, "tune"), "Tune", true),
      level_(processor.state(), padParamId(pad, "level"), "Level", true),
      pan_(processor.state(), padParamId(pad, "pan"), "Pan", true),
      choke_(processor.state(), padParamId(pad, "choke"), "Choke", true)
{
    menu_.onClick = [this] { showMenu(); };
    menu_.setTooltip("Choose the sample on this pad");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &menu_, &tune_, &level_, &pan_, &choke_ })
        addAndMakeVisible(c);
    refresh();
}

void PadView::refresh()
{
    name_ = juce::String(processor_.pads()[static_cast<size_t>(pad_)].name);
    if (name_.isEmpty())
        name_ = "(empty)";
    repaint();
}

void PadView::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds());
    auto header = getLocalBounds().removeFromTop(38).reduced(8, 4);
    g.setColour(kPink);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(juce::String(kFirstPadNote + pad_), header.removeFromTop(16), juce::Justification::centredLeft);
    g.setColour(kText);
    g.setFont(juce::FontOptions(12.0f));
    g.drawFittedText(name_, header, juce::Justification::centredLeft, 1, 0.7f);
}

void PadView::resized()
{
    auto area = getLocalBounds().reduced(4);
    menu_.setBounds(area.getRight() - 58, area.getY() + 2, 56, 18);
    area.removeFromTop(36);
    auto top = area.removeFromTop(area.getHeight() / 2);
    tune_.setBounds(top.removeFromLeft(top.getWidth() / 2));
    level_.setBounds(top);
    pan_.setBounds(area.removeFromLeft(area.getWidth() / 2));
    choke_.setBounds(area);
}

void PadView::mouseDown(const juce::MouseEvent& e)
{
    // Clicking the name area plays the pad.
    if (e.y < 38)
        processor_.auditionPad(pad_);
}

void PadView::showMenu()
{
    enum { kLoadFile = 1, kDefault = 2, kLibrary = 100 };
    juce::PopupMenu menu;
    menu.addItem(kLoadFile, "Load audio file...");
    menu.addItem(kDefault, "Default kit sound");

    // Imported samples, one submenu per song.
    const auto& library = processor_.sampleLibrary();
    if (!library.empty())
    {
        juce::PopupMenu imported;
        std::string song;
        juce::PopupMenu samples;
        for (size_t i = 0; i <= library.size(); ++i)
        {
            if (i == library.size() || library[i].group != song)
            {
                if (samples.getNumItems() > 0)
                    imported.addSubMenu(juce::String(song), samples);
                samples = {};
                if (i == library.size())
                    break;
                song = library[i].group;
            }
            const PadSample& sample = library[i];
            samples.addItem(kLibrary + static_cast<int>(i), shortName(sample.name, sample.group) + "  ("
                                                              + juce::String(sample.data.size()) + " bytes)");
        }
        menu.addSubMenu("Imported samples", imported);
    }

    juce::Component::SafePointer<PadView> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menu_), [safe](int result) {
        if (safe == nullptr || result == 0)
            return;
        PadView& self = *safe;
        if (result == kLoadFile)
        {
            self.chooser_ = std::make_unique<juce::FileChooser>("Load a sample", juce::File(),
                                                                "*.wav;*.aif;*.aiff;*.flac");
            self.chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                       [safe](const juce::FileChooser& chooser) {
                                           const juce::File file = chooser.getResult();
                                           if (safe == nullptr || file == juce::File())
                                               return;
                                           if (!safe->processor_.loadPadFile(safe->pad_, file))
                                               juce::AlertWindow::showMessageBoxAsync(
                                                   juce::MessageBoxIconType::WarningIcon, "OPM86",
                                                   "Cannot read " + file.getFileName() + " as audio.");
                                       });
        }
        else if (result == kDefault)
            self.processor_.setPadSample(self.pad_, makeDefaultKit()[static_cast<size_t>(self.pad_)]);
        else
        {
            const auto index = static_cast<size_t>(result - kLibrary);
            const auto& lib = self.processor_.sampleLibrary();
            if (index < lib.size())
                self.processor_.setPadSample(self.pad_, lib[index]);
        }
    });
}

SongPanel::SongPanel(OpmProcessor& processor) : processor_(processor)
{
    for (int stem = 0; stem <= kStemDrums; ++stem)
        stem_.addItem(stemName(stem), stem + 1);
    stem_.onChange = [this] {
        const int stem = stem_.getSelectedId() - 1;
        if (stem >= 0 && stem != processor_.stem())
            processor_.setStem(stem);
    };
    song_.onChange = [this] {
        const juce::String name = song_.getText();
        if (name.isNotEmpty() && name != processor_.songName())
            processor_.setSong(name);
    };
    songLabel_.setText("Song", juce::dontSendNotification);
    stemLabel_.setText("Plays", juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, kCyan);
    status_.setFont(juce::FontOptions(15.0f));
    help_.setJustificationType(juce::Justification::topLeft);
    help_.setFont(juce::FontOptions(14.0f));
    help_.setText("Song mode plays an imported VGM song exactly as the game does, driven by a song track: "
                  "one MIDI note per beat of the song. Make one with\n\n"
                  "    opm86 songtrack SONG.vgz SONG-track.mid\n\n"
                  "and drag it into GarageBand: it makes one track for each instrument of the song. Put OPM86 "
                  "on each track; each copy sets itself to its song and instrument when the song track first "
                  "plays. Move, cut and repeat the regions on all tracks together to rearrange the song.",
                  juce::dontSendNotification);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &song_, &stem_, &songLabel_, &stemLabel_, &status_, &help_ })
        addAndMakeVisible(c);
    refresh();
}

void SongPanel::refresh()
{
    song_.clear(juce::dontSendNotification);
    const auto names = processor_.librarySongs();
    for (int i = 0; i < names.size(); ++i)
        song_.addItem(names[i], i + 1);
    song_.setSelectedItemIndex(names.indexOf(processor_.songName()), juce::dontSendNotification);
    stem_.setSelectedId(processor_.stem() + 1, juce::dontSendNotification);
    status_.setText(processor_.songStatus(), juce::dontSendNotification);
}

void SongPanel::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds());
}

void SongPanel::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto row = area.removeFromTop(28);
    songLabel_.setBounds(row.removeFromLeft(50));
    song_.setBounds(row.removeFromLeft(360));
    row.removeFromLeft(24);
    stemLabel_.setBounds(row.removeFromLeft(50));
    stem_.setBounds(row.removeFromLeft(160));
    area.removeFromTop(12);
    status_.setBounds(area.removeFromTop(24));
    area.removeFromTop(12);
    help_.setBounds(area.removeFromTop(200).withWidth(std::min(area.getWidth(), 760)));
}

OpmEditor::OpmEditor(OpmProcessor& p)
    : AudioProcessorEditor(p),
      processor_(p),
      fmLevel_(p.state(), "fm_level", "FM"),
      pcmLevel_(p.state(), "pcm_level", "PCM"),
      velSens_(p.state(), "vel_sens", "Velocity"),
      bendRange_(p.state(), "bend_range", "Bend"),
      drumChannel_(p.state(), "drum_channel", "Drum Ch"),
      alg_(p.state(), "alg", "Algorithm"),
      fb_(p.state(), "fb", "Feedback"),
      lfoFreq_(p.state(), "lfo_freq", "LFO Rate"),
      pmd_(p.state(), "pmd", "PM Depth"),
      amd_(p.state(), "amd", "AM Depth"),
      pms_(p.state(), "pms", "PM Sens"),
      ams_(p.state(), "ams", "AM Sens"),
      lfoWave_(p.state(), "lfo_wave", "LFO Wave"),
      pan_(p.state(), "pan", "Output"),
      keyboard_(p.keyboard(), juce::MidiKeyboardComponent::horizontalKeyboard),
      songPanel_(p)
{
    setLookAndFeel(&lookAndFeel_);

    presets_.onChange = [this] {
        const int index = presets_.getSelectedId() - 1;
        if (index >= 0 && index != processor_.getCurrentProgram())
            processor_.setCurrentProgram(index);
    };
    import_.onClick = [this] { showImportMenu(); };
    mode_.addItem("Synth", 1);
    mode_.addItem("Song", 2);
    mode_.onChange = [this] {
        processor_.setMode(mode_.getSelectedId() == 2 ? OpmProcessor::Mode::Song : OpmProcessor::Mode::Synth);
    };
    addAndMakeVisible(mode_);
    addChildComponent(songPanel_);
    kit_.onClick = [this] { processor_.restoreDefaultKit(); };
    status_.setFont(juce::FontOptions(12.0f));
    status_.setJustificationType(juce::Justification::centredRight);

    algDiagram_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    algDiagram_.setColour(juce::Label::textColourId, kCyan);
    algDiagram_.setJustificationType(juce::Justification::centred);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &presets_, &import_, &kit_, &status_, &fmLevel_, &pcmLevel_, &velSens_, &bendRange_, &drumChannel_,
             &alg_, &fb_, &lfoFreq_, &pmd_, &amd_, &pms_, &ams_, &lfoWave_, &pan_, &algDiagram_, &keyboard_ })
        addAndMakeVisible(c);

    for (int op = 0; op < 4; ++op)
        addAndMakeVisible(operators_.add(new OperatorPanel(p.state(), op)));
    for (int pad = 0; pad < kPads; ++pad)
        addAndMakeVisible(pads_.add(new PadView(p, pad)));

    keyboard_.setAvailableRange(24, 108);
    keyboard_.setLowestVisibleKey(24);
    keyboard_.setOctaveForMiddleC(4); // MIDI 60 is C4, so the keys run C1 to C8
    keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, kPink);
    keyboard_.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, kOrange.withAlpha(0.4f));

    refreshPresets();
    showMode();
    timerCallback();
    processor_.addChangeListener(this);
    startTimerHz(15);
    setSize(kWidth, kHeight);
}

OpmEditor::~OpmEditor()
{
    processor_.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void OpmEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);

    // Header: title over sunset stripes.
    auto header = getLocalBounds().removeFromTop(kHeaderHeight - 14).withTrimmedTop(8);
    for (int i = 0; i < 4; ++i)
    {
        g.setColour(kOrange.interpolatedWith(kPink, static_cast<float>(i) / 3.0f).withAlpha(0.85f));
        g.fillRect(0, kHeaderHeight - 12 + i * 3, getWidth(), 2);
    }
    g.setColour(kText);
    g.setFont(juce::FontOptions(30.0f, juce::Font::bold | juce::Font::italic));
    g.drawText("OPM86", header.withTrimmedLeft(kMargin + 2).withHeight(50), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("YM2151 + SegaPCM", header.withTrimmedLeft(kMargin + 122).withHeight(50),
               juce::Justification::centredLeft);

    if (processor_.mode() == OpmProcessor::Mode::Song)
        return;
    drawPanel(g, voiceArea_);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("DRUMS  -  click a pad name to play it", drumArea_.withHeight(16), juce::Justification::centredLeft);
}

void OpmEditor::resized()
{
    auto area = getLocalBounds().reduced(kMargin, 0);

    auto header = area.removeFromTop(kHeaderHeight).withTrimmedBottom(16).withTrimmedTop(4);
    auto knobs = header.removeFromRight(5 * 58);
    for (Knob* k : { &fmLevel_, &pcmLevel_, &velSens_, &bendRange_, &drumChannel_ })
        k->setBounds(knobs.removeFromLeft(58));
    header.removeFromLeft(240);
    auto buttons = header.withSizeKeepingCentre(header.getWidth(), 26);
    mode_.setBounds(buttons.removeFromLeft(90));
    buttons.removeFromLeft(8);
    // Song mode has no patch list, so the import menu takes its place.
    if (processor_.mode() != OpmProcessor::Mode::Song)
        presets_.setBounds(buttons.removeFromLeft(200));
    buttons.removeFromLeft(8);
    import_.setBounds(buttons.removeFromLeft(110));
    buttons.removeFromLeft(8);
    kit_.setBounds(buttons.removeFromLeft(90));
    status_.setBounds(buttons.reduced(4, 0));

    area.removeFromTop(6);
    songPanel_.setBounds(area.withTrimmedBottom(kMargin));
    voiceArea_ = area.removeFromTop(92);
    auto voice = voiceArea_.reduced(8, 6);
    algDiagram_.setBounds(voice.removeFromRight(300));
    const int w = voice.getWidth() / 9;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &alg_, &fb_, &lfoFreq_, &lfoWave_, &pmd_,
                                                                         &amd_, &pms_, &ams_, &pan_ })
        c->setBounds(voice.removeFromLeft(w));

    area.removeFromTop(10);
    auto ops = area.removeFromTop(214);
    const int opWidth = (ops.getWidth() - 3 * 10) / 4;
    for (auto* op : operators_)
    {
        op->setBounds(ops.removeFromLeft(opWidth));
        ops.removeFromLeft(10);
    }

    area.removeFromTop(10);
    keyboard_.setBounds(area.removeFromBottom(64).withTrimmedBottom(kMargin - 4));
    keyboard_.setKeyWidth(static_cast<float>(keyboard_.getWidth()) / 50.0f); // 50 white keys from C1 to C8
    area.removeFromBottom(8);
    drumArea_ = area;
    area.removeFromTop(20);
    const int padWidth = (area.getWidth() - 7 * 8) / 8;
    const int rowHeight = (area.getHeight() - 8) / 2;
    for (int row = 0; row < 2; ++row)
    {
        auto line = area.removeFromTop(rowHeight);
        area.removeFromTop(8);
        for (int i = 0; i < 8; ++i)
        {
            pads_[row * 8 + i]->setBounds(line.removeFromLeft(padWidth));
            line.removeFromLeft(8);
        }
    }
}

void OpmEditor::showMode()
{
    const bool song = processor_.mode() == OpmProcessor::Mode::Song;
    mode_.setSelectedId(song ? 2 : 1, juce::dontSendNotification);
    for (juce::Component* c : getChildren())
        if (c != &mode_ && c != &songPanel_ && c != &import_ && c != &fmLevel_ && c != &pcmLevel_ && c != &status_)
            c->setVisible(!song);
    songPanel_.setVisible(song);
    songPanel_.refresh();
    resized();
    repaint();
}

void OpmEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    showMode();
    refreshPresets();
    for (auto* pad : pads_)
        pad->refresh();
}

void OpmEditor::timerCallback()
{
    const int alg = static_cast<int>(processor_.state().getRawParameterValue("alg")->load());
    if (alg == shownAlg_ || alg < 0 || alg > 7)
        return;
    shownAlg_ = alg;
    algDiagram_.setText(kAlgorithmDiagrams[alg], juce::dontSendNotification);
    for (int op = 0; op < 4; ++op)
        operators_[op]->setCarrier(((kCarrierMask[alg] >> op) & 1) != 0);
}

void OpmEditor::refreshPresets()
{
    // Built-in patches first, then one submenu per imported song.
    presets_.clear(juce::dontSendNotification);
    const auto& patches = processor_.patches();
    std::vector<std::pair<std::string, juce::PopupMenu>> songs;
    for (size_t i = 0; i < patches.size(); ++i)
    {
        const FmPatch& p = patches[i];
        const int id = static_cast<int>(i) + 1;
        if (p.group.empty())
        {
            presets_.addItem(juce::String(p.name), id);
            continue;
        }
        if (songs.empty() || songs.back().first != p.group)
            songs.emplace_back(p.group, juce::PopupMenu());
        // Full names, because the closed box shows only the selected item.
        songs.back().second.addItem(id, juce::String(p.name));
    }
    if (!songs.empty())
    {
        presets_.addSectionHeading("Imported");
        for (auto& [song, menu] : songs)
            presets_.getRootMenu()->addSubMenu(juce::String(song), menu);
    }
    presets_.setSelectedId(processor_.getCurrentProgram() + 1, juce::dontSendNotification);
}

void OpmEditor::showImportMenu()
{
    enum { kImport = 1, kReveal = 2, kKit = 100 };
    juce::PopupMenu menu;
    menu.addItem(kImport, "Import VGM file...");

    // Each imported song's samples, loaded onto the pads in the order a
    // converted MIDI file expects.
    std::vector<std::string> songs;
    for (const PadSample& sample : processor_.sampleLibrary())
        if (songs.empty() || songs.back() != sample.group)
            songs.push_back(sample.group);
    juce::PopupMenu kits;
    for (size_t i = 0; i < songs.size(); ++i)
        kits.addItem(kKit + static_cast<int>(i), juce::String(songs[i]));
    menu.addSubMenu("Load drum kit from", kits, !songs.empty());
    menu.addItem(kReveal, "Show imported files in Finder");

    juce::Component::SafePointer<OpmEditor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&import_), [safe, songs](int result) {
        if (safe == nullptr)
            return;
        if (result >= kKit && result - kKit < static_cast<int>(songs.size()))
            safe->processor_.loadSongKit(songs[static_cast<size_t>(result - kKit)]);
        else if (result == kImport)
            safe->importVgm();
        else if (result == kReveal)
        {
            const juce::File directory = OpmProcessor::libraryDirectory();
            directory.createDirectory();
            directory.revealToUser();
        }
    });
}

void OpmEditor::importVgm()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import patches and samples from a VGM file", juce::File(),
                                                   "*.vgm;*.vgz");
    juce::Component::SafePointer<OpmEditor> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe](const juce::FileChooser& chooser) {
                              const juce::File file = chooser.getResult();
                              if (safe == nullptr || file == juce::File())
                                  return;
                              const juce::String error = safe->processor_.importVgm(file);
                              safe->status_.setText(error.isEmpty() ? "Imported " + file.getFileName() : error,
                                                    juce::dontSendNotification);
                              safe->status_.setColour(juce::Label::textColourId, error.isEmpty() ? kCyan : kPink);
                          });
}

} // namespace opm86
