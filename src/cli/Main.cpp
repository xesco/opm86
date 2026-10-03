// opm86: play and render the synth without a DAW.

#include "AudioFile.h"
#include "Engine.h"
#include "MidiExport.h"
#include "SongTrack.h"
#include "VgmSong.h"
#include "VgmImport.h"
#include "VgmPlayer.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace opm86;

namespace
{

constexpr const char* kUsage = R"(usage: opm86 <command> [options]

commands:
  play                      play live from every connected MIDI input
  render IN.mid OUT.wav     render a standard MIDI file to a WAV file
  demo OUT.wav              render a short groove to a WAV file
  vgm2mid IN.vgz OUT.mid    convert a VGM file to a MIDI file that plays its patches and drums
  vgmplay IN.vgz OUT.wav    render a VGM file as the game's chips play it, to compare with render
  songtrack IN.vgz OUT.mid  write a song track, one note per beat, to rearrange the exact song
  patches                   list the patches and the drum pads
  tune                      measure the tuning of six notes

options:
  --patch N|NAME            use this patch on every MIDI channel (default: 1)
  --vgm FILE                import patches and drum samples from a .vgm or .vgz file
  --pad NOTE=FILE           put an audio file on the drum pad for NOTE (36-51); repeat for more pads
  --drum-channel N          play drums on MIDI channel N, on every channel with all, or on none with 0 (default: 10)
  --rate HZ                 render at HZ samples per second (default: 48000)
  --tail SECONDS            keep rendering for SECONDS after the last MIDI event (default: 2)
  --song FILE               render a song track by playing FILE, a .vgm or .vgz, exactly
  --mix                     write the song track as one track for the whole song, not one per stem

A MIDI program change P selects patch number P + 1 in the list that `opm86 patches` prints.
)";

struct Options
{
    std::vector<std::string> positional;
    std::string patch = "1";
    std::string vgm;
    std::string song;
    bool mix = false;
    std::map<int, std::string> pads;
    int drumChannel = 10;
    double rate = 48000.0;
    double tail = 2.0;
};

std::optional<Options> parse(int argc, char** argv)
{
    Options o;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--patch" && hasValue)
            o.patch = argv[++i];
        else if (arg == "--vgm" && hasValue)
            o.vgm = argv[++i];
        else if (arg == "--drum-channel" && hasValue)
        {
            const std::string value = argv[++i];
            o.drumChannel = value == "all" ? Engine::kDrumsOnAllChannels : std::atoi(value.c_str());
            if (o.drumChannel < 0 || o.drumChannel > Engine::kDrumsOnAllChannels)
            {
                std::fprintf(stderr, "opm86: --drum-channel takes 0 to 16 or all, not %s\n", value.c_str());
                return std::nullopt;
            }
        }
        else if (arg == "--rate" && hasValue)
            o.rate = std::atof(argv[++i]);
        else if (arg == "--tail" && hasValue)
            o.tail = std::atof(argv[++i]);
        else if (arg == "--song" && hasValue)
            o.song = argv[++i];
        else if (arg == "--mix")
            o.mix = true;
        else if (arg == "--pad" && hasValue)
        {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            const int note = eq == std::string::npos ? 0 : std::atoi(spec.substr(0, eq).c_str());
            if (note < kFirstPadNote || note >= kFirstPadNote + kPads)
            {
                std::fprintf(stderr, "opm86: --pad needs NOTE=FILE with NOTE from %d to %d\n", kFirstPadNote,
                             kFirstPadNote + kPads - 1);
                return std::nullopt;
            }
            o.pads[note] = spec.substr(eq + 1);
        }
        else if (arg.rfind("--", 0) == 0)
        {
            std::fprintf(stderr, "opm86: unknown option or missing value: %s\n", arg.c_str());
            return std::nullopt;
        }
        else
            o.positional.push_back(arg);
    }
    if (o.rate < 8000.0 || o.rate > 384000.0)
    {
        std::fprintf(stderr, "opm86: --rate must be between 8000 and 384000\n");
        return std::nullopt;
    }
    return o;
}

