#pragma once

#include "PcmBank.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

namespace opm86
{

// Read any format JUCE knows (WAV, AIFF, FLAC...) into a pad sample,
// mixed to mono and converted to the chip's 8-bit format.
inline std::optional<PadSample> readPadSample(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return std::nullopt;

    // The chip holds at most ~4 s, so a minute of input is plenty.
    const auto length = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples,
                                                               static_cast<juce::int64>(reader->sampleRate * 60.0)));
    const auto channels = static_cast<int>(reader->numChannels);
    juce::AudioBuffer<float> buffer(channels, length);
    reader->read(&buffer, 0, length, 0, true, true);

    std::vector<float> mono(static_cast<size_t>(length), 0.0f);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < length; ++i)
            mono[static_cast<size_t>(i)] += buffer.getSample(ch, i) / static_cast<float>(channels);
    return makeSample(file.getFileNameWithoutExtension().toStdString(), mono.data(), mono.size(), reader->sampleRate);
}

} // namespace opm86
