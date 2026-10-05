#pragma once

#include "SongPlayer.h"
#include "Tune.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace opm86
{

// The gains a tune's mute, solo and volume give the chip channels, over
// time, for a song built without them: each note's channel, as the report
// has the build put it, from the note's start at its track's volume, or
// silent when the note is not heard.
std::shared_ptr<ChannelMix> mixOf(const Tune& tune, const BuildReport& report);

// When each FM chip channel plays one track's notes, over time, as the
// report has the build put them: gain 1 from the start of each of its notes
// and 0 from the start of any other track's, for a display that follows one
// track (SongPlayer::setProbe).
std::shared_ptr<ChannelMix> ownerOf(const Tune& tune, const BuildReport& report, int track);

// The tune's edited instruments, for the player (SongPlayer::setPatchEdits):
// which track each FM channel plays when, as the report has the build put
// the notes, each edited track's patch against the one imported, and the
// chip's edited LFO and noise. With mutedTrack, that track's operators in
// muted are left out of its key ons to audition the others (see
// PatchEdits::muted). None when nothing is edited or muted.
std::shared_ptr<PatchEdits> patchEditsOf(const Tune& tune, const BuildReport& report, int mutedTrack = -1, uint8_t muted = 0);

// A built version of a tune, ready for the player.
struct TuneVersion
{
    uint64_t version = 0;   // the request it answers
    std::shared_ptr<const VgmSong> song;
    std::shared_ptr<const SongSnapshots> snapshots;
    bool complete = false;  // the snapshots reach the end of the song
    BuildReport report;
};

// The index of the first write that differs between two versions of a song,
// for each chip: the writes before it are the same in both.
void firstDifference(const VgmSong& a, const VgmSong& b, size_t& ym, size_t& pcm);

// Builds tunes on a background thread, newest request first: a request
// replaces one still waiting, and a newer request stops work on an older one.
// Each build reuses the snapshots of the build before it up to the first
// change, then makes snapshots up to just past the playhead and publishes
// the version, so playback can switch to it at once; then it makes the rest
// and publishes the version again, complete. A build that a newer request
// overtakes before it is published is dropped, so a run of quick edits
// publishes only the newest.
class TuneRebuilder
{
public:
    TuneRebuilder();
    ~TuneRebuilder();

    // Message thread. position is the song time playback is at, in VGM
    // samples. Returns the request's number, which its version carries.
    uint64_t request(const Tune& tune, double position);
    // Where playback is now, in VGM samples, or below 0 when it is stopped:
    // the first snapshots reach past it, as it moves on while they are made.
    void setPlayhead(double songTime) { playhead_.store(songTime); }
    // Build without mute, solo and volume, which the player then applies (see mixOf).
    void setMixInPlayer(bool inPlayer) { mixInPlayer_.store(inPlayer); }

    // The newest version published since the last call, if any.
    std::optional<TuneVersion> take();

private:
    void run();
    bool superseded();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Tune> pending_;
    double position_ = 0;
    uint64_t requested_ = 0;
    std::optional<TuneVersion> ready_;
    bool quit_ = false;
    std::atomic<bool> mixInPlayer_ { false };
    std::atomic<double> playhead_ { -1.0 };

    // Worker thread only: the last build, whose snapshots the next one reuses.
    std::shared_ptr<const VgmSong> lastSong_;
    std::shared_ptr<const SongSnapshots> lastSnapshots_;

    std::thread worker_;
};

} // namespace opm86