// Everything the options load: the patch list, the pads and the patch to start with.
struct Setup
{
    std::vector<FmPatch> patches;
    PadSet pads = makeDefaultKit();
    size_t initialPatch = 0;
    int drumChannel = 10;
};

std::optional<Setup> load(const Options& o)
{
    Setup s;
    s.patches = factoryPatches();
    s.drumChannel = o.drumChannel;

    if (!o.vgm.empty())
    {
        const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(o.vgm);
        juce::MemoryBlock bytes;
        if (!file.loadFileAsData(bytes))
        {
            std::fprintf(stderr, "opm86: cannot read %s\n", o.vgm.c_str());
            return std::nullopt;
        }
        const auto* begin = static_cast<const uint8_t*>(bytes.getData());
        const VgmImport result = importVgm({ begin, begin + bytes.getSize() },
                                           file.getFileNameWithoutExtension().toStdString());
        if (!result.error.empty())
            std::fprintf(stderr, "opm86: %s: %s\n", o.vgm.c_str(), result.error.c_str());
        if (result.patches.empty() && result.samples.empty())
            return std::nullopt;

        // Imported patches come first, so the song's sounds are 1, 2, 3...
        s.patches.insert(s.patches.begin(), result.patches.begin(), result.patches.end());
        for (size_t i = 0; i < result.samples.size() && i < s.pads.size(); ++i)
            s.pads[i] = result.samples[i];
        std::printf("imported %zu patches and %zu samples from %s\n", result.patches.size(),
                    result.samples.size(), o.vgm.c_str());
        if (result.samples.size() > s.pads.size())
            std::printf("only the %zu most used samples fit on the pads\n", s.pads.size());
    }

    for (const auto& [note, path] : o.pads)
    {
        const auto sample = readPadSample(juce::File::getCurrentWorkingDirectory().getChildFile(path));
        if (!sample)
        {
            std::fprintf(stderr, "opm86: cannot read audio file %s\n", path.c_str());
            return std::nullopt;
        }
        s.pads[static_cast<size_t>(note - kFirstPadNote)] = *sample;
    }

    // --patch takes a 1-based number or a case-insensitive name.
    const juce::String wanted(o.patch);
    if (wanted.containsOnly("0123456789"))
    {
        const int n = wanted.getIntValue();
        if (n < 1 || n > static_cast<int>(s.patches.size()))
        {
            std::fprintf(stderr, "opm86: --patch must be from 1 to %zu\n", s.patches.size());
            return std::nullopt;
        }
        s.initialPatch = static_cast<size_t>(n - 1);
    }
    else
    {
        auto it = std::find_if(s.patches.begin(), s.patches.end(),
                               [&](const FmPatch& p) { return wanted.equalsIgnoreCase(juce::String(p.name)); });
        if (it == s.patches.end())
        {
            std::fprintf(stderr, "opm86: no patch named \"%s\"; run `opm86 patches` for the list\n", o.patch.c_str());
            return std::nullopt;
        }
        s.initialPatch = static_cast<size_t>(it - s.patches.begin());
    }
    return s;
}

void configure(Engine& engine, const Setup& s, double rate)
{
    engine.prepare(rate);
    engine.setBank(buildBank(s.pads));
    engine.setDrumChannel(s.drumChannel);
    engine.setPatchLibrary(s.patches);
    engine.fm().setPatch(s.patches[s.initialPatch]);
}

