#include "PluginProcessor.h"

#include "AudioFile.h"
#include "PluginEditor.h"
#include "VgmImport.h"

namespace opm86
{

namespace
{

// Pad samples are saved in the plugin state as base64 bytes, so a project
// reopens with its drums even when their source files are gone.
juce::ValueTree sampleToTree(const PadSample& s)
{
    juce::ValueTree t("sample");
    t.setProperty("name", juce::String(s.name), nullptr);
    t.setProperty("group", juce::String(s.group), nullptr);
    t.setProperty("step", s.step, nullptr);
    t.setProperty("loop", s.loop, nullptr);
    t.setProperty("loopStart", static_cast<int>(s.loopStart), nullptr);
    t.setProperty("data", juce::MemoryBlock(s.data.data(), s.data.size()).toBase64Encoding(), nullptr);
    return t;
}

PadSample sampleFromTree(const juce::ValueTree& t)
{
    PadSample s;
    s.name = t["name"].toString().toStdString();
    s.group = t["group"].toString().toStdString();
    s.step = static_cast<uint8_t>(juce::jlimit(1, 255, static_cast<int>(t["step"])));
    s.loop = t["loop"];
    s.loopStart = static_cast<uint32_t>(juce::jmax(0, static_cast<int>(t["loopStart"])));
    juce::MemoryBlock data;
    if (data.fromBase64Encoding(t["data"].toString()))
    {
        const auto* bytes = static_cast<const uint8_t*>(data.getData());
        s.data.assign(bytes, bytes + std::min(data.getSize(), kMaxSampleBytes));
    }
    return s;
}

} // namespace

OpmProcessor::OpmProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      state_(*this, nullptr, "OPM86", createParameterLayout()),
      params_(state_),
      pads_(makeDefaultKit())
{
    loadLibrary();
    rebuildBank();
    startTimerHz(10);
}

juce::File OpmProcessor::libraryDirectory()
{
    const juce::String custom = juce::SystemStats::getEnvironmentVariable("OPM86_LIBRARY_DIR", {});
    if (custom.isNotEmpty())
        return juce::File(custom);
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Application Support/OPM86/VGM");
}

void OpmProcessor::loadLibrary()
{
    patches_ = factoryPatches();
    sampleLibrary_.clear();

    auto files = libraryDirectory().findChildFiles(juce::File::findFiles, false, "*.vgm;*.vgz");
    files.sort();
    for (const juce::File& file : files)
    {
        juce::MemoryBlock bytes;
        if (!file.loadFileAsData(bytes))
            continue;
        const auto* begin = static_cast<const uint8_t*>(bytes.getData());
        const VgmImport result = opm86::importVgm({ begin, begin + bytes.getSize() },
                                                  file.getFileNameWithoutExtension().toStdString());
        patches_.insert(patches_.end(), result.patches.begin(), result.patches.end());
        sampleLibrary_.insert(sampleLibrary_.end(), result.samples.begin(), result.samples.end());
    }
}

void OpmProcessor::prepareToPlay(double sampleRate, int)
{
    engine_.prepare(sampleRate);
    songTrack_.player().prepare(sampleRate);
    patchValid_ = false;
    keyboard_.reset();
}

bool OpmProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void OpmProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    {
        const juce::SpinLock::ScopedTryLockType lock(bankLock_);
        if (lock.isLocked() && pendingBank_ != nullptr)
        {
            // The bank being replaced is parked, so it is freed on the message thread.
            retiredBank_ = engine_.bank();
            engine_.setBank(std::move(pendingBank_));
            pendingBank_ = nullptr;
        }
    }

