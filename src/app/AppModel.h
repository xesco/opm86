#pragma once

#include "PreviewVoice.h"
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
#include <optional>

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
    void newProject(); // an empty song to write, at 120 beats a minute in 4/4 (see Tune::empty)
    // A project: its rip, then its tracks as saved (see project::read).
    bool openProject(const juce::File& file, juce::String& error);
    bool saveProject(const juce::File& file, juce::String& error);
    bool exportJson(const juce::File& file, juce::String& error); // the project as JSON; what is open stays as it is
    const juce::File& imported() const { return imported_; }      // the JSON the song came from, saved to no project yet
    const juce::File& projectFile() const { return project_; } // the project open now; none for a rip opened alone
    // What the song is called: its project's file, else its rip's, else Untitled.
    juce::String name() const
    {
        return project_ != juce::File()    ? project_.getFileName()
               : imported_ != juce::File() ? imported_.getFileName()
               : file_ != juce::File()     ? file_.getFileName()
                                           : juce::String("Untitled");
    }
    // The song's name without its file's extension, for the files exported from it.
    juce::String title() const
    {
        const juce::String n = name();
        return n.containsChar('.') ? n.upToLastOccurrenceOf(".", false, false) : n;
    }
    bool unsaved() const { return tune_ != nullptr && (revision_ != savedRevision_ || loopChanged_); } // changed since opened or saved
    bool hasTune() const { return tune_ != nullptr; }
    const Tune& tune() const { return *tune_; }
    const juce::File& file() const { return file_; } // the rip open now; none for a song written from empty
    int revision() const { return revision_; } // changes with every new tune or edit

    // Per track, per note: the chip channel of the newest build, -1 for a conflict.
    const BuildReport& report() const { return report_; }
    int reportRevision() const { return reportRevision_; }

    // Per chip channel, 8 FM then 16 PCM: the loudest output since the last call.
    std::array<float, SongPlayer::kMeterChannels> takePeaks() { return player_.takePeaks(); }
    bool ready() const { return built_ != nullptr; } // a build exists to play

    // Change the tune, as one step of undo; with newStep false, as part of the
    // step before (a knob turn is one step). Its samples change with it, in
    // the same step. Playing or stopped alike: the tune shows the edit at
    // once, and an edit that needs a new song has it built in the background;
    // playback then switches to it at the playhead with a crossfade (see
    // SongPlayer::replaceSong). Returns false with no tune.
    bool edit(const juce::String& name, const std::function<void(Tune&)>& change, bool newStep = true);
    // Undo, or redo, one step, playing or stopped alike, as an edit goes.
    bool undoStep(bool redo = false);
    juce::UndoManager& undo() { return undo_; }
    std::function<void(const juce::String&)> onNotice; // a word for the status line, as what a paste left out

    // The song's structure (see Tune::grid), each change a step of undo, or
    // with newStep false part of the step before, as a drag is. A change of
    // tempo moves a song's written notes with their beats, and playback
    // carries on from the same beat.
    bool setTempo(double bpm, bool newStep = true);
    bool setBeatsPerBar(int beats, bool newStep = true);
    bool barOneHere(); // bar 1 starts at the playhead, which is then on a downbeat

    // The loop (see Tune::loop): set at once, for the player too, and saved
    // with the project, but not a step of undo.
    const LoopRegion& loop() const { return tune_->loop; }
    void setLoop(const LoopRegion& loop);

    void play();
    void stop();
    bool playing() const { return playing_.load(); }
    double position() const { return position_.load(); } // song time, VGM samples
    // The position now, between audio blocks, for a playhead that moves smoothly.
    double displayPosition() const;
    void setPosition(double songTime);
    // Stopped, move to a song time and let a moment of it sound, as a jog
    // wheel does; a run of these sounds on while they come.
    void scrub(double songTime);
    void setMonitor(bool on) { monitor_.store(on); } // off: plays and meters, but outputs silence
    void setSpeed(double speed) { speed_.store(speed); } // tempo, 1 as made; the pitch stays
    double speed() const { return speed_.load(); }
    // The patch window's probe on some FM voices of a track (bit n for voice
    // n, 0 for none): their output while they play that track's notes, muted
    // or not, summed, and the loudest one's operators' envelopes, from the
    // audio thread.
    // Stopped, the probe reads the preview instead, whatever it plays.
    void setProbe(int voices, int track);
    void probeOutput(float* out, int count) const
    {
        if (playing())
            player_.probeOutput(out, count);
        else
            preview_.probeOutput(out, count);
    }
    void probeEnvelopes(float* levels, int* stages) const
    {
        if (playing())
            player_.probeEnvelopes(levels, stages);
        else
            preview_.probeEnvelopes(levels, stages);
    }

    // Audition: an instrument heard with the song stopped, on a chip of its
    // own (see PreviewVoice), through the same output. While playing, a key
    // down or a note does nothing. A test keyboard sets the instrument with
    // previewPatch and holds keys with previewKey; previewNote plays a note
    // of an FM track at its pitch, with its fine tune, velocity and pan and
    // the track's instrument as edited, for its length up to kPreviewLength,
    // letting go of any key held before.
    // The instrument plays with the chip's LFO as it is now (see chipNow).
    // previewPatch's track is the one whose audition mutes it takes (see
    // setAudition), or -1 for none.
    static constexpr double kPreviewLength = 0.6; // seconds
    void previewPatch(const FmPatch& patch, int track);
    void previewKey(int key, bool on);
    void previewNote(int track, const TuneNote& note);
    void previewOff(); // every key let go
    // A drum track's sample, as edited, played at a MIDI key with the song
    // stopped, on the preview's SegaPCM (see PreviewVoice::pcmOn): 60 at its
    // pitch. A key down starts it again; a key up stops it.
    void previewSample(int track, int key, bool on);
    // A drum hit as the build triggers it, its expression in its step and
    // volumes (see Tune::hitVoice), for its length up to kPreviewLength.
    void previewHit(int track, const TuneNote& note);
    bool previewingSample() const { return !playing() && preview_.pcmSounding(); }
    // The instrument an FM track plays: as edited, else as imported.
    const FmPatch& editedPatch(int track) const;

    // Audition mutes: one track's operators (bit i for op[i]) silenced in
    // playback and in its previews, to hear the others alone. They are not
    // an edit: not saved, not a step of undo, and the instrument stays as it
    // is. A new song, or setAudition(-1, 0), clears them.
    void setAudition(int track, uint8_t operators);
    uint8_t audition(int track) const { return track >= 0 && track == auditionTrack_ ? auditionOps_ : 0; }

    // The chip's LFO and noise as the song has written them at the playhead,
    // following playback, whatever the chip's edits make of them; and as
    // heard, those edits over them.
    ChipState gameChip() const;
    ChipState chipNow() const { return tune_ ? tune_->chip.over(gameChip()) : gameChip(); }
    // Stopped, whether a preview sounds, and its lowest key (see PreviewVoice::pitch).
    bool previewing() const { return !playing() && preview_.sounding(); }
    int previewPitch() const { return preview_.pitch(); }

    // Export: the song as it sounds here, from a copy of the tune, so it
    // works stopped or playing. A VGM has every edit and the mix in its
    // writes (see bakeSong), gzipped as a .vgz unless the file ends in .vgm;
    // a MIDI file has the notes (see exportMidi).
    bool exportVgm(const juce::File& file, juce::String& error) const;
    bool exportMidi(const juce::File& file, juce::String& error) const;
    // A WAV file rendered as playback sounds (see TuneRenderer) on a thread of
    // its own, one at a time; the file appears when the render is complete.
    struct WavOptions
    {
        double rate = 44100.0;
        int bits = 16;
        bool loopOnly = false; // only the loop, once, when it is on
    };
    bool exportWav(const juce::File& file, const WavOptions& options);
    bool exporting() const { return wavExport_ != nullptr; }
    double exportProgress() const; // 0 to 1
    juce::File exportFile() const; // the file the render writes
    void cancelExport();           // the render stops, and no file is written
    // When the render has ended: what came of it, once; else nothing.
    std::optional<juce::String> takeExportResult();