// Render a sorted MIDI sequence, splitting the audio at every event.
juce::AudioBuffer<float> renderSequence(Engine& engine, const juce::MidiMessageSequence& events, double seconds,
                                        double rate)
{
    const int total = static_cast<int>(std::ceil(seconds * rate));
    juce::AudioBuffer<float> out(2, total);
    out.clear();

    // Events are queued at their exact time, a little ahead of each block,
    // since the resampler reads a few chip samples ahead of the audio.
    constexpr int kBlock = 512, kAhead = 64;
    int next = 0;
    for (int pos = 0; pos < total; pos += kBlock)
    {
        const int n = std::min(kBlock, total - pos);
        for (; next < events.getNumEvents(); ++next)
        {
            const juce::MidiMessage& m = events.getEventPointer(next)->message;
            // Times snap to 1/44100 s, the grid of VGM files and of the files
            // vgm2mid writes, undoing the microsecond rounding of MIDI tempos.
            const double at = std::round(m.getTimeStamp() * kVgmRate) / kVgmRate * rate;
            if (at >= pos + n + kAhead)
                break;
            engine.scheduleMidi(m.getRawData(), m.getRawDataSize(), at);
        }
        engine.render(out.getWritePointer(0, pos), out.getWritePointer(1, pos), n);
    }
    return out;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions {}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(24);
    const auto writer = wav.createWriterFor(stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

int finishRender(const juce::AudioBuffer<float>& audio, const std::string& path, double rate)
{
    const float peak = std::max(audio.getMagnitude(0, 0, audio.getNumSamples()),
                                audio.getMagnitude(1, 0, audio.getNumSamples()));
    if (!writeWav(juce::File::getCurrentWorkingDirectory().getChildFile(path), audio, rate))
    {
        std::fprintf(stderr, "opm86: cannot write %s\n", path.c_str());
        return 1;
    }
    std::printf("wrote %s: %.1f s, peak %.1f dBFS%s\n", path.c_str(), audio.getNumSamples() / rate,
                juce::Decibels::gainToDecibels(peak, -120.0f), peak > 1.0f ? " (clipped)" : "");
    return 0;
}

std::optional<std::vector<uint8_t>> readBytes(const std::string& path);

// Copy a VGM file to the plugin's library folder, where the plugin finds the
// song that a converted file or a song track names.
void addToLibrary(const std::string& path)
{
    const juce::File source = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    const juce::String custom = juce::SystemStats::getEnvironmentVariable("OPM86_LIBRARY_DIR", {});
    const juce::File library = custom.isNotEmpty() ? juce::File(custom)
                                                   : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                                         .getChildFile("Application Support/OPM86/VGM");
    const juce::File copy = library.getChildFile(source.getFileName());
    if (copy != source && (!library.createDirectory() || !source.copyFileTo(copy)))
    {
        std::fprintf(stderr, "opm86: cannot copy %s to %s\n", path.c_str(), library.getFullPathName().toRawUTF8());
        return;
    }
    const juce::String home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
    juce::String shown = copy.getFullPathName();
    if (shown.startsWith(home))
        shown = "~" + shown.substring(home.length());
    std::printf("added the song to the plugin's library: %s\n", shown.toRawUTF8());
}

std::shared_ptr<const VgmSong> readSong(const std::string& path)
{
    const auto bytes = readBytes(path);
    if (!bytes)
        return nullptr;
    const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    std::string error;
    auto song = loadVgmSong(*bytes, file.getFileNameWithoutExtension().toStdString(), error);
    if (!error.empty())
        std::fprintf(stderr, "opm86: %s: %s\n", path.c_str(), error.c_str());
    return song;
}

// Each track of a song track file drives its own song player, as each
// GarageBand track drives its own plugin; the players' outputs are mixed.
int renderSongTrack(const Options& o, const juce::MidiFile& midi)
{
    const auto song = readSong(o.song);
    if (!song)
        return 1;
    const std::shared_ptr<const SongSnapshots> snapshots = makeSnapshots(*song);

    double end = 0;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        end = std::max(end, midi.getTrack(t)->getEndTime());
    const int total = static_cast<int>(std::ceil((end + o.tail) * o.rate));
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    std::vector<float> left(static_cast<size_t>(total)), right(static_cast<size_t>(total));

    int players = 0;
    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const juce::MidiMessageSequence& track = *midi.getTrack(t);
        bool hasNotes = false;
        for (const auto* e : track)
            hasNotes = hasNotes || e->message.isNoteOn();
        if (!hasNotes)
            continue;
        ++players;

        SongTrackPlayer player;
        player.player().prepare(o.rate);
        player.player().setSong(song, snapshots);
        int pos = 0, i = 0;
        const int count = track.getNumEvents();
        while (i < count)
        {
            // All messages at one sample, then the check for a stop.
            const int at = std::clamp(static_cast<int>(std::lround(track.getEventPointer(i)->message.getTimeStamp() * o.rate)), 0, total);
            if (at > pos)
                player.player().render(&left[static_cast<size_t>(pos)], &right[static_cast<size_t>(pos)], at - pos);
            pos = std::max(pos, at);
            for (; i < count && std::min<long>(total, std::lround(track.getEventPointer(i)->message.getTimeStamp() * o.rate)) <= at; ++i)
            {
                const juce::MidiMessage& m = track.getEventPointer(i)->message;
                player.handleMidi(m.getRawData(), m.getRawDataSize());
            }
            player.endOfEvents();
        }
        if (total > pos)
            player.player().render(&left[static_cast<size_t>(pos)], &right[static_cast<size_t>(pos)], total - pos);
        out.addFrom(0, 0, left.data(), total);
        out.addFrom(1, 0, right.data(), total);
    }
    std::printf("played %d tracks of %s\n", players, o.song.c_str());
    return finishRender(out, o.positional[1], o.rate);
}

int songTrack(const Options& o)
{
    if (o.positional.size() != 2)
    {
        std::fprintf(stderr, "usage: opm86 songtrack IN.vgz OUT.mid [--mix]\n");
        return 2;
    }
    const auto song = readSong(o.positional[0]);
    if (!song)
        return 1;
    const SongTrackFile track = writeSongTrack(*song, !o.mix);
    if (!juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[1]).replaceWithData(track.file.data(), track.file.size()))
    {
        std::fprintf(stderr, "opm86: cannot write %s\n", o.positional[1].c_str());
        return 1;
    }
    const auto seconds = static_cast<int>(song->length / kVgmRate);
    std::printf("wrote %s: %zu tracks, %d beats, %d:%02d\n", o.positional[1].c_str(), track.stems.size(),
                song->beatCount, seconds / 60, seconds % 60);

    addToLibrary(o.positional[0]);
    std::printf("tempo %.2f BPM; set the project to %.0f BPM\n", track.tempo, std::round(track.tempo));
    for (int stem : track.stems)
        std::printf("  %s\n", stemName(stem).c_str());
    return 0;
}