    // Setup controllers at the start of a track: a song track's stem (CC 86)
    // switches to song mode; a converted track's song (CC 87-90) and patch
    // (CC 102) or drum kit (CC 103) are loaded on the message thread.
    for (const auto meta : midi)
    {
        if (meta.numBytes != 3 || (meta.data[0] & 0xF0) != 0xB0)
            continue;
        const int number = meta.data[1], value = meta.data[2] & 0x7F;
        if (number == kStemController)
            mode_.store(static_cast<int>(Mode::Song));
        else if (number >= kSongIdController && number < kSongIdController + 4)
        {
            const int shift = 21 - 7 * (number - kSongIdController);
            setupId_ = (setupId_ & ~(0x7Fu << shift)) | static_cast<uint32_t>(value) << shift;
            setupIdParts_ |= 1 << (number - kSongIdController);
        }
        else if ((number == kPatchSelectController || number == kKitSelectController) && setupIdParts_ == 0x0F)
        {
            setupSong_.store(setupId_);
            if (number == kPatchSelectController)
                setupPatch_.store(value);
            else
                setupKit_.store(true);
        }
    }
    if (mode_.load() == static_cast<int>(Mode::Song))
    {
        processSong(buffer, midi);
        return;
    }

    const FmPatch patch = params_.patch();
    if (!patchValid_ || !(patch == lastPatch_))
    {
        engine_.fm().setPatch(patch);
        lastPatch_ = patch;
        patchValid_ = true;
    }
    engine_.fm().setVelocitySensitivity(params_.velocitySensitivity());
    engine_.fm().setBendRange(params_.bendRange());
    engine_.setDrumChannel(params_.drumChannel());
    engine_.setLevels(params_.fmLevel(), params_.pcmLevel());
    for (int pad = 0; pad < kPads; ++pad)
        engine_.setPadParams(pad, params_.pad(pad));

    keyboard_.processNextMidiBuffer(midi, 0, numSamples, true);

    // Pads clicked in the editor play as drum-channel notes.
    if (const uint32_t pads = auditions_.exchange(0); pads != 0)
    {
        const int setting = params_.drumChannel();
        const int channel = setting >= 1 && setting <= 16 ? setting : 10;
        if (setting == 0)
            engine_.setDrumChannel(channel);
        for (int pad = 0; pad < kPads; ++pad)
            if (pads & (1u << pad))
                midi.addEvent(juce::MidiMessage::noteOn(channel, kFirstPadNote + pad, static_cast<juce::uint8>(110)), 0);
    }

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);
    // Each message lands on its exact chip sample within the block.
    const double blockStart = engine_.hostTime();
    for (const auto meta : midi)
        engine_.scheduleMidi(meta.data, meta.numBytes, blockStart + juce::jlimit(0, numSamples, meta.samplePosition));
    engine_.render(left, right, numSamples);
}

void OpmProcessor::processSong(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    {
        const juce::SpinLock::ScopedTryLockType lock(songLock_);
        if (lock.isLocked() && pendingSong_ != nullptr)
        {
            // The song being replaced is parked, to be freed on the message thread.
            retiredSong_ = std::move(playingSong_);
            playingSong_ = std::move(pendingSong_);
            songTrack_.player().setSong(playingSong_->song, playingSong_->snapshots);
        }
    }
    if (const int s = stemRequest_.exchange(-1); s >= 0)
        songTrack_.setStem(s);
    songTrack_.player().setLevels(params_.fmLevel(), params_.pcmLevel());

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);
    int pos = 0;
    auto it = midi.begin();
    while (it != midi.end())
    {
        // All messages at one sample, then the check for a stop.
        const int at = juce::jlimit(0, numSamples, (*it).samplePosition);
        if (at > pos)
            songTrack_.player().render(left + pos, right + pos, at - pos);
        pos = juce::jmax(pos, at);
        for (; it != midi.end() && juce::jlimit(0, numSamples, (*it).samplePosition) == at; ++it)
            songTrack_.handleMidi((*it).data, (*it).numBytes);
        songTrack_.endOfEvents();
    }
    if (numSamples > pos)
        songTrack_.player().render(left + pos, right + pos, numSamples - pos);

    stem_.store(songTrack_.stem());
    midiSong_.store(songTrack_.requestedSong());
}

void OpmProcessor::setMode(Mode mode)
{
    mode_.store(static_cast<int>(mode));
    sendChangeMessage();
}

