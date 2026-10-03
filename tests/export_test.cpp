// Checks that a VGM converted to MIDI and played through the synth sounds
// like the VGM played straight into the chips.

#include "Engine.h"
#include "MidiExport.h"
#include "TestVgm.h"
#include "VgmImport.h"
#include "VgmPlayer.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstdio>

using namespace opm86;

namespace
{

int failures = 0;
constexpr double kRate = 48000.0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        ++failures;
}

juce::MidiFile readMidi(const std::vector<uint8_t>& bytes)
{
    juce::MemoryInputStream in(bytes.data(), bytes.size(), false);
    juce::MidiFile midi;
    midi.readFrom(in, false);
    midi.convertTimestampTicksToSeconds();
    return midi;
}

// Loudness in 20 ms windows of the mono sum.
std::vector<double> envelope(const std::vector<float>& left, const std::vector<float>& right)
{
    const size_t window = static_cast<size_t>(kRate * 0.02);
    std::vector<double> out;
    for (size_t start = 0; start + window <= left.size(); start += window)
    {
        double sum = 0;
        for (size_t i = start; i < start + window; ++i)
        {
            const double x = (left[i] + right[i]) * 0.5;
            sum += x * x;
        }
        out.push_back(std::sqrt(sum / static_cast<double>(window)));
    }
    return out;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i)
        ma += a[i], mb += b[i];
    ma /= static_cast<double>(n), mb /= static_cast<double>(n);
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i)
    {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return ab / std::sqrt(aa * bb);
}

// Magnitude spectrum of the mono sum over a span, in 4096-point bins.
std::vector<double> spectrum(const std::vector<float>& left, const std::vector<float>& right, double from)
{
    constexpr int kSize = 4096;
    const auto start = static_cast<size_t>(from * kRate);
    std::vector<double> out(kSize / 2);
    for (int k = 0; k < kSize / 2; ++k)
    {
        double re = 0, im = 0;
        for (int i = 0; i < kSize; ++i)
        {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / kSize);
            const double x = hann * (left[start + static_cast<size_t>(i)] + right[start + static_cast<size_t>(i)]);
            re += x * std::cos(2.0 * juce::MathConstants<double>::pi * k * i / kSize);
            im -= x * std::sin(2.0 * juce::MathConstants<double>::pi * k * i / kSize);
        }
        out[static_cast<size_t>(k)] = std::sqrt(re * re + im * im);
    }
    return out;
}

size_t peak(const std::vector<double>& s)
{
    return static_cast<size_t>(std::max_element(s.begin(), s.end()) - s.begin());
}

} // namespace

