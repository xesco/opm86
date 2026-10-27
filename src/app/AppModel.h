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
#include <set>

namespace opm86
{

// The player's state: the open tune and its edits, undo, the background
// builds, and exact playback on the audio device. Message thread, except
// where marked.
class AppModel : public juce::ChangeBroadcaster, private juce::Timer, private juce::AudioIODeviceCallback
{
public:
    AppModel();
    ~AppModel() override;

    bool open(const juce::File& file, juce::String& error);
    void close(); // stops, and leaves no song open
    bool hasTune() const { return tune_ != nullptr; }
    const Tune& tune() const { return *tune_; }
    const juce::File& file() const { return file_; } // the rip open now
    int revision() const { return revision_; } // changes with every new tune or edit

    // Per track, per note: the chip channel of the newest build, -1 for a conflict.
    const BuildReport& report() const { return report_; }
    int reportRevision() const { return reportRevision_; }

    // Per chip channel, 8 FM then 16 PCM: the loudest output since the last call.
    std::array<float, SongPlayer::kMeterChannels> takePeaks() { return player_.takePeaks(); }
    ChipState chipState() const { return player_.chipState(); } // the chips as they play, from the audio thread
    std::array<float, 4> takeChipPeaks() { return player_.takeChipPeaks(); } // each chip's output: FM left, right, PCM left, right
    bool ready() const { return built_ != nullptr; } // a build exists to play

    // Change the tune, as one step of undo; with newStep false, as part of the
    // step before (a knob turn is one step).
    void edit(const juce::String& name, const std::function<void(Tune&)>& change, bool newStep = true);
    juce::UndoManager& undo() { return undo_; }

    void play();
    void stop();
    bool playing() const { return playing_.load(); }
    double position() const { return position_.load(); } // song time, VGM samples
    bool takeEnded() { return ended_.exchange(false); } // whether playing stopped at the song's end since the last call
    // The position now, between audio blocks, for a playhead that moves smoothly.
    double displayPosition() const;
    void setPosition(double songTime);
    // Stopped, move to a song time and let a moment of it sound; a run of
    // these sounds on while they come.
    void scrub(double songTime);
    void setMonitor(bool on) { monitor_.store(on); } // off: plays and meters, but outputs silence
    void setSpeed(double speed) { speed_.store(speed); } // tempo, 1 as made; the pitch stays
    // The whole output, in dB; this app's sound only, under the system volume.
    void setMasterVolume(float dB) { master_.store(dB <= TuneTrack::kSilent ? 0.0f : juce::Decibels::decibelsToGain(dB)); }
    double speed() const { return speed_.load(); }

    // A section of the song, in VGM samples: the playhead goes to its start,
    // and playing from inside it stops at its end, or loops. Moving the
    // playhead out of it clears it.
    void setSelection(double start, double end, bool keepPlayhead = false); // keepPlayhead: unless it falls outside
    void clearSelection();
    bool hasSelection() const { return selectionEnd_.load() > selectionStart_.load(); }
    double selectionStart() const { return selectionStart_.load(); }
    double selectionEnd() const { return selectionEnd_.load(); }
    // Play only some notes, each a track index and note id, from the first
    // note's start to the last one's end, each cut at its own. Then playing stops,
    // the mix goes back to what it was, and the playhead to the first note's start.
    void playNotes(const std::set<std::pair<int, int>>& notes);
    const std::set<std::pair<int, int>>& playingNotes() const { return playingNotes_; } // empty while the song plays
    // The notes selected in the tracker, each a track index and note id,
    // which Space and the play button play alone; with none, the song plays.
    void setSelectedNotes(const std::set<std::pair<int, int>>& notes) { selectedNotes_ = notes; }
    const std::set<std::pair<int, int>>& selectedNotes() const { return selectedNotes_; }
    void playSelected();
    // Loop: the selection over and over, or the whole song without one.
    void setLoop(bool on) { loop_.store(on); }
    bool loop() const { return loop_.load(); }

private:
    class TracksChange;
    friend class TracksChange;

    void setTracks(const std::vector<TuneTrack>& tracks);
    void rebuild();
    void sendMix(); // sends the tracks' mute, solo and volume to the player
    void endNotes(); // after playNotes: the mix back, and the playhead to the notes' start
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
    std::atomic<double> selectionStart_ { 0.0 }, selectionEnd_ { 0.0 }; // empty when the end is not past the start
    std::atomic<bool> loop_ { false };
    std::deque<TuneVersion> keep_;          // recent versions, freed here, never on the audio thread
    std::deque<std::shared_ptr<const ChannelMix>> mixes_; // and recent mixes

    juce::AudioDeviceManager devices_;
    SongPlayer player_; // audio thread

    // Commands for the audio thread, taken at the start of a block.
    struct Commands
    {
        std::shared_ptr<const VgmSong> song;
        std::shared_ptr<const SongSnapshots> snapshots;
        std::shared_ptr<const ChannelMix> mix;
        size_t ymUnchanged = 0, pcmUnchanged = 0;
        bool newSong = false, sameSong = false, fresh = false;
        bool play = false, stop = false;
        bool scrub = false; // the play is a scrub's moment
        double playAt = 0;
        double notesEnd = -1; // the play is of notes alone, which stops here
    };
    juce::SpinLock commandLock_;
    Commands commands_;
    bool waitingForSnapshots_ = false; // audio thread: a switch that must still jump
    int scrubLeft_ = 0;                // audio thread: output samples a scrub still sounds
    double notesEnd_ = -1;             // audio thread: where notes played alone stop; -1 for the song
    std::set<std::pair<int, int>> playingNotes_, selectedNotes_;
    double notesFrom_ = 0;             // the first note's start, where the playhead goes after them
    std::atomic<bool> notesDone_ { false }; // set where the notes' playing ends

    std::atomic<bool> playing_ { false };
    std::atomic<bool> ended_ { false }; // set where playing stops at the song's end
    std::atomic<bool> monitor_ { true };
    std::atomic<double> speed_ { 1.0 };
    std::atomic<float> master_ { 1.0f }; // the master volume as a gain
    std::atomic<double> position_ { 0.0 };
    std::atomic<double> positionStamp_ { 0.0 }; // when position_ was set, in ms
    double sampleRate_ = 48000.0;
};

} // namespace opm86
