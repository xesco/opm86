#include "Theme.h"

#include <array>

#include "BinaryData.h"

namespace opm86::theme
{

namespace
{

juce::Typeface::Ptr load(const void* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
}

} // namespace

juce::Font pixel(float height)
{
    static auto face = load(BinaryData::PressStart2PRegular_ttf, BinaryData::PressStart2PRegular_ttfSize);
    return juce::Font(juce::FontOptions(face).withHeight(height));
}

juce::Font mono(float height, bool bold)
{
    static auto regular = load(BinaryData::JetBrainsMonoRegular_ttf, BinaryData::JetBrainsMonoRegular_ttfSize);
    static auto heavy = load(BinaryData::JetBrainsMonoBold_ttf, BinaryData::JetBrainsMonoBold_ttfSize);
    return juce::Font(juce::FontOptions(bold ? heavy : regular).withHeight(height));
}

namespace
{
// Hues hand-spaced so neighbouring semitones stay apart on a dark ground.
constexpr float kHue[12] = { 0.020f, 0.075f, 0.120f, 0.165f, 0.250f, 0.330f,
                             0.440f, 0.520f, 0.585f, 0.680f, 0.790f, 0.895f };
std::array<juce::Colour, 12> chosen {}; // transparent: the default
int revision = 0;

int pitchClass(int midiNote) { return ((midiNote % 12) + 12) % 12; }
} // namespace

juce::Colour defaultPitchColour(int midiNote)
{
    return juce::Colour::fromHSL(kHue[pitchClass(midiNote)], 0.78f, 0.62f, 1.0f);
}

juce::Colour pitchColour(int midiNote)
{
    const juce::Colour c = chosen[static_cast<size_t>(pitchClass(midiNote))];
    return c.isTransparent() ? defaultPitchColour(midiNote) : c;
}

void setPitchColour(int pc, juce::Colour colour)
{
    chosen[static_cast<size_t>(pitchClass(pc))] = colour == defaultPitchColour(pc) ? juce::Colour() : colour;
    ++revision;
}

void resetPitchColours()
{
    chosen.fill(juce::Colour());
    ++revision;
}

int pitchColoursRevision() { return revision; }

juce::String pitchColoursText()
{
    // Twelve entries, C to B: a colour as hex, or "-" for the default.
    juce::StringArray entries;
    bool any = false;
    for (const juce::Colour& c : chosen)
    {
        entries.add(c.isTransparent() ? "-" : c.toDisplayString(false));
        any = any || !c.isTransparent();
    }
    return any ? entries.joinIntoString(",") : juce::String();
}

void setPitchColoursText(const juce::String& saved)
{
    const auto entries = juce::StringArray::fromTokens(saved, ",", "");
    for (int pc = 0; pc < 12; ++pc)
        chosen[static_cast<size_t>(pc)] = pc < entries.size() && entries[pc] != "-" && entries[pc].isNotEmpty()
                                              ? juce::Colour::fromString("ff" + entries[pc])
                                              : juce::Colour();
    ++revision;
}

const std::vector<juce::Colour>& pitchPalette()
{
    static const std::vector<juce::Colour> palette = [] {
        std::vector<juce::Colour> colours;
        for (const float lightness : { 0.74f, 0.62f, 0.46f })
            for (const float hue : kHue)
                colours.push_back(juce::Colour::fromHSL(hue, 0.78f, lightness, 1.0f));
        return colours;
    }();
    return palette;
}

juce::Colour instrumentColour(int index)
{
    static const juce::Colour kColours[8] = { orange, magenta, cyan, juce::Colour(0xffffd23d),
                                              juce::Colour(0xff8cff6b), juce::Colour(0xffb58cff),
                                              juce::Colour(0xffff7a6b), juce::Colour(0xff4dffc9) };
    return kColours[static_cast<size_t>(((index % 8) + 8) % 8)];
}

juce::String noteName(int midiNote)
{
    static const char* kNames[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    const int pc = ((midiNote % 12) + 12) % 12;
    const int octave = static_cast<int>(std::floor(midiNote / 12.0)) - 1;
    return juce::String(kNames[pc]) + juce::String(octave);
}

} // namespace opm86::theme
