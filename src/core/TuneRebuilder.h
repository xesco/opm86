#pragma once

#include "SongPlayer.h"
#include "Tune.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace opm86
{

// The gains a tune's mute, solo and volume give the chip channels, over
// time, for a song built without them: each note's channel, as the report
// has the build put it, from the note's start at its track's volume, or
// silent when the note is not heard.
std::shared_ptr<ChannelMix> mixOf(const Tune& tune, const BuildReport& report);

// Some notes played alone: a mix that silences every other note, whatever the
// mute and solo, and each chosen note at its end, with no release; and the
// song time from the first note's start to the last one's end.
struct NoteMix
{
    std::shared_ptr<ChannelMix> mix;
    double from = 0, to = 0;
};
NoteMix noteMixOf(const Tune& tune, const BuildReport& report, const std::set<std::pair<int, int>>& notes); // each a track and note id

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
// change, then makes snapshots up to just past the given play position and
// publishes the version, so playback can switch to it at once; then it makes
// the rest and publishes the version again, complete.
class TuneRebuilder
{
public:
    TuneRebuilder();
    ~TuneRebuilder();

    // Message thread. position is the song time playback is at, in VGM samples.
    void request(const Tune& tune, double position);
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

    // Worker thread only: the last build, whose snapshots the next one reuses.
    std::shared_ptr<const VgmSong> lastSong_;
    std::shared_ptr<const SongSnapshots> lastSnapshots_;

    std::thread worker_;
};

} // namespace opm86