juce::StringArray OpmProcessor::librarySongs() const
{
    juce::StringArray names;
    for (const auto& file : libraryDirectory().findChildFiles(juce::File::findFiles, false, "*.vgm;*.vgz"))
        names.add(file.getFileNameWithoutExtension());
    names.sort(true);
    return names;
}

void OpmProcessor::setSong(const juce::String& name)
{
    songName_ = name;
    loaded_ = nullptr;
    songWaiting_ = name.isNotEmpty();
    update();
    sendChangeMessage();
}

juce::String OpmProcessor::songStatus() const
{
    if (songName_.isEmpty())
        return "No song. Import a VGM file, or play a song track made by opm86 songtrack.";
    if (loaded_ == nullptr)
        return "Loading " + songName_ + "...";
    const VgmSong& song = *loaded_->song;
    return juce::String(song.tempo(), 2) + " BPM, " + juce::String(song.beatCount) + " beats, "
           + juce::String(static_cast<int>(song.length / kVgmRate / 60)) + ":"
           + juce::String(static_cast<int>(song.length / kVgmRate) % 60).paddedLeft('0', 2);
}

void OpmProcessor::update()
{
    // A converted track named its song and instrument or drum kit.
    const int patch = setupPatch_.exchange(0);
    const bool kit = setupKit_.exchange(false);
    if (patch > 0 || kit)
    {
        const uint32_t id = setupSong_.load();
        for (const auto& name : librarySongs())
        {
            if (songId(name.toStdString()) != id)
                continue;
            if (kit)
            {
                loadSongKit(name.toStdString());
                if (auto* channel = state_.getParameter("drum_channel"))
                    channel->setValueNotifyingHost(channel->convertTo0to1(static_cast<float>(Engine::kDrumsOnAllChannels)));
            }
            if (patch > 0)
            {
                // The song's patches keep the importer's order, so patch n is the nth.
                int seen = 0;
                for (size_t i = 0; i < patches_.size(); ++i)
                    if (juce::String(patches_[i].group) == name && ++seen == patch)
                    {
                        setCurrentProgram(static_cast<int>(i));
                        break;
                    }
            }
            break;
        }
    }

    // A song track named a song: find it in the library by its id.
    const uint32_t asked = midiSong_.load();
    if (asked != 0 && (songName_.isEmpty() || songId(songName_.toStdString()) != asked))
        for (const auto& name : librarySongs())
            if (songId(name.toStdString()) == asked)
            {
                setSong(name);
                break;
            }

    if (songWaiting_)
    {
        juce::File file;
        for (const auto& f : libraryDirectory().findChildFiles(juce::File::findFiles, false, "*.vgm;*.vgz"))
            if (f.getFileNameWithoutExtension() == songName_)
                file = f;
        if (file == juce::File())
            songWaiting_ = false;
        else if (auto song = SongCache::get(file))
        {
            loaded_ = song;
            songWaiting_ = false;
            std::shared_ptr<const LoadedSong> unused, retired;
            {
                const juce::SpinLock::ScopedLockType lock(songLock_);
                unused = std::exchange(pendingSong_, song);
                retired = std::exchange(retiredSong_, nullptr);
            }
            sendChangeMessage();
        }
    }

    if (mode_.load() != seenMode_)
    {
        seenMode_ = mode_.load();
        sendChangeMessage();
    }
}

juce::AudioProcessorEditor* OpmProcessor::createEditor()
{
    return new OpmEditor(*this);
}

int OpmProcessor::getNumPrograms()
{
    return static_cast<int>(patches_.size());
}

void OpmProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= getNumPrograms())
        return;
    currentProgram_ = index;
    applyPatchToParameters(state_, patches_[static_cast<size_t>(index)]);
    sendChangeMessage();
}

const juce::String OpmProcessor::getProgramName(int index)
{
    if (index < 0 || index >= getNumPrograms())
        return {};
    return juce::String(patches_[static_cast<size_t>(index)].name);
}

void OpmProcessor::rebuildBank()
{
    auto bank = buildBank(pads_);
    std::shared_ptr<const PcmBank> unused, retired;
    {
        const juce::SpinLock::ScopedLockType lock(bankLock_);
        unused = std::exchange(pendingBank_, std::move(bank));
        retired = std::exchange(retiredBank_, nullptr);
    }
    // Banks the audio thread let go of are freed here, outside the lock.
}

