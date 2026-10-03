#pragma once

#include "Engine.h"
#include "MidiExport.h"
#include "Parameters.h"
#include "SongCache.h"
#include "SongTrack.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <vector>

namespace opm86
{

class OpmProcessor : public juce::AudioProcessor, public juce::ChangeBroadcaster, private juce::Timer
{
public:
    // Synth plays patches and pads from MIDI notes; Song plays a VGM song
    // exactly, driven by a song track (see SongTrack.h).
    enum class Mode { Synth = 0, Song = 1 };
    OpmProcessor();

    // AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OPM86"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram_; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Message-thread API for the editor.
    juce::AudioProcessorValueTreeState& state() { return state_; }
    juce::MidiKeyboardState& keyboard() { return keyboard_; }
    const std::vector<FmPatch>& patches() const { return patches_; }
    const PadSet& pads() const { return pads_; }
    const std::vector<PadSample>& sampleLibrary() const { return sampleLibrary_; }

    // Imported songs are copies of their VGM files in this folder.
    static juce::File libraryDirectory();

    // Copies the file to the library and puts the song's samples on the pads.
    // Returns an error, a warning, or an empty string.
    juce::String importVgm(const juce::File& file);
    void loadSongKit(const std::string& song);
    bool loadPadFile(int pad, const juce::File& file);
    void setPadSample(int pad, const PadSample& sample);
    void restoreDefaultKit();
    void auditionPad(int pad) { auditions_.fetch_or(1u << pad); }

    // Song mode, message thread.
    Mode mode() const { return static_cast<Mode>(mode_.load()); }
    void setMode(Mode mode);
    juce::StringArray librarySongs() const;   // names of the imported songs
    juce::String songName() const { return songName_; }
    void setSong(const juce::String& name);
    int stem() const { return stem_.load(); }
    void setStem(int stem) { stemRequest_.store(stem); stem_.store(stem); sendChangeMessage(); }
    juce::String songStatus() const;

    // Finds and loads the song a song track asks for; a timer calls it ten
    // times a second.
    void update();

private:
    void timerCallback() override { update(); }
    void processSong(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);
    void loadLibrary();
    void rebuildBank();

    juce::AudioProcessorValueTreeState state_;
    ParameterRefs params_;
    Engine engine_;
    juce::MidiKeyboardState keyboard_;

    // Owned by the message thread; the audio thread only sees built banks.
    std::vector<FmPatch> patches_;
    std::vector<PadSample> sampleLibrary_;
    PadSet pads_;
    int currentProgram_ = 0;

    juce::SpinLock bankLock_;
    std::shared_ptr<const PcmBank> pendingBank_;
    std::shared_ptr<const PcmBank> retiredBank_;
    std::atomic<uint32_t> auditions_ { 0 };

    FmPatch lastPatch_;
    bool patchValid_ = false;

    // Song mode. The loaded song reaches the audio thread like a bank does.
    SongTrackPlayer songTrack_;
    std::atomic<int> mode_ { 0 };
    std::atomic<int> stem_ { 0 }, stemRequest_ { -1 };
    std::atomic<uint32_t> midiSong_ { 0 };  // song a song track asked for
    // A converted track's setup: the song it names, and the patch number or
    // drum kit it asks for (0 when none is pending).
    uint32_t setupId_ = 0;
    int setupIdParts_ = 0;
    std::atomic<uint32_t> setupSong_ { 0 };
    std::atomic<int> setupPatch_ { 0 };
    std::atomic<bool> setupKit_ { false };
    juce::String songName_;
    std::shared_ptr<const LoadedSong> loaded_;
    bool songWaiting_ = false;
    int seenMode_ = 0;
    juce::SpinLock songLock_;
    std::shared_ptr<const LoadedSong> pendingSong_, playingSong_, retiredSong_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpmProcessor)
};

} // namespace opm86
