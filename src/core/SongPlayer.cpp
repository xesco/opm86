#include "SongPlayer.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{

constexpr float kChipScale = 1.0f / 32768.0f;
constexpr double kSnapshotEvery = kVgmRate / 4.0; // VGM samples

// Apply a SegaPCM write, including two markers: 0x1000 stops a looping
// sample at the song's end, 0x2000 stops any sample (a shortened drum).
void writePcm(SegaPcm& pcm, const VgmWrite& w)
{
    if (w.address & 0x2000)
    {
        const int flags = w.address & 0xFF;
        pcm.write(flags, pcm.read(flags) | 0x01);
    }
    else if (w.address & 0x1000)
    {
        const int flags = w.address & 0xFF;
        if (!(pcm.read(flags) & 0x02))
            pcm.write(flags, pcm.read(flags) | 0x01);
    }
    else
        pcm.write(w.address, w.value);
}

// The first chip sample at or after a song time.
uint64_t chipSample(double songTime, double chipRate)
{
    return static_cast<uint64_t>(std::max(0.0, std::ceil(songTime * chipRate / kVgmRate - 1e-9)));
}

// The chip sample at or before a song time, and how far past it the time is.
uint64_t chipSampleBefore(double songTime, double chipRate, double& fraction)
{
    const double x = std::max(0.0, songTime) * chipRate / kVgmRate;
    const double whole = std::floor(x + 1e-9);
    fraction = std::max(0.0, x - whole);
    return static_cast<uint64_t>(whole);
}

struct NullInterface : ymfm::ymfm_interface {};

} // namespace

void extendSnapshots(const VgmSong& song, SongSnapshots& snapshots, double until)
{
    // FM: from the last snapshot, or from a reset chip.
    NullInterface interface;
    MaskedYm2151 fm(interface);
    fm.reset();
    uint64_t k = 0;
    size_t write = 0;
    std::vector<uint8_t> state;
    if (!snapshots.fm.empty())
    {
        const auto& last = snapshots.fm.back();
        state = last.state;
        ymfm::ymfm_saved_state restore(state, false);
        fm.save_restore(restore);
        fm.invalidate_caches();
        k = last.sample;
        write = last.write;
    }
    for (;; ++k)
    {
        const double now = static_cast<double>(k) * kVgmRate / song.fmRate;
        if (now > until)
            break;
        // A snapshot holds the state before this sample's writes.
        if (snapshots.fm.empty() || now >= static_cast<double>(snapshots.fm.size()) * kSnapshotEvery)
        {
            ymfm::ymfm_saved_state saved(state, true);
            fm.save_restore(saved);
            snapshots.fm.push_back({ k, write, state });
        }
        while (write < song.ym.size() && song.ym[write].time <= now)
        {
            fm.write(0, static_cast<uint8_t>(song.ym[write].address));
            fm.write(1, song.ym[write].value);
            ++write;
        }
        ymfm::ym2151::output_data data;
        fm.generate(&data, 0);
    }

    SegaPcm pcm;
    pcm.reset();
    k = 0;
    write = 0;
    if (!snapshots.pcm.empty())
    {
        const auto& last = snapshots.pcm.back();
        pcm = last.chip;
        k = last.sample;
        write = last.write;
    }
    pcm.setRom(song.rom.data());
    for (;; ++k)
    {
        const double now = static_cast<double>(k) * kVgmRate / song.pcmRate;
        if (now > until)
            break;
        if (snapshots.pcm.empty() || now >= static_cast<double>(snapshots.pcm.size()) * kSnapshotEvery)
            snapshots.pcm.push_back({ k, write, pcm });
        while (write < song.pcm.size() && song.pcm[write].time <= now)
            writePcm(pcm, song.pcm[write++]);
        int32_t l = 0, r = 0;
        pcm.generate(l, r);
    }
}

std::shared_ptr<SongSnapshots> keepSnapshots(const SongSnapshots& previous, size_t ymUnchanged, size_t pcmUnchanged)
{
    auto out = std::make_shared<SongSnapshots>();
    for (const auto& s : previous.fm)
        if (s.write <= ymUnchanged)
            out->fm.push_back(s);
        else
            break;
    for (const auto& s : previous.pcm)
        if (s.write <= pcmUnchanged)
            out->pcm.push_back(s);
        else
            break;
    return out;
}

double songEnd(const VgmSong& song)
{
    return song.length + 3.0 * kVgmRate;
}

std::shared_ptr<SongSnapshots> makeSnapshots(const VgmSong& song)
{
    auto out = std::make_shared<SongSnapshots>();
    extendSnapshots(song, *out, songEnd(song));
    return out;
}

std::shared_ptr<SongSnapshots> makeStartSnapshot(const VgmSong& song)
{
    auto out = std::make_shared<SongSnapshots>();
    NullInterface interface;
    MaskedYm2151 fm(interface);
    fm.reset();
    std::vector<uint8_t> state;
    ymfm::ymfm_saved_state saved(state, true);
    fm.save_restore(saved);
    out->fm.push_back({ 0, 0, state });
    SegaPcm pcm;
    pcm.reset();
    pcm.setRom(song.rom.data());
    out->pcm.push_back({ 0, 0, pcm });
    return out;
}

