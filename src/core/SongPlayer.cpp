#include "SongPlayer.h"

#include <algorithm>
#include <cmath>
#include <thread>

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

// Gives the chip the writes due by a song time, and returns the next write.
// The chip reads each channel's key state once per chip sample, so a key off
// and a key on in one sample would never restart the note. A key write that
// changes what this sample already set for its channel waits for the next
// sample, along with the writes after it.
size_t writeFm(MaskedYm2151& fm, const VgmSong& song, size_t write, double now)
{
    std::array<int, 8> keys;
    keys.fill(-1);
    for (; write < song.ym.size() && song.ym[write].time <= now; ++write)
    {
        const VgmWrite& w = song.ym[write];
        if (w.address == 0x08)
        {
            int& key = keys[w.value & 7];
            const int operators = w.value & 0x78;
            if (key >= 0 && key != operators)
                break;
            key = operators;
        }
        fm.write(0, static_cast<uint8_t>(w.address));
        fm.write(1, w.value);
    }
    return write;
}

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
        write = writeFm(fm, song, write, now);
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
    pcm.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
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
    pcm.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
    out->pcm.push_back({ 0, 0, pcm });
    return out;
}

namespace
{

// Takes up a channel's output over time, for ownerPeaks.
class OwnerTally
{
public:
    OwnerTally(const ChannelOwners& owners, std::vector<int32_t>& loudest) : owners_(owners), loudest_(loudest) {}

    void add(size_t channel, double now, int32_t peak)
    {
        const auto& v = owners_.changes[channel];
        size_t& i = at_[channel];
        while (i < v.size() && v[i].time <= now)
            ++i;
        if (i > 0 && v[i - 1].owner >= 0)
        {
            int32_t& most = loudest_[static_cast<size_t>(v[i - 1].owner)];
            most = std::max(most, peak);
        }
    }

private:
    const ChannelOwners& owners_;
    std::vector<int32_t>& loudest_;
    std::array<size_t, 24> at_ {};
};

bool stopped(const std::atomic<bool>* stop, uint64_t sample)
{
    return stop && (sample & 0x3FFF) == 0 && stop->load();
}

void tallyFmChannel(const VgmSong& song, int channel, OwnerTally& tally, const std::atomic<bool>* stop)
{
    NullInterface interface;
    MaskedYm2151 fm(interface);
    fm.reset();
    size_t write = 0;
    const double end = songEnd(song);
    for (uint64_t k = 0;; ++k)
    {
        const double now = static_cast<double>(k) * kVgmRate / song.fmRate;
        if (now > end || stopped(stop, k))
            return;
        write = writeFm(fm, song, write, now);
        tally.add(static_cast<size_t>(channel), now, fm.generateChannel(static_cast<uint32_t>(channel)));
    }
}

void tallyPcm(const VgmSong& song, OwnerTally& tally, const std::atomic<bool>* stop)
{
    SegaPcm pcm;
    pcm.reset();
    pcm.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
    size_t write = 0;
    const double end = songEnd(song);
    for (uint64_t k = 0;; ++k)
    {
        const double now = static_cast<double>(k) * kVgmRate / song.pcmRate;
        if (now > end || stopped(stop, k))
            return;
        while (write < song.pcm.size() && song.pcm[write].time <= now)
            writePcm(pcm, song.pcm[write++]);
        int32_t l = 0, r = 0;
        std::array<int32_t, 16> peaks {};
        pcm.generate(l, r, 0xFFFF, peaks.data());
        for (size_t c = 0; c < peaks.size(); ++c)
            tally.add(8 + c, now, peaks[c]);
    }
}

} // namespace

std::vector<float> ownerPeaks(const VgmSong& song, const ChannelOwners& owners, int count, const std::atomic<bool>* stop)
{
    // One job per FM channel, and one for the PCM chip. A core stays free for
    // the audio and the display.
    constexpr int kJobs = 9;
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, kJobs);
    std::vector<std::vector<int32_t>> tallies(static_cast<size_t>(kJobs), std::vector<int32_t>(static_cast<size_t>(count), 0));
    std::atomic<int> next { 0 };
    const auto work = [&] {
        for (int job = next.fetch_add(1); job < kJobs; job = next.fetch_add(1))
        {
            OwnerTally tally(owners, tallies[static_cast<size_t>(job)]);
            if (job < 8)
                tallyFmChannel(song, job, tally, stop);
            else
                tallyPcm(song, tally, stop);
        }
    };
    {
        std::vector<std::thread> threads;
        for (int w = 0; w < workers; ++w)
            threads.emplace_back(work);
        for (auto& t : threads)
            t.join();
    }
    if (stop && stop->load())
        return {};

    std::vector<float> out(static_cast<size_t>(count), 0.0f);
    for (const auto& tally : tallies)
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = std::max(out[i], static_cast<float>(tally[i]) * kChipScale);
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

