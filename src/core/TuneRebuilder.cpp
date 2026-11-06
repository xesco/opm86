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

namespace
{

// The chip channel a note plays on: the report's, or with no free channel
// the one it had in the game, as the build does. FM channels are 0-7 and
// PCM channels 8 on.
size_t mixSlot(const Tune& tune, const BuildReport& report, size_t t, size_t n)
{
    const TuneTrack& track = tune.tracks[t];
    int ch = t < report.channels.size() && n < report.channels[t].size() ? report.channels[t][n] : -1;
    if (ch < 0)
        ch = tune.originalChannel(track, track.notes[n]);
    return static_cast<size_t>(ch + (track.kind == TuneTrack::Kind::Drum ? 8 : 0));
}

// A track's volume as a gain, as the build would set it, up to the boost's limit.
float volumeGain(const TuneTrack& track)
{
    return track.volume <= TuneTrack::kSilent ? 0.0f : std::pow(10.0f, std::min(track.volume, TuneTrack::kMaxVolume) / 20.0f);
}

void sortMix(ChannelMix& mix)
{
    for (auto& v : mix.changes)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
}

} // namespace

std::shared_ptr<ChannelMix> mixOf(const Tune& tune, const BuildReport& report)
{
    auto mix = std::make_shared<ChannelMix>();
    for (size_t t = 0; t < tune.tracks.size(); ++t)
    {
        const TuneTrack& track = tune.tracks[t];
        const float volume = volumeGain(track);
        for (size_t n = 0; n < track.notes.size(); ++n)
        {
            const size_t slot = mixSlot(tune, report, t, n);
            if (slot < mix->changes.size())
                mix->changes[slot].push_back({ static_cast<double>(track.notes[n].start), tune.audible(track, track.notes[n]) ? volume : 0.0f });
        }
    }
    sortMix(*mix);
    return mix;
}

NoteMix noteMixOf(const Tune& tune, const BuildReport& report, const std::set<std::pair<int, int>>& notes)
{
    NoteMix out;
    out.mix = std::make_shared<ChannelMix>();
    ChannelMix& mix = *out.mix;
    // Every channel silent from the start, and each chosen note silent from
    // its end; then each note's own gain from its start, which comes after a
    // chosen note's end at the same time.
    for (auto& v : mix.changes)
        v.push_back({ 0.0, 0.0f });
    for (size_t t = 0; t < tune.tracks.size(); ++t)
        for (size_t n = 0; n < tune.tracks[t].notes.size(); ++n)
        {
            const TuneNote& note = tune.tracks[t].notes[n];
            const size_t slot = mixSlot(tune, report, t, n);
            if (slot < mix.changes.size() && notes.count({ static_cast<int>(t), note.id }) > 0)
                mix.changes[slot].push_back({ static_cast<double>(note.start + note.length), 0.0f });
        }
    double from = -1;
    for (size_t t = 0; t < tune.tracks.size(); ++t)
    {
        const TuneTrack& track = tune.tracks[t];
        const float volume = volumeGain(track);
        for (size_t n = 0; n < track.notes.size(); ++n)
        {
            const TuneNote& note = track.notes[n];
            const size_t slot = mixSlot(tune, report, t, n);
            if (slot >= mix.changes.size())
                continue;
            const bool heard = notes.count({ static_cast<int>(t), note.id }) > 0;
            mix.changes[slot].push_back({ static_cast<double>(note.start), heard ? volume : 0.0f });
            if (heard)
            {
                from = from < 0 ? static_cast<double>(note.start) : std::min(from, static_cast<double>(note.start));
                out.to = std::max(out.to, static_cast<double>(note.start + note.length));
            }
        }
    }
    sortMix(mix);
    out.from = std::max(0.0, from);
    return out;
}

std::vector<float> trackLevels(const Tune& tune, const std::atomic<bool>* stop)
{
    BuildReport report;
    const auto song = tune.build(&report, false);
    // A channel belongs to its newest note's track, release included, as in
    // the mix; a silent hit silences it there, so it belongs to no one.
    ChannelOwners owners;
    for (size_t t = 0; t < tune.tracks.size(); ++t)
    {
        const TuneTrack& track = tune.tracks[t];
        for (size_t n = 0; n < track.notes.size(); ++n)
        {
            const size_t slot = mixSlot(tune, report, t, n);
            if (slot < owners.changes.size())
                owners.changes[slot].push_back({ static_cast<double>(track.notes[n].start), tune.silentHit(track, track.notes[n]) ? -1 : static_cast<int>(t) });
        }
    }
    for (auto& v : owners.changes)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.time < b.time; });

    const auto peaks = ownerPeaks(*song, owners, static_cast<int>(tune.tracks.size()), stop);
    if (peaks.size() != tune.tracks.size())
        return {};
    const float loudest = peaks.empty() ? 0.0f : *std::max_element(peaks.begin(), peaks.end());
    std::vector<float> levels(peaks.size(), TuneTrack::kSilent);
    for (size_t t = 0; t < peaks.size(); ++t)
        if (peaks[t] > 0.0f)
            levels[t] = std::max(TuneTrack::kSilent + 0.1f, 20.0f * std::log10(peaks[t] / loudest)); // sounding, so never read as silent
    return levels;
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