SongPlayer::SongPlayer()
{
    state_.reserve(1 << 16);
    prepare(48000.0);
}

void SongPlayer::prepare(double hostRate)
{
    hostRate_ = hostRate;
    fade_ = static_cast<float>(1.0 / (0.003 * hostRate));
    for (Deck& d : decks_)
        d.on = false, d.gain = d.target = 0;
}

void SongPlayer::setSong(std::shared_ptr<const VgmSong> song, std::shared_ptr<const SongSnapshots> snapshots)
{
    song_ = std::move(song);
    snapshots_ = std::move(snapshots);
    for (Deck& d : decks_)
        d.on = false, d.gain = d.target = 0;
}

double SongPlayer::position() const
{
    const Deck& d = decks_[static_cast<size_t>(active_)];
    return d.on ? d.songTime : 0.0;
}

bool SongPlayer::seek(Deck& deck, double songTime)
{
    const VgmSong& song = *song_;
    deck.song = song_;
    // Before the song: silence, until the first output sample past time 0.
    int64_t silence = 0;
    double start = songTime;
    if (songTime < 0)
    {
        silence = static_cast<int64_t>(std::ceil(-songTime * hostRate_ / kVgmRate - 1e-9));
        start = songTime + static_cast<double>(silence) * kVgmRate / hostRate_;
    }
    // Output sample 0 lands exactly on the start time, between chip samples
    // if need be, as it does when the song plays straight through.
    double fmOffset = 0, pcmOffset = 0;
    const uint64_t fmTarget = chipSampleBefore(start, song.fmRate, fmOffset);
    const uint64_t pcmTarget = chipSampleBefore(start, song.pcmRate, pcmOffset);

    // The last snapshot of each chip at or before the target.
    const auto& fms = snapshots_->fm;
    const auto& pcms = snapshots_->pcm;
    auto fmIt = std::upper_bound(fms.begin(), fms.end(), fmTarget,
                                 [](uint64_t s, const SongSnapshots::Fm& f) { return s < f.sample; });
    auto pcmIt = std::upper_bound(pcms.begin(), pcms.end(), pcmTarget,
                                  [](uint64_t s, const SongSnapshots::Pcm& p) { return s < p.sample; });
    if (fmIt == fms.begin() || pcmIt == pcms.begin())
        return false;
    const auto& fmSnap = *(fmIt - 1);
    const auto& pcmSnap = *(pcmIt - 1);
    // A target past the snapshots made so far would need a long run here.
    if (fmTarget - fmSnap.sample > static_cast<uint64_t>(song.fmRate)
        || pcmTarget - pcmSnap.sample > static_cast<uint64_t>(song.pcmRate))
        return false;

    state_.assign(fmSnap.state.begin(), fmSnap.state.end());
    ymfm::ymfm_saved_state saved(state_, false);
    deck.fm.save_restore(saved);
    deck.fm.invalidate_caches();
    deck.fmSample = fmSnap.sample;
    deck.ymWrite = fmSnap.write;
    deck.pcm = pcmSnap.chip;
    deck.pcm.setRom(song.rom.data());
    deck.pcmSample = pcmSnap.sample;
    deck.pcmWrite = pcmSnap.write;

    // Run silently from the snapshot to the target, as the song would.
    ymfm::ym2151::output_data data;
    for (; deck.fmSample < fmTarget; ++deck.fmSample)
    {
        const double now = static_cast<double>(deck.fmSample) * kVgmRate / song.fmRate;
        for (; deck.ymWrite < song.ym.size() && song.ym[deck.ymWrite].time <= now; ++deck.ymWrite)
        {
            deck.fm.write(0, static_cast<uint8_t>(song.ym[deck.ymWrite].address));
            deck.fm.write(1, song.ym[deck.ymWrite].value);
        }
        deck.fm.generate(&data, 0);
    }
    for (; deck.pcmSample < pcmTarget; ++deck.pcmSample)
    {
        const double now = static_cast<double>(deck.pcmSample) * kVgmRate / song.pcmRate;
        for (; deck.pcmWrite < song.pcm.size() && song.pcm[deck.pcmWrite].time <= now; ++deck.pcmWrite)
            writePcm(deck.pcm, song.pcm[deck.pcmWrite]);
        int32_t l = 0, r = 0;
        deck.pcm.generate(l, r);
    }

    deck.fmResampler.setup(song.fmRate, hostRate_);
    deck.pcmResampler.setup(song.pcmRate, hostRate_);
    deck.fmResampler.reset(fmOffset);
    deck.pcmResampler.reset(pcmOffset);
    deck.fmFilter.setup(15000.0, song.fmRate);
    deck.pcmFilter.setup(14000.0, hostRate_);
    deck.songTime = songTime;
    deck.silence = silence;
    return true;
}

