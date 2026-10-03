#pragma once

#include "SongPlayer.h"
#include "Tune.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace opm86
{

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

    // Worker thread only: the last build, whose snapshots the next one reuses.
    std::shared_ptr<const VgmSong> lastSong_;
    std::shared_ptr<const SongSnapshots> lastSnapshots_;

    std::thread worker_;
};

} // namespace opm86
