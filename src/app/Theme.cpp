#include "Theme.h"

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

juce::Colour pitchColour(int midiNote)
{
    const int pc = ((midiNote % 12) + 12) % 12;
    // Hues hand-spaced so neighbouring semitones stay apart on a dark ground.
    static constexpr float kHue[12] = { 0.020f, 0.075f, 0.120f, 0.165f, 0.250f, 0.330f,
                                        0.440f, 0.520f, 0.585f, 0.680f, 0.790f, 0.895f };
    return juce::Colour::fromHSL(kHue[pc], 0.78f, 0.62f, 1.0f);
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