bool SongPlayer::play(double songTime)
{
    if (!song_ || !snapshots_)
        return false;
    const bool wasOn = decks_[static_cast<size_t>(active_)].on;
    const int next = wasOn ? 1 - active_ : active_;
    Deck& deck = decks_[static_cast<size_t>(next)];
    if (!seek(deck, songTime))
        return false;
    deck.on = true;
    deck.target = 1.0f;
    if (wasOn)
    {
        // Crossfade from the old position to the new one.
        deck.gain = 0.0f;
        decks_[static_cast<size_t>(active_)].target = 0.0f;
        active_ = next;
    }
    else
        deck.gain = 1.0f;
    return true;
}

bool SongPlayer::replaceSong(std::shared_ptr<const VgmSong> song, std::shared_ptr<const SongSnapshots> snapshots,
                             size_t ymUnchanged, size_t pcmUnchanged)
{
    song_ = std::move(song);
    snapshots_ = std::move(snapshots);
    Deck& deck = decks_[static_cast<size_t>(active_)];
    if (!deck.on)
        return true;
    if (deck.ymWrite <= ymUnchanged && deck.pcmWrite <= pcmUnchanged && deck.silence == 0)
    {
        deck.song = song_;
        return true;
    }
    return play(deck.songTime);
}

void SongPlayer::stop()
{
    decks_[static_cast<size_t>(active_)].target = 0.0f;
}

void SongPlayer::renderDeck(Deck& deck, float* left, float* right, int count)
{
    const VgmSong& song = *deck.song;
    constexpr int kChunk = 256;
    float fmL[kChunk], fmR[kChunk], pcmL[kChunk], pcmR[kChunk];
    // Only the deck being heard is measured.
    const bool measure = &deck == &decks_[static_cast<size_t>(active_)];
    std::array<int32_t, kMeterChannels> peaks {};

    int pos = 0;
    // Before the song starts: silence.
    while (pos < count && deck.silence > 0)
    {
        ++pos;
        --deck.silence;
        deck.songTime += kVgmRate / hostRate_;
    }

    while (pos < count)
    {
        const int n = std::min(count - pos, kChunk);
        deck.fmResampler.process(fmL, fmR, n, [&](float& l, float& r) {
            const double now = static_cast<double>(deck.fmSample++) * kVgmRate / song.fmRate;
            for (; deck.ymWrite < song.ym.size() && song.ym[deck.ymWrite].time <= now; ++deck.ymWrite)
            {
                deck.fm.write(0, static_cast<uint8_t>(song.ym[deck.ymWrite].address));
                deck.fm.write(1, song.ym[deck.ymWrite].value);
            }
            ymfm::ym2151::output_data data;
            deck.fm.generate(&data, fmMask_, measure ? peaks.data() : nullptr);
            l = static_cast<float>(data.data[0]) * kChipScale;
            r = static_cast<float>(data.data[1]) * kChipScale;
            deck.fmFilter.process(l, r);
        });
        deck.pcmResampler.process(pcmL, pcmR, n, [&](float& l, float& r) {
            const double now = static_cast<double>(deck.pcmSample++) * kVgmRate / song.pcmRate;
            for (; deck.pcmWrite < song.pcm.size() && song.pcm[deck.pcmWrite].time <= now; ++deck.pcmWrite)
                writePcm(deck.pcm, song.pcm[deck.pcmWrite]);
            int32_t a = 0, b = 0;
            deck.pcm.generate(a, b, pcmMask_, measure ? peaks.data() + 8 : nullptr);
            l = static_cast<float>(a) * kChipScale;
            r = static_cast<float>(b) * kChipScale;
        });

        for (int i = 0; i < n; ++i)
        {
            deck.pcmFilter.process(pcmL[i], pcmR[i]);
            if (deck.gain != deck.target)
                deck.gain = deck.target > deck.gain ? std::min(deck.target, deck.gain + fade_)
                                                    : std::max(deck.target, deck.gain - fade_);
            left[pos + i] += (fmL[i] * fmLevel_ + pcmL[i] * pcmLevel_) * deck.gain;
            right[pos + i] += (fmR[i] * fmLevel_ + pcmR[i] * pcmLevel_) * deck.gain;
        }
        pos += n;
        deck.songTime += n * kVgmRate / hostRate_;
    }
    if (deck.gain == 0.0f && deck.target == 0.0f)
        deck.on = false;
    if (measure)
        for (size_t c = 0; c < peaks.size(); ++c)
        {
            // FM channels reach 32767; a PCM channel 127 x 127.
            const float level = static_cast<float>(peaks[c]) * (c < 8 ? kChipScale : 1.0f / 16129.0f);
            if (level > peaks_[c].load(std::memory_order_relaxed))
                peaks_[c].store(level, std::memory_order_relaxed);
        }
}

std::array<float, SongPlayer::kMeterChannels> SongPlayer::takePeaks()
{
    std::array<float, kMeterChannels> result {};
    for (size_t c = 0; c < result.size(); ++c)
        result[c] = peaks_[c].exchange(0.0f, std::memory_order_relaxed);
    return result;
}

void SongPlayer::render(float* left, float* right, int count)
{
    std::fill(left, left + count, 0.0f);
    std::fill(right, right + count, 0.0f);
    if (!song_)
        return;
    for (Deck& deck : decks_)
        if (deck.on)
            renderDeck(deck, left, right, count);
}

} // namespace opm86
