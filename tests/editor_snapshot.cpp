// Renders the plugin editor to a PNG, to check the layout without a host.

#include "PluginEditor.h"
#include "PluginProcessor.h"

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "usage: editor_snapshot OUT.png [song]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    opm86::OpmProcessor processor;
    if (argc == 3 && juce::String(argv[2]) == "song")
        processor.setMode(opm86::OpmProcessor::Mode::Song);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    editor->setBounds(editor->getBounds()); // lay out at the editor's own size

    const juce::Image image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    out.deleteFile();
    juce::FileOutputStream stream(out);
    juce::PNGImageFormat png;
    return stream.openedOk() && png.writeImageToStream(image, stream) ? 0 : 1;
}