int render(const Options& o, const Setup& s)
{
    if (o.positional.size() != 2)
    {
        std::fprintf(stderr, "usage: opm86 render IN.mid OUT.wav [options]\n");
        return 2;
    }
    juce::FileInputStream in(juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[0]));
    juce::MidiFile midi;
    // No added note offs: two hits of one pad may overlap, as the game's do.
    if (!in.openedOk() || !midi.readFrom(in, false))
    {
        std::fprintf(stderr, "opm86: cannot read MIDI file %s\n", o.positional[0].c_str());
        return 1;
    }
    midi.convertTimestampTicksToSeconds();

    if (!o.song.empty())
        return renderSongTrack(o, midi);

    juce::MidiMessageSequence events;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        events.addSequence(*midi.getTrack(t), 0.0);
    events.sort();

    Engine engine;
    configure(engine, s, o.rate);
    const double length = events.getEndTime() + o.tail;
    return finishRender(renderSequence(engine, events, length, o.rate), o.positional[1], o.rate);
}

juce::MidiMessageSequence groove()
{
    juce::MidiMessageSequence events;
    const double beat = 0.5; // 120 BPM
    const int bass[4] = { 29, 29, 31, 31 };
    const int chords[2][3] = { { 65, 69, 72 }, { 67, 71, 74 } };
    auto note = [&](int channel, int n, int velocity, double start, double length) {
        events.addEvent(juce::MidiMessage::noteOn(channel, n, static_cast<juce::uint8>(velocity)), start);
        events.addEvent(juce::MidiMessage::noteOff(channel, n), start + length);
    };

    for (int bar = 0; bar < 2; ++bar)
    {
        const double t0 = bar * 4 * beat;
        for (int i = 0; i < 8; ++i)
        {
            const double t = t0 + i * beat / 2;
            note(2, bass[(bar * 2 + i / 4) % 4] + (i % 2 ? 12 : 0), 110, t, beat * 0.4);
            note(10, 42, i % 2 ? 70 : 100, t, 0.1);
            if (i % 4 == 0)
                note(10, 36, 127, t, 0.1);
            if (i % 4 == 2)
                note(10, 38, 120, t, 0.1);
        }
        for (int n : chords[bar])
            note(1, n, 90, t0, 3.5 * beat);
    }
    note(10, 49, 110, 4.0, 0.1);

    // Channel 1 plays the chosen patch; channel 2 the bass.
    events.addEvent(juce::MidiMessage::programChange(2, 1), 0.0);
    events.sort();
    return events;
}