double SongPlayer::fmNow(const Deck& d) const
{
    const double rate = d.song->fmRate;
    return d.stretched ? d.fmAnchorTime + static_cast<double>(d.fmSample - d.fmAnchor) * speed_ * kVgmRate / rate
                       : static_cast<double>(d.fmSample) * kVgmRate / rate;
}

double SongPlayer::pcmNow(const Deck& d) const
{
    const double rate = d.song->pcmRate;
    return d.stretched ? d.pcmAnchorTime + static_cast<double>(d.pcmSample - d.pcmAnchor) * speed_ * kVgmRate / rate
                       : static_cast<double>(d.pcmSample) * kVgmRate / rate;
}

void SongPlayer::seekMix(Deck& d, bool glide)
{
    d.mix = mix_;
    const double fm = fmNow(d), pcm = pcmNow(d);
    if (glide)
    {
        d.fromGain = d.lastGain;
        d.fadeFrom[0] = fm, d.fadeFrom[1] = pcm;
    }
    else
        d.fadeFrom[0] = d.fadeFrom[1] = -1e18;
    if (!d.mix)
        return;
    for (size_t c = 0; c < d.mixAt.size(); ++c)
    {
        const auto& v = d.mix->changes[c];
        const double now = c < 8 ? fm : pcm;
        d.mixAt[c] = static_cast<size_t>(std::upper_bound(v.begin(), v.end(), now, [](double t, const ChannelMix::Change& x) {
                                             return t < x.time;
                                         }) - v.begin());
    }
}