juce::String OpmProcessor::importVgm(const juce::File& file)
{
    // Parse first, so a file without sounds never reaches the library.
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes))
        return "Cannot read " + file.getFullPathName();
    const std::string group = file.getFileNameWithoutExtension().toStdString();
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    const VgmImport result = opm86::importVgm({ begin, begin + bytes.getSize() }, group);
    if (result.patches.empty() && result.samples.empty())
        return juce::String(result.error);

    // A copy in the library makes the song's sounds available to every instance.
    const juce::File directory = libraryDirectory();
    const juce::File copy = directory.getChildFile(file.getFileName());
    if (!directory.createDirectory() || (copy != file && !file.copyFileTo(copy)))
        return "Cannot copy " + file.getFileName() + " to " + directory.getFullPathName();
    loadLibrary();

    loadSongKit(group);
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
    const auto first = std::find_if(patches_.begin(), patches_.end(),
                                    [&](const FmPatch& p) { return p.group == group; });
    setCurrentProgram(first == patches_.end() ? 0 : static_cast<int>(first - patches_.begin()));
    return juce::String(result.error);
}

void OpmProcessor::loadSongKit(const std::string& song)
{
    // The song's samples, most used first, from pad 36 up; other pads keep their sound.
    size_t pad = 0;
    for (const PadSample& sample : sampleLibrary_)
        if (sample.group == song && pad < pads_.size())
            pads_[pad++] = sample;
    if (pad == 0)
        return;
    rebuildBank();
    sendChangeMessage();
}

bool OpmProcessor::loadPadFile(int pad, const juce::File& file)
{
    auto sample = readPadSample(file);
    if (!sample)
        return false;
    setPadSample(pad, *sample);
    return true;
}

void OpmProcessor::setPadSample(int pad, const PadSample& sample)
{
    pads_[static_cast<size_t>(pad)] = sample;
    rebuildBank();
    sendChangeMessage();
}

void OpmProcessor::restoreDefaultKit()
{
    pads_ = makeDefaultKit();
    rebuildBank();
    sendChangeMessage();
}

void OpmProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree root("OPM86State");
    root.appendChild(state_.copyState(), nullptr);
    // The program is saved by name, because the library may change between sessions.
    root.setProperty("program", getProgramName(currentProgram_), nullptr);
    root.setProperty("mode", mode_.load(), nullptr);
    root.setProperty("song", songName_, nullptr);
    root.setProperty("stem", stem_.load(), nullptr);

    juce::ValueTree pads("pads");
    for (const PadSample& s : pads_)
        pads.appendChild(sampleToTree(s), nullptr);
    root.appendChild(pads, nullptr);

    if (const auto xml = root.createXml())
        copyXmlToBinary(*xml, destData);
}

void OpmProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName("OPM86State"))
        return;
    const juce::ValueTree root = juce::ValueTree::fromXml(*xml);

    const juce::ValueTree params = root.getChildWithName(state_.state.getType());
    if (params.isValid())
        state_.replaceState(params);

    const juce::ValueTree pads = root.getChildWithName("pads");
    if (pads.getNumChildren() == kPads)
        for (int i = 0; i < kPads; ++i)
            pads_[static_cast<size_t>(i)] = sampleFromTree(pads.getChild(i));

    // The parameters hold the sound; the name only selects the list entry.
    const std::string program = root["program"].toString().toStdString();
    const auto match = std::find_if(patches_.begin(), patches_.end(),
                                    [&](const FmPatch& p) { return p.name == program; });
    currentProgram_ = match == patches_.end() ? 0 : static_cast<int>(match - patches_.begin());
    rebuildBank();

    mode_.store(static_cast<int>(root["mode"]) == 1 ? 1 : 0);
    setStem(static_cast<int>(root["stem"]));
    setSong(root["song"].toString());
    sendChangeMessage();
}

} // namespace opm86

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new opm86::OpmProcessor();
}