int demo(const Options& o, const Setup& s)
{
    if (o.positional.size() != 1)
    {
        std::fprintf(stderr, "usage: opm86 demo OUT.wav [options]\n");
        return 2;
    }
    Engine engine;
    configure(engine, s, o.rate);
    engine.setDrumChannel(10);
    const auto events = groove();
    return finishRender(renderSequence(engine, events, events.getEndTime() + o.tail, o.rate), o.positional[0],
                        o.rate);
}

std::optional<std::vector<uint8_t>> readBytes(const std::string& path)
{
    juce::MemoryBlock bytes;
    if (!juce::File::getCurrentWorkingDirectory().getChildFile(path).loadFileAsData(bytes))
    {
        std::fprintf(stderr, "opm86: cannot read %s\n", path.c_str());
        return std::nullopt;
    }
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    return std::vector<uint8_t>(begin, begin + bytes.getSize());
}

int vgmToMidi(const Options& o)
{
    if (o.positional.size() != 2)
    {
        std::fprintf(stderr, "usage: opm86 vgm2mid IN.vgz OUT.mid\n");
        return 2;
    }
    const auto bytes = readBytes(o.positional[0]);
    if (!bytes)
        return 1;
    const juce::File in = juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[0]);
    const std::string song = in.getFileNameWithoutExtension().toStdString();
    const VgmImport imported = importVgm(*bytes, song);
    if (!imported.error.empty())
        std::fprintf(stderr, "opm86: %s: %s\n", o.positional[0].c_str(), imported.error.c_str());
    if (imported.notes.empty() && imported.hits.empty())
        return 1;

    const MidiExport midi = exportMidi(imported, song);
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(o.positional[1]);
    if (!out.replaceWithData(midi.file.data(), midi.file.size()))
    {
        std::fprintf(stderr, "opm86: cannot write %s\n", o.positional[1].c_str());
        return 1;
    }

    const auto seconds = static_cast<int>(imported.length / kVgmRate);
    std::printf("wrote %s: %zu tracks, %d:%02d\n", o.positional[1].c_str(), midi.tracks.size(), seconds / 60,
                seconds % 60);
    std::printf("  %-36s %-16s %5s %6s\n", "track", "MIDI channels", "patch", "notes");
    for (const auto& t : midi.tracks)
    {
        std::string channels;
        for (int c : t.channels)
            channels += (channels.empty() ? "" : ",") + std::to_string(c);
        const std::string patch = t.patch == 0 ? "-" : std::to_string(t.patch);
        std::printf("  %-36s %-16s %5s %6zu\n", t.name.c_str(), channels.c_str(), patch.c_str(), t.notes);
    }
    std::printf("tempo %.2f BPM; set a music program to %.0f BPM before you bring the file in\n", midi.tempo,
                std::round(midi.tempo));
    addToLibrary(o.positional[0]);
    if (midi.skippedHits > 0)
        std::printf("left out %zu drum hits of samples beyond the 16 pads\n", midi.skippedHits);
    // Quote paths with spaces, so the line can be pasted into a shell.
    auto shell = [](const std::string& path) {
        return path.find(' ') == std::string::npos ? path : "\"" + path + "\"";
    };
    std::printf("play it with: opm86 render %s OUT.wav --vgm %s\n", shell(o.positional[1]).c_str(),
                shell(o.positional[0]).c_str());
    return 0;
}

