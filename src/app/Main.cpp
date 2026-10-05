#include "MainComponent.h"
#include "Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace opm86
{

class MainWindow : public juce::DocumentWindow
{
public:
    // Opens where it was when the app last quit, or centred the first time.
    explicit MainWindow(std::unique_ptr<MainComponent> content)
        : DocumentWindow("OPM86 Tracker", theme::background, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setResizeLimits(1020, 560, 10000, 10000);
        auto& settings = content->settings();
        setContentOwned(content.release(), true);
        centreWithSize(getWidth(), getHeight());
        if (const auto place = settings.getValue("mainWindow"); place.isNotEmpty())
            restoreWindowStateFromString(place);
        setVisible(true);
    }

    ~MainWindow() override
    {
        if (auto* c = dynamic_cast<MainComponent*>(getContentComponent()))
            c->settings().setValue("mainWindow", getWindowStateAsString());
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    MainComponent* content() { return dynamic_cast<MainComponent*>(getContentComponent()); }
};

class TrackerApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "OPM86 Tracker"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    // One window, except for snapshots, which must not hand over to a running app.
    bool moreThanOneInstanceAllowed() override { return getCommandLineParameters().startsWith("--snapshot"); }

    void initialise(const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        // Hidden: --snapshot rip.vgz out.png [seconds] [lane height] [tracks to mute, as 0,2, or - for none]
        // [track for the piano roll] draws the window to a PNG and quits.
        if (args.size() >= 3 && args[0] == "--snapshot")
        {
            snapshot(juce::File(args[1].unquoted()), juce::File(args[2].unquoted()), args.size() > 3 ? args[3].getDoubleValue() : 0.0,
                     args.size() > 4 ? args[4].getIntValue() : 0, args.size() > 5 && args[5] != "-" ? args[5] : juce::String(),
                     args.size() > 6 ? args[6].getIntValue() : -1);
            return;
        }
        auto content = std::make_unique<MainComponent>();
        if (args.size() == 1 && juce::File(args[0].unquoted()).existsAsFile())
            content->open(juce::File(args[0].unquoted()));
        window_ = std::make_unique<MainWindow>(std::move(content));
#if JUCE_MAC
        // The menus in the system's menu bar; other systems go without.
        juce::MenuBarModel::setMacMainMenu(window_->content());
#endif
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        const juce::File file(commandLine.unquoted());
        if (window_ && file.existsAsFile())
            if (auto* c = dynamic_cast<MainComponent*>(window_->getContentComponent()))
                c->open(file);
    }

    // Quitting asks first to save changes not saved.
    void systemRequestedQuit() override
    {
        if (window_ && window_->content())
            window_->content()->whenSaved([] { juce::JUCEApplication::getInstance()->quit(); });
        else
            quit();
    }

    void shutdown() override
    {
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
        window_ = nullptr;
        snapshot_ = nullptr;
    }

private:
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
            // Off screen no frame comes, so the strips take the header's width here.
            component->matchHeaderWidths();
            const auto image = component->createComponentSnapshot(component->getLocalBounds(), true, 2.0f);
            out.deleteFile();
            juce::FileOutputStream stream(out);
            juce::PNGImageFormat().writeImageToStream(image, stream);
            stream.flush();
            std::printf("%s\n", out.getFullPathName().toRawUTF8());
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    void snapshot(const juce::File& in, const juce::File& out, double seconds, int laneHeight, const juce::String& mute, int rollTrack)
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
        // The piano roll opens first, so the lane height sets its rows' height.
        view.setPianoRoll(rollTrack);
        if (laneHeight > 0)
            view.zoomLanes(static_cast<double>(laneHeight) / (view.inPianoRoll() ? view.rowHeight() : view.laneHeight()), TrackerView::kRulerHeight);
        for (const auto& track : juce::StringArray::fromTokens(mute, ",", ""))
            snapshot_->component->model().edit("Mute", [t = static_cast<size_t>(track.getIntValue())](Tune& tune) { tune.tracks[t].mute = true; });
        snapshot_->startTimer(20);
    }

    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<Snapshot> snapshot_;
};

} // namespace opm86

START_JUCE_APPLICATION(opm86::TrackerApplication)