void SongPlayer::setSpeed(double speed)
{
    if (speed == speed_)
        return;
    // Each deck carries on from where it is, at the new speed.
    for (Deck& d : decks_)
        if (d.on && d.song)
        {
            d.fmAnchorTime = fmNow(d), d.fmAnchor = d.fmSample;
            d.pcmAnchorTime = pcmNow(d), d.pcmAnchor = d.pcmSample;
            d.stretched = true;
        }
    speed_ = speed;
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
    deck.pcm.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
    deck.pcmSample = pcmSnap.sample;
    deck.pcmWrite = pcmSnap.write;

    // Run silently from the snapshot to the target, as the song would.
    ymfm::ym2151::output_data data;
    for (; deck.fmSample < fmTarget; ++deck.fmSample)
    {
        const double now = static_cast<double>(deck.fmSample) * kVgmRate / song.fmRate;
        deck.ymWrite = writeFm(deck.fm, song, deck.ymWrite, now);
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
    deck.stretched = speed_ != 1.0;
    deck.fmAnchor = deck.fmSample, deck.fmAnchorTime = static_cast<double>(deck.fmSample) * kVgmRate / song.fmRate;
    deck.pcmAnchor = deck.pcmSample, deck.pcmAnchorTime = static_cast<double>(deck.pcmSample) * kVgmRate / song.pcmRate;
    deck.lastGain.fill(1.0f);
    seekMix(deck, false);
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
    if (deck.mix != mix_)
        seekMix(deck, true);
    // The mix's gains at a song time, each cursor moving on past the changes
    // due: the channels heard, and whether all of them are at gain 1, so the
    // chip can mix them itself, exactly.
    auto mixed = [&deck](double now, size_t first, size_t count, float* gains, bool& plain) {
        uint32_t mask = (1u << count) - 1;
        plain = true;
        constexpr double kGlide = 0.02 * kVgmRate; // 20 ms
        const double since = now - deck.fadeFrom[first == 0 ? 0 : 1];
        const float along = since < kGlide ? static_cast<float>(std::max(0.0, since) / kGlide) : 1.0f;
        for (size_t c = 0; c < count; ++c)
        {
            float target = 1.0f;
            if (deck.mix)
            {
                const auto& v = deck.mix->changes[first + c];
                size_t& i = deck.mixAt[first + c];
                while (i < v.size() && v[i].time <= now)
                    ++i;
                target = i > 0 ? v[i - 1].gain : 1.0f;
            }
            const float from = deck.fromGain[first + c];
            gains[c] = along < 1.0f ? from + (target - from) * along : target;
            deck.lastGain[first + c] = gains[c];
            if (gains[c] <= 0.0f)
                mask &= ~(1u << c);
            else if (std::abs(gains[c] - 1.0f) > 1e-6f)
                plain = false;
        }
        return mask;
    };
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
        deck.songTime += speed_ * kVgmRate / hostRate_;
    }

    while (pos < count)
    {
        const int n = std::min(count - pos, kChunk);
        deck.fmResampler.process(fmL, fmR, n, [&](float& l, float& r) {
            const double now = fmNow(deck);
            ++deck.fmSample;
            deck.ymWrite = writeFm(deck.fm, song, deck.ymWrite, now);
            ymfm::ym2151::output_data data;
            float gains[8];
            bool plain = true;
            const uint32_t mask = fmMask_ & mixed(now, 0, 8, gains, plain);
            if (plain)
                deck.fm.generate(&data, mask, measure ? peaks.data() : nullptr);
            else
                deck.fm.generate(&data, mask, gains, measure ? peaks.data() : nullptr);
            l = static_cast<float>(data.data[0]) * kChipScale;
            r = static_cast<float>(data.data[1]) * kChipScale;
            deck.fmFilter.process(l, r);
        });
        deck.pcmResampler.process(pcmL, pcmR, n, [&](float& l, float& r) {
            const double now = pcmNow(deck);
            ++deck.pcmSample;
            for (; deck.pcmWrite < song.pcm.size() && song.pcm[deck.pcmWrite].time <= now; ++deck.pcmWrite)
                writePcm(deck.pcm, song.pcm[deck.pcmWrite]);
            int32_t a = 0, b = 0;
            float gains[16];
            bool plain = true;
            const auto mask = static_cast<uint16_t>(pcmMask_ & mixed(now, 8, 16, gains, plain));
            deck.pcm.generate(a, b, mask, measure ? peaks.data() + 8 : nullptr, plain ? nullptr : gains);
            l = static_cast<float>(a) * kChipScale;
            r = static_cast<float>(b) * kChipScale;
        });

        std::array<float, 4> chip {};
        for (int i = 0; i < n; ++i)
        {
            deck.pcmFilter.process(pcmL[i], pcmR[i]);
            if (deck.gain != deck.target)
                deck.gain = deck.target > deck.gain ? std::min(deck.target, deck.gain + fade_)
                                                    : std::max(deck.target, deck.gain - fade_);
            left[pos + i] += (fmL[i] * fmLevel_ + pcmL[i] * pcmLevel_) * deck.gain;
            right[pos + i] += (fmR[i] * fmLevel_ + pcmR[i] * pcmLevel_) * deck.gain;
            // Each chip's own output, before the two are mixed.
            chip[0] = std::max(chip[0], std::abs(fmL[i] * fmLevel_ * deck.gain));
            chip[1] = std::max(chip[1], std::abs(fmR[i] * fmLevel_ * deck.gain));
            chip[2] = std::max(chip[2], std::abs(pcmL[i] * pcmLevel_ * deck.gain));
            chip[3] = std::max(chip[3], std::abs(pcmR[i] * pcmLevel_ * deck.gain));
        }
        if (measure)
            for (size_t c = 0; c < chip.size(); ++c)
                if (chip[c] > chipPeaks_[c].load(std::memory_order_relaxed))
                    chipPeaks_[c].store(chip[c], std::memory_order_relaxed);
        pos += n;
        deck.songTime += n * speed_ * kVgmRate / hostRate_;
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
    if (const Deck& deck = decks_[static_cast<size_t>(active_)]; deck.on)
        publishChips(deck);
}

void SongPlayer::publishChips(const Deck& deck)
{
    ChipState state;
    for (uint32_t slot = 0; slot < 32; ++slot)
    {
        uint32_t stage = 0, attenuation = 0;
        deck.fm.envelope(slot, stage, attenuation);
        // A release that has reached silence is off.
        state.stage[slot] = stage == ymfm::EG_ATTACK    ? ChipState::Stage::Attack
                            : stage == ymfm::EG_DECAY   ? ChipState::Stage::Decay
                            : stage == ymfm::EG_SUSTAIN ? ChipState::Stage::Sustain
                            : attenuation >= 0x3FF      ? ChipState::Stage::Off
                                                        : ChipState::Stage::Release;
        state.level[slot] = 1.0f - static_cast<float>(std::min<uint32_t>(attenuation, 0x3FF)) / 1023.0f;
    }
    for (int ch = 0; ch < SegaPcm::kChannels; ++ch)
    {
        state.pcmAddress[static_cast<size_t>(ch)] = deck.pcm.address(ch);
        if (deck.pcm.playing(ch))
            state.pcmPlaying = static_cast<uint16_t>(state.pcmPlaying | (1 << ch));
    }
    state.gain = deck.lastGain;
    state.valid = true;
    // A sequence counter, odd while writing: the reader copies and retries
    // until it reads the same even count before and after.
    const uint32_t sequence = chipsSequence_.load(std::memory_order_relaxed);
    chipsSequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    chips_ = state;
    chipsSequence_.store(sequence + 2, std::memory_order_release);
}

std::array<float, 4> SongPlayer::takeChipPeaks()
{
    std::array<float, 4> result {};
    for (size_t c = 0; c < result.size(); ++c)
        result[c] = chipPeaks_[c].exchange(0.0f, std::memory_order_relaxed);
    return result;
}

ChipState SongPlayer::chipState() const
{
    for (;;)
    {
        const uint32_t before = chipsSequence_.load(std::memory_order_acquire);
        if (before & 1)
            continue;
        const ChipState copy = chips_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (chipsSequence_.load(std::memory_order_relaxed) == before)
            return copy;
    }
}

} // namespace opm86