int vgmPlay(const Options& o)
{
    if (o.positional.size() != 2)
    {
        std::fprintf(stderr, "usage: opm86 vgmplay IN.vgz OUT.wav\n");
        return 2;
    }
    const auto bytes = readBytes(o.positional[0]);
    if (!bytes)
        return 1;
    const VgmRender song = renderVgm(*bytes, o.rate, o.tail);
    if (!song.error.empty())
        std::fprintf(stderr, "opm86: %s: %s\n", o.positional[0].c_str(), song.error.c_str());
    if (song.left.empty())
        return 1;

    juce::AudioBuffer<float> audio(2, static_cast<int>(song.left.size()));
    audio.copyFrom(0, 0, song.left.data(), audio.getNumSamples());
    audio.copyFrom(1, 0, song.right.data(), audio.getNumSamples());
    return finishRender(audio, o.positional[1], o.rate);
}

int listPatches(const Setup& s)
{
    std::printf("patches (MIDI program change P selects patch P + 1):\n");
    for (size_t i = 0; i < s.patches.size(); ++i)
        std::printf("  %3zu  %-24s alg %d  %s\n", i + 1, s.patches[i].name.c_str(), s.patches[i].alg,
                    kAlgorithmDiagrams[s.patches[i].alg & 7]);
    const std::string channel = s.drumChannel == Engine::kDrumsOnAllChannels ? "every MIDI channel"
                                : s.drumChannel == 0                        ? "no MIDI channel"
                                                                            : "MIDI channel " + std::to_string(s.drumChannel);
    std::printf("drum pads (%s):\n", channel.c_str());
    for (int pad = 0; pad < kPads; ++pad)
    {
        const PadSample& p = s.pads[static_cast<size_t>(pad)];
        std::printf("  %3d  %-24s %6zu bytes%s\n", kFirstPadNote + pad, p.name.c_str(), p.data.size(),
                    p.loop ? "  loop" : "");
    }
    return 0;
}

double measureFrequency(const float* x, int from, int to, double rate)
{
    double first = -1, last = -1;
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + static_cast<double>(x[i - 1] / (x[i - 1] - x[i]));
            if (first < 0)
                first = t;
            else
                ++crossings;
            last = t;
        }
    }
    return crossings > 0 ? rate * crossings / (last - first) : 0.0;
}

int tune()
{
    constexpr double rate = 48000.0;
    double worst = 0;
    for (int note : { 33, 45, 57, 69, 81, 93 })
    {
        Engine engine;
        engine.prepare(rate);
        engine.fm().setPatch(factoryPatches().back()); // Init Sine
        juce::MidiMessageSequence events;
        events.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(127)), 0.0);
        const auto audio = renderSequence(engine, events, 1.0, rate);

        const double expected = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        const int n = audio.getNumSamples();
        const double measured = measureFrequency(audio.getReadPointer(0), n / 4, n, rate);
        const double cents = 1200.0 * std::log2(measured / expected);
        worst = std::max(worst, std::abs(cents));
        std::printf("note %3d  expected %8.2f Hz  measured %8.2f Hz  %+5.1f cents\n", note, expected, measured, cents);
    }
    std::printf("worst error %.1f cents\n", worst);
    return worst <= 3.0 ? 0 : 1;
}

// Live playing: MIDI inputs feed a collector the audio callback drains.
class LivePlayer : public juce::AudioIODeviceCallback, public juce::MidiInputCallback
{
public:
    explicit LivePlayer(const Setup& setup) : setup_(setup) {}

    void selectPatch(size_t index) { pendingPatch_.store(static_cast<int>(index)); }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        const double rate = device->getCurrentSampleRate();
        collector_.reset(rate);
        configure(engine_, setup_, rate);
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        const int patch = pendingPatch_.exchange(-1);
        if (patch >= 0)
            engine_.fm().setPatch(setup_.patches[static_cast<size_t>(patch)]);

        juce::MidiBuffer midi;
        collector_.removeNextBlockOfMessages(midi, numSamples);