int main()
{
    const std::vector<uint8_t> vgm = test::makeSong();
    const VgmImport song = importVgm(vgm, "Song");
    check(song.error.empty(), "the test song imports without error");
    check(song.patches.size() == 2 && song.samples.size() == 2, "it has two patches and two samples");

    const MidiExport exported = exportMidi(song, "Song");
    const juce::MidiFile midi = readMidi(exported.file);
    check(midi.getNumTracks() == 5, "the MIDI file has a tempo track, two instrument tracks and two drum tracks");
    check(std::abs(exported.tempo - 120.0) < 0.1, "the file runs at the song's 120 BPM");
    const double lead = exported.leadIn / kVgmRate; // seconds before the song's time 0
    const double beat = 60.0 / exported.tempo;

    // The lead's first note: A4 written for a 3.58 MHz clock sounds 1.92
    // semitones sharp at 4 MHz, so it is key 71 bent down 8 cents.
    int firstKey = -1, bends = 0, drumNotes = 0, quietLevel = -1, quietVelocity = -1, setupPatch = 0, setupKit = 0;
    bool drumGate = false, onBeat = true;
    for (int t = 1; t < midi.getNumTracks(); ++t)
        for (const auto* e : *midi.getTrack(t))
        {
            const auto& m = e->message;
            if (m.isNoteOn() && m.getChannel() == 1 && firstKey < 0)
                firstKey = m.getNoteNumber();
            const double songTime = m.getTimeStamp() - lead;
            if (m.isController() && m.getChannel() == 1 && m.getControllerNumber() == 11 && std::abs(songTime - 0.5) < 0.01)
                quietLevel = m.getControllerValue();
            if (m.isNoteOn() && m.getChannel() == 1 && std::abs(songTime - 0.5) < 0.01)
                quietVelocity = m.getVelocity();
            // The lead plays on the beats: every note within 2 ms of a beat line.
            if (m.isNoteOn() && m.getChannel() == 1)
            {
                const double beats = (m.getTimeStamp() - 4.0 * beat) / beat;
                onBeat = onBeat && std::abs(beats - std::round(beats)) * beat < 0.002;
            }
            if (m.isController() && m.getControllerNumber() == kPatchSelectController)
                setupPatch = m.getControllerValue();
            if (m.isController() && m.getControllerNumber() == kKitSelectController)
                setupKit = m.getControllerValue();
            if (m.isController() && m.getChannel() == 10 && m.getControllerNumber() == Engine::kDrumGateController)
                drumGate = m.getControllerValue() >= 64;
            if (m.isPitchWheel() && m.getChannel() == 1)
                ++bends;
            if (m.isNoteOn() && m.getChannel() == 10)
                ++drumNotes;
        }
    check(firstKey == 71, "the first lead note is key 71");
    check(quietVelocity == 95 && quietLevel == 127, "a note 8 level steps (6 dB) quieter has velocity 95");
    check(onBeat, "the lead's notes sit on the file's beats");
    check(setupPatch > 0 && setupKit == 1, "the tracks start with the controllers that set up the plugin");
    check(drumGate, "the drum track ends hits where the game cut them");
    check(bends >= 3, "the slide is written as pitch bend");
    check(drumNotes == 12, "every drum hit is a note on channel 10");

    // Play the MIDI file through the synth, set up as `opm86 render --vgm` does.
    juce::MidiMessageSequence events;
    for (int t = 0; t < midi.getNumTracks(); ++t)
        events.addSequence(*midi.getTrack(t), 0.0);
    events.sort();

    Engine engine;
    engine.prepare(kRate);
    PadSet pads = makeDefaultKit();
    for (size_t i = 0; i < song.samples.size(); ++i)
        pads[i] = song.samples[i];
    engine.setBank(buildBank(pads));
    engine.setPatchLibrary(song.patches);
    engine.fm().setPatch(song.patches.front());

    const VgmRender reference = renderVgm(vgm, kRate, 0.0);
    const size_t total = reference.left.size();
    std::vector<float> left(total), right(total);
    // Song time 0 comes after the lead-in; times snap to the VGM grid, as
    // `opm86 render` snaps them, and the setup before time 0 lands at 0.
    for (const auto* e : events)
    {
        const double songTime = std::max(0.0, std::round((e->message.getTimeStamp() - lead) * kVgmRate) / kVgmRate);
        engine.scheduleMidi(e->message.getRawData(), e->message.getRawDataSize(), songTime * kRate);
    }
    engine.render(left.data(), right.data(), static_cast<int>(total));

    const auto a = envelope(left, right), b = envelope(reference.left, reference.right);
    const double r = correlation(a, b);
    double sa = 0, sb = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        sa += a[i], sb += b[i];
    const double levelDb = 20.0 * std::log10(sa / sb);
    std::printf("      loudness correlation %.3f, level difference %+.2f dB\n", r, levelDb);
    check(r > 0.999, "the MIDI rendering follows the reference loudness over time");
    check(std::abs(levelDb) < 0.1, "the MIDI rendering is within 0.1 dB of the reference level");

    // Sample by sample: what is left after subtracting the reference.
    double diff = 0, power = 0;
    for (size_t i = 0; i < total; ++i)
    {
        diff += std::pow(left[i] - reference.left[i], 2) + std::pow(right[i] - reference.right[i], 2);
        power += std::pow(reference.left[i], 2) + std::pow(reference.right[i], 2);
    }
    const double residualDb = 10.0 * std::log10(diff / power);
    std::printf("      residual %.1f dB below the reference\n", residualDb);
    check(residualDb < -30.0, "the MIDI rendering matches the reference sample by sample to -30 dB");

    // The held end of the slide, between drum hits: the same spectrum and
    // the same strongest partial in both renderings.
    const auto ours = spectrum(left, right, 1.85), theirs = spectrum(reference.left, reference.right, 1.85);
    const double similarity = correlation(ours, theirs);
    std::printf("      spectrum after the slide: correlation %.3f, peaks at bins %zu and %zu\n", similarity,
                peak(ours), peak(theirs));
    check(similarity > 0.97, "the slide ends with the reference spectrum");
    check(peak(ours) == peak(theirs), "the slide ends on the reference pitch");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "some checks failed");
    return failures == 0 ? 0 : 1;
}
