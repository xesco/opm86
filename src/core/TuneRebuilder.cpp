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
            // An FM note also takes its voice's own volume, by the channel the game played it on.
            float gain = volume;
            if (!drum)
            {
                const float voice = track.voiceVolume[static_cast<size_t>(tune.originalChannel(track, note) & 7)];
                gain = voice <= TuneTrack::kSilent ? 0.0f : gain * std::pow(10.0f, std::min(voice, TuneTrack::kMaxVolume) / 20.0f);
            }
            if (slot < mix->changes.size())
                mix->changes[slot].push_back({ static_cast<double>(note.start), tune.audible(track, note) ? gain : 0.0f });
        }
    }
    for (auto& v : mix->changes)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return mix;
}

std::shared_ptr<ChannelMix> ownerOf(const Tune& tune, const BuildReport& report, int track)
{
    auto owner = std::make_shared<ChannelMix>();
    for (size_t t = 0; t < tune.tracks.size(); ++t)
    {
        const TuneTrack& tr = tune.tracks[t];
        if (tr.kind != TuneTrack::Kind::Fm)
            continue;
        for (size_t n = 0; n < tr.notes.size(); ++n)
        {
            int ch = t < report.channels.size() && n < report.channels[t].size() ? report.channels[t][n] : -1;
            if (ch < 0)
                ch = tune.originalChannel(tr, tr.notes[n]);
            if (ch >= 0 && ch < 8)
                owner->changes[static_cast<size_t>(ch)].push_back({ static_cast<double>(tr.notes[n].start), static_cast<int>(t) == track ? 1.0f : 0.0f });
        }
    }
    for (auto& v : owner->changes)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return owner;
}

std::shared_ptr<PatchEdits> patchEditsOf(const Tune& tune, const BuildReport& report, int mutedTrack, uint8_t muted)
{
    const bool any = std::any_of(tune.tracks.begin(), tune.tracks.end(), [](const TuneTrack& t) {
        return t.patch.has_value() || std::any_of(t.voiceSides.begin(), t.voiceSides.end(), [](uint8_t s) { return s != 0; });
    });
    const bool audition = mutedTrack >= 0 && static_cast<size_t>(mutedTrack) < tune.tracks.size() && (muted & 0xF) != 0;
    if (!any && !tune.chip.any() && !audition)
        return nullptr;
    auto edits = std::make_shared<PatchEdits>();
    edits->chip = tune.chip;
    if (audition)
        edits->mutedTrack = mutedTrack, edits->muted = static_cast<uint8_t>(muted & 0xF);
    edits->tracks.resize(tune.tracks.size());
    for (size_t t = 0; t < tune.tracks.size(); ++t)
    {
        const TuneTrack& tr = tune.tracks[t];
        if (tr.kind != TuneTrack::Kind::Fm)
            continue;
        const bool sides = std::any_of(tr.voiceSides.begin(), tr.voiceSides.end(), [](uint8_t s) { return s != 0; });
        if (tr.patch || sides)
        {
            const FmPatch& original = tune.basePatch(tr);
            edits->tracks[t] = PatchEdits::Edit { tr.patch ? *tr.patch : original, original, tr.voiceSides };
        }
        for (size_t n = 0; n < tr.notes.size(); ++n)
        {
            int ch = t < report.channels.size() && n < report.channels[t].size() ? report.channels[t][n] : -1;
            if (ch < 0)
                ch = tune.originalChannel(tr, tr.notes[n]);
            if (ch >= 0 && ch < 8)
                edits->owner[static_cast<size_t>(ch)].push_back({ static_cast<double>(tune.firstWrite(tr, tr.notes[n], ch)),
                                                                  static_cast<int>(t) | (tr.notes[n].pan ? PatchEdits::kNotePan : 0) });
        }
    }
    for (auto& v : edits->owner)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return edits;
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

uint64_t TuneRebuilder::request(const Tune& tune, double position)
{
    uint64_t number = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_ = tune;
        position_ = position;
        number = ++requested_;
    }
    wake_.notify_all();
    return number;
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

        // First up to just past the playhead, so playback can switch now: again
        // while it has moved on as they were made.
        const double end = songEnd(*song);
        double reached = -1.0;
        for (;;)
        {
            const double want = std::min(end, std::max(position, playhead_.load()) + 2.0 * kVgmRate);
            if (want <= reached)
                break;
            extendSnapshots(*song, *snapshots, want);
            reached = want;
        }
        // A newer edit came while this was built: this version is never heard.
        if (superseded())
            continue;
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
