#include "TuneRebuilder.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{

size_t firstDifferent(const std::vector<VgmWrite>& a, const std::vector<VgmWrite>& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i].time != b[i].time || a[i].address != b[i].address || a[i].value != b[i].value)
            return i;
    return n;
}

} // namespace

std::shared_ptr<ChannelMix> mixOf(const Tune& tune, const BuildReport& report)
{
    auto mix = std::make_shared<ChannelMix>();
    for (size_t t = 0; t < tune.tracks.size(); ++t)
    {
        const TuneTrack& track = tune.tracks[t];
        const bool drum = track.kind == TuneTrack::Kind::Drum;
        // The volume, as the build would set it, up to the boost's limit.
        const float volume = track.volume <= TuneTrack::kSilent ? 0.0f : std::pow(10.0f, std::min(track.volume, TuneTrack::kMaxVolume) / 20.0f);
        for (size_t n = 0; n < track.notes.size(); ++n)
        {
            const TuneNote& note = track.notes[n];
            // A note with no free channel keeps the one it had in the game, as the build does.
            int ch = t < report.channels.size() && n < report.channels[t].size() ? report.channels[t][n] : -1;
            if (ch < 0)
                ch = tune.originalChannel(track, note);
            const size_t slot = static_cast<size_t>(ch + (drum ? 8 : 0));
            if (slot < mix->changes.size())
                mix->changes[slot].push_back({ static_cast<double>(note.start), tune.audible(track, note) ? volume : 0.0f });
        }
    }
    for (auto& v : mix->changes)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return mix;
}

void firstDifference(const VgmSong& a, const VgmSong& b, size_t& ym, size_t& pcm)
{
    ym = firstDifferent(a.ym, b.ym);
    pcm = firstDifferent(a.pcm, b.pcm);
}

TuneRebuilder::TuneRebuilder() : worker_([this] { run(); }) {}

TuneRebuilder::~TuneRebuilder()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void TuneRebuilder::request(const Tune& tune, double position)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_ = tune;
        position_ = position;
        ++requested_;
    }
    wake_.notify_all();
}

std::optional<TuneVersion> TuneRebuilder::take()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::optional<TuneVersion> out;
    std::swap(out, ready_);
    return out;
}

bool TuneRebuilder::superseded()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return pending_.has_value() || quit_;
}

void TuneRebuilder::run()
{
    for (;;)
    {
        std::optional<Tune> tune;
        double position = 0;
        uint64_t version = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return pending_.has_value() || quit_; });
            if (quit_)
                return;
            tune = std::move(pending_);
            pending_.reset();
            position = position_;
            version = requested_;
        }

        TuneVersion v;
        v.version = version;
        auto song = tune->build(&v.report, !mixInPlayer_.load());
        std::shared_ptr<SongSnapshots> snapshots;
        if (lastSong_ && lastSnapshots_)
        {
            size_t ym = 0, pcm = 0;
            firstDifference(*lastSong_, *song, ym, pcm);
            snapshots = keepSnapshots(*lastSnapshots_, ym, pcm);
        }
        else
            snapshots = std::make_shared<SongSnapshots>();

        // First up to just past the play position, so playback can switch now.
        const double end = songEnd(*song);
        double reached = std::min(end, position + 2.0 * kVgmRate);
        extendSnapshots(*song, *snapshots, reached);
        v.song = song;
        v.snapshots = std::make_shared<SongSnapshots>(*snapshots);
        v.complete = reached >= end;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ready_ = v;
        }
        lastSong_ = song;
        lastSnapshots_ = v.snapshots;

        // Then the rest, ten seconds at a time, unless a newer edit arrives.
        while (reached < end)
        {
            if (superseded())
                break;
            reached = std::min(end, reached + 10.0 * kVgmRate);
            extendSnapshots(*song, *snapshots, reached);
            lastSnapshots_ = std::make_shared<SongSnapshots>(*snapshots);
            if (reached >= end)
            {
                v.snapshots = lastSnapshots_;
                v.complete = true;
                const std::lock_guard<std::mutex> lock(mutex_);
                ready_ = v;
            }
        }
    }
}

} // namespace opm86
