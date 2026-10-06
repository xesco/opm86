#include "MainComponent.h"
#include "Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace opm86
{

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow(std::unique_ptr<MainComponent> content)
        : DocumentWindow("OPM86 Player", theme::background, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setResizeLimits(980, 560, 10000, 10000);
        setContentOwned(content.release(), true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class TrackerApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "OPM86 Player"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    // One window, except for snapshots, which must not hand over to a running app.
    bool moreThanOneInstanceAllowed() override { return getCommandLineParameters().startsWith("--snapshot"); }

    void initialise(const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        // Hidden: --snapshot rip.vgz out.png [seconds] [lane height] [tracks to collapse, as 0,2] [tracks to mute]
        // draws the window to a PNG and quits.
        if (args.size() >= 3 && args[0] == "--snapshot")
        {
            snapshot(juce::File(args[1].unquoted()), juce::File(args[2].unquoted()), args.size() > 3 ? args[3].getDoubleValue() : 0.0,
                     args.size() > 4 ? args[4].getIntValue() : 0, args.size() > 5 ? args[5] : juce::String(),
                     args.size() > 6 ? args[6] : juce::String());
            return;
        }
        // The last file opened is opened again, unless one is given or it is gone.
        juce::PropertiesFile::Options options;
        options.applicationName = getApplicationName();
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        settings_.setStorageParameters(options);
        auto content = std::make_unique<MainComponent>();
        content->onOpened = [this](const juce::File& file) {
            if (auto* user = settings_.getUserSettings())
                user->setValue("lastFile", file.getFullPathName()), user->saveIfNeeded();
        };
        const juce::File last(settings_.getUserSettings() ? settings_.getUserSettings()->getValue("lastFile") : juce::String());
        if (args.size() == 1 && juce::File(args[0].unquoted()).existsAsFile())
            content->open(juce::File(args[0].unquoted()));
        else if (last.existsAsFile())
            content->open(last);
        window_ = std::make_unique<MainWindow>(std::move(content));
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        const juce::File file(commandLine.unquoted());
        if (window_ && file.existsAsFile())
            if (auto* c = dynamic_cast<MainComponent*>(window_->getContentComponent()))
                c->open(file);
    }

    void shutdown() override
    {
        window_ = nullptr;
        snapshot_ = nullptr;
    }

private:
    juce::ApplicationProperties settings_;
    // Waits for the first build, then writes the component's image.
    struct Snapshot : juce::Timer
    {
        std::unique_ptr<MainComponent> component;
        juce::File out;
        int ticks = 0;
        bool play = false; // play silently first, so the meters show
        int playTicks = 0;

        void timerCallback() override
        {
            if (!component->model().ready() && ++ticks < 300)
                return;
            if (play)
            {
                // Playing waits for the snapshots to reach the position.
                if (!component->model().playing())
                {
                    component->model().play();
                    if (++ticks < 600)
                        return;
                }
                component->view().followPlayhead();
                component->view().updateMeters(0.02);
                if (++playTicks < 60)
                    return;
            }
            stopTimer();
            const auto image = component->createComponentSnapshot(component->getLocalBounds(), true, 2.0f);
            out.deleteFile();
            juce::FileOutputStream stream(out);
            juce::PNGImageFormat().writeImageToStream(image, stream);
            stream.flush();
            std::printf("%s\n", out.getFullPathName().toRawUTF8());
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    void snapshot(const juce::File& in, const juce::File& out, double seconds, int laneHeight, const juce::String& collapse,
                  const juce::String& mute)
    {
        snapshot_ = std::make_unique<Snapshot>();
        snapshot_->component = std::make_unique<MainComponent>();
        snapshot_->component->setSize(1400, 820);
        snapshot_->out = out;
        if (!snapshot_->component->open(in))
        {
            std::fprintf(stderr, "cannot open %s\n", in.getFullPathName().toRawUTF8());
            setApplicationReturnValue(1);
            quit();
            return;
        }
        if (seconds > 0)
        {
            snapshot_->component->model().setPosition(seconds * kVgmRate);
            snapshot_->component->model().setMonitor(false);
            snapshot_->play = true;
        }
        auto& view = snapshot_->component->view();
        if (laneHeight > 0)
            view.zoomLanes(static_cast<double>(laneHeight) / view.laneHeight(), TrackerView::kRulerHeight);
        for (const auto& track : juce::StringArray::fromTokens(collapse, ",", ""))
            view.setCollapsed(track.getIntValue(), true);
        for (const auto& track : juce::StringArray::fromTokens(mute, ",", ""))
            snapshot_->component->model().edit("Mute", [t = static_cast<size_t>(track.getIntValue())](Tune& tune) { tune.tracks[t].mute = true; });
        snapshot_->startTimer(20);
    }

    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<Snapshot> snapshot_;
};

} // namespace opm86

START_JUCE_APPLICATION(opm86::TrackerApplication)
