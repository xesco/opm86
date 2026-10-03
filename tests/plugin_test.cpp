// Checks of the plugin processor: audio output, bank hand-over and saved state.

#include "PluginProcessor.h"
#include "TestVgm.h"

#include <cstdio>

using namespace opm86;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        ++failures;
}

// Peak level of a note, ignoring the first blocks, which may still hold
// the tail of earlier sound.
float play(OpmProcessor& processor, int channel, int note, int skipBlocks = 0)
{
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(120)), 0);
    float peak = 0;
    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
        midi.clear();
        if (block >= skipBlocks)
            peak = std::max(peak, buffer.getMagnitude(0, 512));
    }
    return peak;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // Every processor in this test uses an empty library of its own.
    const juce::TemporaryFile scratch;
    const juce::File library = scratch.getFile().getChildFile("library");
    setenv("OPM86_LIBRARY_DIR", library.getFullPathName().toRawUTF8(), 1);

    OpmProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    check(play(processor, 1, 60) > 0.01f, "an FM note sounds on channel 1");
    check(play(processor, 10, 36) > 0.01f, "the default kick sounds on channel 10");

    // A pad with silence must play silence once the new bank reaches the audio
    // thread. The FM note from the first check still holds, so mute FM.
    processor.state().getParameter("fm_level")->setValueNotifyingHost(0.0f);
    PadSample quiet;
    quiet.name = "Quiet";
    quiet.data.assign(4096, 0x80);
    processor.setPadSample(0, quiet);
    check(play(processor, 10, 36, 2) < 0.001f, "a replaced pad sample is used by the audio thread");

    processor.setCurrentProgram(2);
    processor.state().getParameter("fb")->setValueNotifyingHost(processor.state().getParameter("fb")->convertTo0to1(3));

    juce::MemoryBlock saved;
    processor.getStateInformation(saved);

    OpmProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    check(restored.getCurrentProgram() == 2, "the current program is restored");
    check(restored.pads()[0].name == "Quiet" && restored.pads()[0].data.size() == 4096, "pad samples are restored");
    check(restored.pads()[1].data == processor.pads()[1].data, "untouched pads are restored byte for byte");
    check(static_cast<int>(restored.state().getRawParameterValue("fb")->load()) == 3, "parameters are restored");
    check(restored.getNumPrograms() == processor.getNumPrograms(), "the patch list is restored");

    // An imported song stays in the library for every later instance.
    const juce::File source = scratch.getFile().getChildFile("Test Song.vgm");
    const auto vgm = test::makeVgm();
    source.getParentDirectory().createDirectory();
    source.replaceWithData(vgm.data(), vgm.size());
    const int builtIn = OpmProcessor().getNumPrograms();
    const juce::String error = restored.importVgm(source);
    check(error.isEmpty(), "a VGM file imports without error");
    check(library.getChildFile("Test Song.vgm").existsAsFile(), "the import copies the file to the library");
    check(restored.getProgramName(restored.getCurrentProgram()) == "Test Song FM 1",
          "the import selects the song's first patch");
    check(restored.pads()[0].group == "Test Song", "the import puts the song's samples on the pads");

    OpmProcessor later;
    check(later.getNumPrograms() == builtIn + 2, "a new instance lists the imported patches");
    check(later.patches().back().group == "Test Song", "imported patches keep their song");
    check(later.sampleLibrary().size() == 1, "a new instance lists the imported samples");

    // Song mode: a song track's controllers switch an instance over and
    // name the song; its beat notes then play the song.
    OpmProcessor song;
    song.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer setup;
    setup.addEvent(juce::MidiMessage::controllerEvent(1, kStemController, 0), 0);
    const uint32_t id = songId("Test Song");
    for (int part = 0; part < 4; ++part)
        setup.addEvent(juce::MidiMessage::controllerEvent(1, kSongIdController + part, static_cast<int>((id >> (21 - 7 * part)) & 0x7F)), 0);
    song.processBlock(block, setup);
    check(song.mode() == OpmProcessor::Mode::Song, "a song track's controllers switch to song mode");
    for (int tries = 0; tries < 300 && (song.songName().isEmpty() || song.songStatus().startsWith("Loading")); ++tries)
    {
        song.update();
        juce::Thread::sleep(10);
    }
    check(song.songName() == "Test Song", "the song is found in the library by its id");
    check(!song.songStatus().startsWith("Loading"), "the song loads");

    uint8_t note = 0, velocity = 1;
    beatToNote(0, note, velocity);
    juce::MidiBuffer beat;
    beat.addEvent(juce::MidiMessage::noteOn(1, note, velocity), 0);
    float peak = 0;
    for (int i = 0; i < 40; ++i)
    {
        block.clear();
        song.processBlock(block, beat);
        beat.clear();
        peak = std::max(peak, block.getMagnitude(0, 512));
    }
    check(peak > 0.01f, "a beat note plays the song");

    // A converted track's setup controllers load its instrument; the drum
    // track's load the song's drums and play them on every channel.
    OpmProcessor instrument;
    instrument.prepareToPlay(48000.0, 512);
    juce::MidiBuffer pick;
    for (int part = 0; part < 4; ++part)
        pick.addEvent(juce::MidiMessage::controllerEvent(1, kSongIdController + part, static_cast<int>((id >> (21 - 7 * part)) & 0x7F)), 0);
    pick.addEvent(juce::MidiMessage::controllerEvent(1, kPatchSelectController, 2), 0);
    pick.addEvent(juce::MidiMessage::controllerEvent(1, kKitSelectController, 1), 0);
    instrument.processBlock(block, pick);
    instrument.update();
    check(instrument.mode() == OpmProcessor::Mode::Synth, "a converted track keeps the synth mode");
    check(instrument.getProgramName(instrument.getCurrentProgram()) == "Test Song FM 2",
          "a converted track's controllers load its instrument");
    check(instrument.pads()[0].group == "Test Song"
              && static_cast<int>(instrument.state().getRawParameterValue("drum_channel")->load()) == Engine::kDrumsOnAllChannels,
          "a drum track's controllers load the song's drums on every channel");

    scratch.getFile().deleteRecursively();
    std::printf("%s\n", failures == 0 ? "all checks passed" : "some checks failed");
    return failures == 0 ? 0 : 1;
}
