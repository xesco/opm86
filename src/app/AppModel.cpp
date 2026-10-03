#include "AppModel.h"

namespace opm86
{

// One undoable edit: the tracks before and after it.
class AppModel::TracksChange : public juce::UndoableAction
{
public:
    TracksChange(AppModel& model, std::vector<TuneTrack> before, std::vector<TuneTrack> after)
        : model_(model), before_(std::move(before)), after_(std::move(after))
    {
    }

    bool perform() override
    {
        model_.setTracks(after_);
        return true;
    }

    bool undo() override
    {
        model_.setTracks(before_);
        return true;
    }

    int getSizeInUnits() override
    {
        size_t notes = 0;
        for (const auto& t : before_)
            notes += t.notes.size();
        return static_cast<int>(notes / 50 + 1);
    }

private:
    AppModel& model_;
    std::vector<TuneTrack> before_, after_;
};

AppModel::AppModel()
{
    devices_.initialiseWithDefaultDevices(0, 2);
    devices_.addAudioCallback(this);
    startTimerHz(30);
}

AppModel::~AppModel()
{
    devices_.removeAudioCallback(this);
}

bool AppModel::open(const juce::File& file, juce::String& error)
{
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes))
    {
        error = "Cannot read " + file.getFullPathName();
        return false;
    }
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    std::string message;
    auto tune = Tune::load({ begin, begin + bytes.getSize() }, file.getFileNameWithoutExtension().toStdString(), message);
    if (!tune)
    {
        error = message;
        return false;
    }
    stop();
    tune_ = std::move(tune);
    ++revision_;
    file_ = file;
    undo_.clearUndoHistory();
    built_ = nullptr;
    report_ = {};
    position_.store(0.0);
    rebuild();
    sendChangeMessage();
    return true;
}

void AppModel::edit(const juce::String& name, const std::function<void(Tune&)>& change, bool newStep)
{
    if (!tune_)
        return;
    std::vector<TuneTrack> before = tune_->tracks;
    Tune changed = *tune_;
    change(changed);
    if (newStep)
        undo_.beginNewTransaction(name);
    undo_.perform(new TracksChange(*this, std::move(before), changed.tracks));
}

void AppModel::setTracks(const std::vector<TuneTrack>& tracks)
{
    tune_->tracks = tracks;
    ++revision_;
    rebuild();
    sendChangeMessage();
}

void AppModel::rebuild()
{
    rebuilder_.request(*tune_, position_.load());
}

void AppModel::play()
{
    if (!built_)
        return;
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    commands_.play = true;
    commands_.stop = false;
    commands_.playAt = position_.load();
}

void AppModel::stop()
{
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    commands_.stop = true;
    commands_.play = false;
}

double AppModel::displayPosition() const
{
    const double p = position_.load();
    if (!playing_.load())
        return p;
    // Up to 50 ms ahead of the last audio block.
    const double elapsed = std::clamp(juce::Time::getMillisecondCounterHiRes() - positionStamp_.load(), 0.0, 50.0);
    return p + elapsed * kVgmRate / 1000.0;
}

void AppModel::setPosition(double songTime)
{
    position_.store(std::max(0.0, songTime));
    if (playing_.load())
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.play = true;
        commands_.playAt = position_.load();
    }
    sendChangeMessage();
}

void AppModel::timerCallback()
{
    auto version = rebuilder_.take();
    if (!version)
        return;
    report_ = version->report;
    ++reportRevision_;

    // Tell the player what stayed the same, compared with what it has.
    const bool same = built_ == version->song;
    size_t ym = 0, pcm = 0;
    if (built_ && !same)
        firstDifference(*built_, *version->song, ym, pcm);
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.song = version->song;
        commands_.snapshots = version->snapshots;
        commands_.ymUnchanged = ym;
        commands_.pcmUnchanged = pcm;
        commands_.fresh = built_ == nullptr;
        commands_.newSong = !same;
        commands_.sameSong = same;
    }
    built_ = version->song;
    keep_.push_back(std::move(*version));
    while (keep_.size() > 8)
        keep_.pop_front();
    sendChangeMessage();
}

void AppModel::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate_ = device->getCurrentSampleRate();
    player_.prepare(sampleRate_);
}

void AppModel::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                                int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    {
        const juce::SpinLock::ScopedTryLockType lock(commandLock_);
        if (lock.isLocked())
        {
            Commands& c = commands_;
            if (c.newSong || c.sameSong)
            {
                if (c.fresh || !player_.playing())
                    player_.setSong(c.song, c.snapshots);
                else if (c.newSong)
                    waitingForSnapshots_ = !player_.replaceSong(c.song, c.snapshots, c.ymUnchanged, c.pcmUnchanged);
                else
                {
                    player_.updateSnapshots(c.snapshots);
                    if (waitingForSnapshots_)
                        waitingForSnapshots_ = !player_.play(player_.position());
                }
                // The command keeps no reference: the model holds the version.
                c.song.reset();
                c.snapshots.reset();
                c.newSong = c.sameSong = c.fresh = false;
            }
            if (c.stop)
            {
                player_.stop();
                playing_.store(false);
                c.stop = false;
            }
            if (c.play)
            {
                if (player_.play(c.playAt))
                    playing_.store(true);
                c.play = false;
            }
        }
    }

    float scratch[2][1024];
    float* left = numOutputs > 0 && outputs[0] ? outputs[0] : scratch[0];
    float* right = numOutputs > 1 && outputs[1] ? outputs[1] : scratch[1];
    numSamples = std::min(numSamples, 1024);
    player_.render(left, right, numSamples);
    if (!monitor_.load())
    {
        juce::FloatVectorOperations::clear(left, numSamples);
        juce::FloatVectorOperations::clear(right, numSamples);
    }
    for (int ch = 2; ch < numOutputs; ++ch)
        if (outputs[ch])
            juce::FloatVectorOperations::clear(outputs[ch], numSamples);

    if (player_.playing())
    {
        position_.store(player_.position());
        positionStamp_.store(juce::Time::getMillisecondCounterHiRes());
        // Stop after the song's end and its releases.
        if (const VgmSong* song = player_.song(); song && player_.position() > songEnd(*song))
        {
            player_.stop();
            playing_.store(false);
        }
    }
    else
        playing_.store(false);
}

} // namespace opm86
