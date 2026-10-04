#pragma once

#include "SongPlayer.h"
#include "Tune.h"
#include "TuneRebuilder.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>

namespace opm86
{

// The tracker's state: the open tune and its edits, undo, the background
// builds, and exact playback on the audio device. Message thread, except
// where marked.
class AppModel : public juce::ChangeBroadcaster, private juce::Timer, private juce::AudioIODeviceCallback
{
public:
    AppModel();
    ~AppModel() override;

    bool open(const juce::File& file, juce::String& error);
    bool hasTune() const { return tune_ != nullptr; }
    const Tune& tune() const { return *tune_; }
    const juce::File& file() const { return file_; } // the rip open now
    int revision() const { return revision_; } // changes with every new tune or edit

    // Per track, per note: the chip channel of the newest build, -1 for a conflict.
    const BuildReport& report() const { return report_; }
    int reportRevision() const { return reportRevision_; }

    // Per chip channel, 8 FM then 16 PCM: the loudest output since the last call.
    std::array<float, SongPlayer::kMeterChannels> takePeaks() { return player_.takePeaks(); }
    bool ready() const { return built_ != nullptr; } // a build exists to play

    // Change the tune, as one step of undo; with newStep false, as part of the
    // step before (a knob turn is one step).
    void edit(const juce::String& name, const std::function<void(Tune&)>& change, bool newStep = true);
    juce::UndoManager& undo() { return undo_; }

    void play();
    void stop();
    bool playing() const { return playing_.load(); }
    double position() const { return position_.load(); } // song time, VGM samples
    // The position now, between audio blocks, for a playhead that moves smoothly.
    double displayPosition() const;
    void setPosition(double songTime);
    void setMonitor(bool on) { monitor_.store(on); } // off: plays and meters, but outputs silence

private:
    class TracksChange;
    friend class TracksChange;

    void setTracks(const std::vector<TuneTrack>& tracks);
    void rebuild();
    void timerCallback() override;

    // AudioIODeviceCallback (audio thread)
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;

    std::unique_ptr<Tune> tune_;
    juce::File file_;
    int revision_ = 0;
    juce::UndoManager undo_ { 200000 };
    TuneRebuilder rebuilder_;
    BuildReport report_;
    int reportRevision_ = 0;
    std::shared_ptr<const VgmSong> built_;  // newest build sent to the player
    std::deque<TuneVersion> keep_;          // recent versions, freed here, never on the audio thread

    juce::AudioDeviceManager devices_;
    SongPlayer player_; // audio thread

    // Commands for the audio thread, taken at the start of a block.
    struct Commands
    {
        std::shared_ptr<const VgmSong> song;
        std::shared_ptr<const SongSnapshots> snapshots;
        size_t ymUnchanged = 0, pcmUnchanged = 0;
        bool newSong = false, sameSong = false, fresh = false;
        bool play = false, stop = false;
        double playAt = 0;
    };
    juce::SpinLock commandLock_;
    Commands commands_;
    bool waitingForSnapshots_ = false; // audio thread: a switch that must still jump

    std::atomic<bool> playing_ { false };
    std::atomic<bool> monitor_ { true };
    std::atomic<double> position_ { 0.0 };
    std::atomic<double> positionStamp_ { 0.0 }; // when position_ was set, in ms
    double sampleRate_ = 48000.0;
};

} // namespace opm86