        float scratchL[4096], scratchR[4096];
        float* left = numOutputs > 0 && outputs[0] ? outputs[0] : scratchL;
        float* right = numOutputs > 1 && outputs[1] ? outputs[1] : scratchR;
        numSamples = std::min(numSamples, 4096);

        const double blockStart = engine_.hostTime();
        for (const auto meta : midi)
            engine_.scheduleMidi(meta.data, meta.numBytes, blockStart + std::clamp(meta.samplePosition, 0, numSamples));
        engine_.render(left, right, numSamples);
        for (int ch = 2; ch < numOutputs; ++ch)
            if (outputs[ch])
                juce::FloatVectorOperations::clear(outputs[ch], numSamples);
    }

    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        collector_.addMessageToQueue(message);
    }

private:
    const Setup& setup_;
    Engine engine_;
    juce::MidiMessageCollector collector_;
    std::atomic<int> pendingPatch_ { -1 };
};

int play(const Setup& s)
{
    juce::ScopedJuceInitialiser_GUI juce;
    LivePlayer player(s);

    juce::AudioDeviceManager devices;
    const juce::String error = devices.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty() || devices.getCurrentAudioDevice() == nullptr)
    {
        std::fprintf(stderr, "opm86: cannot open audio output: %s\n", error.toRawUTF8());
        return 1;
    }
    devices.addAudioCallback(&player);

    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
    for (const auto& info : juce::MidiInput::getAvailableDevices())
        if (auto input = juce::MidiInput::openDevice(info.identifier, &player))
        {
            input->start();
            std::printf("MIDI input: %s\n", info.name.toRawUTF8());
            inputs.push_back(std::move(input));
        }
    if (inputs.empty())
        std::printf("no MIDI inputs found; connect a keyboard and restart\n");

    auto* device = devices.getCurrentAudioDevice();
    std::printf("audio: %s at %.0f Hz\n", device->getName().toRawUTF8(), device->getCurrentSampleRate());
    std::printf("playing \"%s\". Type a patch number to switch, l to list, q to quit.\n",
                s.patches[s.initialPatch].name.c_str());

    std::string line;
    while (std::printf("> "), std::fflush(stdout), std::getline(std::cin, line))
    {
        const juce::String command = juce::String(line).trim();
        if (command == "q" || command == "quit")
            break;
        if (command == "l" || command == "list")
        {
            for (size_t i = 0; i < s.patches.size(); ++i)
                std::printf("  %3zu  %s\n", i + 1, s.patches[i].name.c_str());
            continue;
        }
        const int n = command.getIntValue();
        if (command.containsOnly("0123456789") && n >= 1 && n <= static_cast<int>(s.patches.size()))
        {
            player.selectPatch(static_cast<size_t>(n - 1));
            std::printf("patch %d: %s\n", n, s.patches[static_cast<size_t>(n - 1)].name.c_str());
        }
        else if (command.isNotEmpty())
            std::printf("type a patch number from 1 to %zu, l or q\n", s.patches.size());
    }

    for (auto& input : inputs)
        input->stop();
    devices.removeAudioCallback(&player);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string command = argc > 1 ? argv[1] : "";
    if (command.empty() || command == "-h" || command == "--help" || command == "help")
    {
        std::fputs(kUsage, command.empty() ? stderr : stdout);
        return command.empty() ? 2 : 0;
    }
    if (command == "tune")
        return tune();

    const auto options = parse(argc, argv);
    if (!options)
        return 2;
    if (command == "vgm2mid")
        return vgmToMidi(*options);
    if (command == "vgmplay")
        return vgmPlay(*options);
    if (command == "songtrack")
        return songTrack(*options);
    const auto setup = load(*options);
    if (!setup)
        return 1;

    if (command == "play")
        return play(*setup);
    if (command == "render")
        return render(*options, *setup);
    if (command == "demo")
        return demo(*options, *setup);
    if (command == "patches")
        return listPatches(*setup);

    std::fprintf(stderr, "opm86: unknown command \"%s\"\n\n%s", command.c_str(), kUsage);
    return 2;
}