private:
    class TracksChange;
    class WavExport;
    struct TuneState; // what a step of undo changes: the tracks, the chip's edits, the grid and the samples
    friend class TracksChange;

    void take(std::unique_ptr<Tune> tune, const juce::File& rip); // a new tune, opened as it is
    void setTracks(const TuneState& state);
    // The loop to the player; while a tempo change waits for its build, with
    // that build (see Remap), as the loop is in its song's time.
    void sendLoop();
    void loopCommand(const LoopRegion& loop); // with commandLock_ held
    // Where the playhead is in the song the newest request builds: moved by
    // the tempo changes waiting for their builds, then by next.
    double playheadAhead(const TimeMap& next = {}) const;
    // The build's request number (see TuneRebuilder::request); next is how
    // this request moves song time, while playing.
    uint64_t rebuild(const TimeMap& next = {});
    // The tracks' mute, solo, volume, edited instruments and chip for the
    // player, by the newest build's channels; kept here, so the audio thread
    // never frees one.
    std::pair<std::shared_ptr<const ChannelMix>, std::shared_ptr<const PatchEdits>> mixNow();
    void sendMix(); // sends them to the player
    void sendPreviewSong(); // the preview's chips, its SegaPCM reading the tune's ROM
    // Key -1 with on false lets go of every key. A key down can carry a
    // note's expression (see PreviewVoice::noteOn).
    void sendPreviewKey(int key, bool on, double seconds, int fine = 0, int quieter = 0, uint8_t sides = 0xC0);
    void timerCallback() override;

    // AudioIODeviceCallback (audio thread)
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;

    std::unique_ptr<Tune> tune_;
    std::unique_ptr<WavExport> wavExport_; // the WAV render under way, or none
    juce::File file_;
    juce::File project_;
    juce::File imported_;
    int savedRevision_ = 0;
    int revision_ = 0;
    bool loopChanged_ = false; // the loop moved since opened or saved
    juce::UndoManager undo_ { 200000 };
    TuneRebuilder rebuilder_;
    BuildReport report_;
    int reportRevision_ = 0;
    std::shared_ptr<const VgmSong> built_;  // newest build sent to the player
    std::deque<TuneVersion> keep_;          // recent versions, freed here, never on the audio thread
    // A tempo change made while playing: the request that builds it, how it
    // moves song time, and the loop in the new time, which goes to the
    // player with the build.
    struct Remap
    {
        uint64_t request = 0;
        TimeMap map;
        std::optional<LoopRegion> loop;
    };
    std::deque<Remap> remaps_; // in order of request
    std::deque<std::shared_ptr<const ChannelMix>> mixes_; // and recent mixes and probe owners
    int probeVoices_ = 0, probeTrack_ = -1;                 // what the probe follows
    std::deque<std::shared_ptr<const PatchEdits>> edits_;   // recent instrument edits, freed here
    int auditionTrack_ = -1;                                // the track with operators muted to audition
    uint8_t auditionOps_ = 0;                               // and those operators, bit i for op[i]
    int previewTrack_ = -1;                                 // the track whose instrument the preview plays now
    mutable std::shared_ptr<const VgmSong> chipSong_;       // the build gameChip last read stopped
    mutable double chipAt_ = -1.0;                          // and the place
    mutable ChipState chipState_;                           // and what it read there

    juce::AudioDeviceManager devices_;
    SongPlayer player_; // audio thread
    PreviewVoice preview_; // audio thread
    std::deque<std::shared_ptr<const FmPatch>> previewPatches_; // recent instruments sent to the preview, freed here
    std::deque<std::shared_ptr<const VgmSong>> previewSongs_;   // and the songs whose ROM its SegaPCM reads

    // Commands for the audio thread, taken at the start of a block.
    struct Commands
    {
        std::shared_ptr<const VgmSong> song;
        std::shared_ptr<const SongSnapshots> snapshots;
        std::shared_ptr<const ChannelMix> mix;
        std::shared_ptr<const ChannelMix> probeOwner;
        std::shared_ptr<const PatchEdits> edits; // the edited instruments, or none
        bool newEdits = false;
        int probeVoices = 0;
        bool probe = false; // the probe changed
        size_t ymUnchanged = 0, pcmUnchanged = 0;
        TimeMap timeMap; // how the playhead moves into the new song
        bool newSong = false, sameSong = false, fresh = false;
        bool play = false, stop = false;
        bool scrub = false; // the play is a scrub's moment
        double playAt = 0;
        bool loop = false; // the loop changed
        double loopStart = 0, loopEnd = 0; // song times; the end at or before the start for none
        // The preview: the song's chip when a new song opens, the instrument
        // from now on, and the keys down and up since the last block, in order.
        bool previewChip = false;
        double previewRate = 0, previewShift = 0;
        std::shared_ptr<const VgmSong> previewSong; // with previewChip, the song for the SegaPCM
        struct PreviewPcm
        {
            bool on = false;
            PcmVoice voice; // a sample started, as a written hit starts it
            double seconds = 0; // how long it plays; 0 until stopped, or a one-shot's end
        };
        std::array<PreviewPcm, 8> previewPcm {}; // samples started and stopped since the last block, in order
        size_t previewPcmCount = 0;
        std::shared_ptr<const FmPatch> previewPatch;
        bool previewMute = false; // the audition mutes changed
        uint8_t previewMuted = 0;
        struct PreviewKey
        {
            int key = -1;
            bool on = false;
            double seconds = 0; // a key down's length; 0 held until its key up
            int fine = 0, quieter = 0; // the note's expression (see PreviewVoice::noteOn)
            uint8_t sides = 0xC0;
        };
        std::array<PreviewKey, 32> previewKeys {};
        size_t previewCount = 0;
    };
    juce::SpinLock commandLock_;
    Commands commands_;
    int scrubLeft_ = 0;                // audio thread: output samples a scrub still sounds

    std::atomic<bool> playing_ { false };
    std::atomic<bool> monitor_ { true };
    std::atomic<double> speed_ { 1.0 };
    std::atomic<double> position_ { 0.0 };
    std::atomic<double> positionStamp_ { 0.0 }; // when position_ was set, in ms
    double sampleRate_ = 48000.0;
};

} // namespace opm86
