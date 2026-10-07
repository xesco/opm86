#include "VgmSong.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace opm86
{

namespace
{

class Recorder : public VgmVisitor
{
public:
    explicit Recorder(VgmSong& song) : song_(song) {}

    void begin(const VgmHeader& h) override { song_.header = h; }
    void wait(uint32_t samples) override { time_ += samples; }

    void ym2151(uint8_t reg, uint8_t value) override
    {
        song_.ym.push_back({ time_, reg, value });
        if (reg == 0x08 && (value & 0x78))
        {
            song_.fmChannelsUsed |= static_cast<uint8_t>(1 << (value & 7));
            onsets.push_back(time_);
        }
    }

    void segaPcm(uint16_t offset, uint8_t value) override
    {
        song_.pcm.push_back({ time_, static_cast<uint16_t>(offset & 0xFF), value });
        pcmRegs_[offset & 0xFF] = value;
        if ((offset & 0x87) == 0x86 && !(value & 1))
        {
            song_.pcmUsed = true;
            // A trigger at volume 0 on both sides makes no sound, so it is no
            // start for the beat: some drivers restart channels so many times
            // a second.
            const int base = (offset & 0x78);
            if (pcmRegs_[static_cast<size_t>(base + 2)] != 0 || pcmRegs_[static_cast<size_t>(base + 3)] != 0)
                onsets.push_back(time_);
        }
    }

    void pcmRom(const uint8_t* data, size_t size, uint32_t, uint32_t start) override
    {
        for (size_t i = 0; i < size && start + i < song_.rom.size(); ++i)
            song_.rom[start + i] = data[i];
    }

    uint32_t time() const { return time_; }
    std::vector<uint32_t> onsets;

private:
    VgmSong& song_;
    std::array<uint8_t, 256> pcmRegs_ {}; // the SegaPCM's registers as written so far
    uint32_t time_ = 0;
};

} // namespace

// Fit a beat grid to the note starts. The most common gap between starts
// is the song's step (usually a 16th note), refined by a least-squares fit
// over the whole song; a beat is the step doubled until the tempo is from
// 80 to 160 BPM.
BeatGrid fitBeatGrid(std::vector<uint32_t> raw)
{
    BeatGrid grid;
    std::sort(raw.begin(), raw.end());
    raw.erase(std::unique(raw.begin(), raw.end()), raw.end());
    // Starts closer than 15 ms are one: a drum and the note that follows it,
    // or a strummed chord. Counted apart, their gaps would vote for a step
    // that is not the music's.
    {
        std::vector<uint32_t> merged;
        for (uint32_t t : raw)
            if (merged.empty() || t - merged.back() > 660)
                merged.push_back(t);
        raw = std::move(merged);
    }
    if (raw.size() < 8)
    {
        return grid;
    }

    // Gaps of 25 ms to 400 ms vote in bins 1% wide; the best run of three
    // bins wins, and its gaps' median is the step.
    constexpr double kMinGap = 1100.0, kMaxGap = 17640.0;
    const int bins = static_cast<int>(std::ceil(std::log(kMaxGap / kMinGap) / std::log(1.01)));
    std::vector<int> votes(static_cast<size_t>(bins) + 1, 0);
    std::vector<double> gaps;
    for (size_t i = 1; i < raw.size(); ++i)
    {
        const double gap = static_cast<double>(raw[i] - raw[i - 1]);
        if (gap < kMinGap || gap > kMaxGap)
            continue;
        gaps.push_back(gap);
        ++votes[static_cast<size_t>(std::log(gap / kMinGap) / std::log(1.01))];
    }
    if (gaps.empty())
    {
        return grid;
    }
    int bestBin = 0, bestVotes = -1;
    for (int i = 1; i + 1 < bins; ++i)
    {
        const int v = votes[static_cast<size_t>(i - 1)] + votes[static_cast<size_t>(i)] + votes[static_cast<size_t>(i + 1)];
        if (v > bestVotes)
            bestVotes = v, bestBin = i;
    }
    const double centre = kMinGap * std::pow(1.01, bestBin + 0.5);
    std::vector<double> near;
    for (double g : gaps)
        if (std::abs(g / centre - 1.0) < 0.02)
            near.push_back(g);
    std::nth_element(near.begin(), near.begin() + static_cast<long>(near.size() / 2), near.end());
    double step = near[near.size() / 2];

    // The median gap is close but not exact, and over a whole song a step a
    // fraction off puts later starts on the wrong step number. So try steps
    // within 2% of it, and keep the one the starts agree with best: each
    // start an angle round the step, the step whose angles line up most.
    double origin = raw.front();
    {
        constexpr double kTwoPi = 6.283185307179586;
        const double rough = step;
        double bestLength = -1.0;
        for (int i = -400; i <= 400; ++i)
        {
            const double candidate = rough * (1.0 + i * 0.00005);
            double c = 0.0, sn = 0.0;
            for (uint32_t t : raw)
            {
                const double a = kTwoPi * std::fmod(static_cast<double>(t), candidate) / candidate;
                c += std::cos(a);
                sn += std::sin(a);
            }
            if (const double length = std::hypot(c, sn); length > bestLength)
            {
                bestLength = length;
                step = candidate;
                origin = std::atan2(sn, c) / kTwoPi * candidate;
            }
        }
    }

    // Least squares: each start's step number against its time.
    for (int pass = 0; pass < 2; ++pass)
    {
        double sn = 0, st = 0, snn = 0, snt = 0, count = 0;
        for (uint32_t t : raw)
        {
            const double n = std::round((t - origin) / step);
            if (std::abs(t - (origin + n * step)) > step * 0.2)
                continue; // off the grid: a triplet or an ornament
            sn += n, st += t, snn += n * n, snt += n * t, count += 1;
        }
        const double denom = count * snn - sn * sn;
        if (denom <= 0)
            break;
        step = (count * snt - sn * st) / denom;
        origin = (st - step * sn) / count;
    }

    // A beat of steps in twos, the shortest from 375 ms (160 BPM) down; or
    // in threes, as in 6/8 or 12/8, when a bar of four such beats is what
    // the music repeats, clearly more than a bar of the twos' beats.
    auto binary = [&] {
        int m = 1;
        while (step * m < 60.0 / 160.0 * kVgmRate && m < 16)
            m *= 2;
        return m;
    };
    auto ternary = [&] {
        int m = 3;
        while (step * m < 60.0 / 160.0 * kVgmRate && m < 48)
            m *= 2;
        return m;
    };
    // The share of starts with another start one bar of four beats later.
    auto repeats = [&](int m) {
        const double bar = 4.0 * step * m, tolerance = step * 0.25;
        int hits = 0;
        for (uint32_t t : raw)
        {
            const auto it = std::lower_bound(raw.begin(), raw.end(), static_cast<double>(t) + bar - tolerance,
                                             [](uint32_t a, double b) { return static_cast<double>(a) < b; });
            if (it != raw.end() && static_cast<double>(*it) <= static_cast<double>(t) + bar + tolerance)
                ++hits;
        }
        return static_cast<double>(hits) / static_cast<double>(raw.size());
    };
    const int twos = binary(), threes = ternary();
    const int multiple = repeats(threes) >= repeats(twos) + 0.2 ? threes : twos;
    const double beat = step * multiple;

    // Of the steps a beat could start on, take the one most starts fall on.
    int bestShift = 0, bestCount = -1;
    for (int shift = 0; shift < multiple; ++shift)
    {
        int hits = 0;
        for (uint32_t t : raw)
        {
            const long n = std::lround((t - origin) / step);
            if (((n - shift) % multiple + multiple) % multiple == 0 && std::abs(t - (origin + n * step)) < step * 0.25)
                ++hits;
        }
        if (hits > bestCount)
            bestCount = hits, bestShift = shift;
    }
    // Beat 1 is the beat of the first start, or the one just before it.
    grid.length = beat;
    const double phase = origin + bestShift * step;
    grid.first = phase + std::floor((raw.front() - phase) / beat + 0.25) * beat;
    return grid;
}

std::shared_ptr<const VgmSong> loadVgmSong(const std::vector<uint8_t>& file, const std::string& name,
                                           std::string& error)
{
    auto song = std::make_shared<VgmSong>();
    song->name = name;
    Recorder recorder(*song);
    error = parseVgm(file, recorder);
    if (song->ym.empty() && song->pcm.empty())
    {
        if (error.empty())
            error = "This VGM file has no YM2151 or SegaPCM writes.";
        return nullptr;
    }
    song->length = recorder.time();
    song->fmRate = (song->header.ym2151Clock ? song->header.ym2151Clock : 4000000) / 64.0;
    song->pcmRate = (song->header.segaPcmClock ? song->header.segaPcmClock : 4000000) / 128.0;

    appendSongEnd(*song);

    const BeatGrid grid = fitBeatGrid(recorder.onsets);
    song->beatLength = grid.length;
    song->firstBeat = grid.first;
    // Beats until two seconds after the end, so releases are played.
    song->beatCount = std::max(1, static_cast<int>(std::ceil((song->length + 2.0 * kVgmRate - song->firstBeat) / song->beatLength)));
    return song;
}

void appendSongEnd(VgmSong& song)
{
    song.ymLogged = song.ym.size();
    song.pcmLogged = song.pcm.size();
    for (uint16_t ch = 0; ch < 8; ++ch)
        song.ym.push_back({ song.length, 0x08, static_cast<uint8_t>(ch) });
    for (uint16_t ch = 0; ch < SegaPcm::kChannels; ++ch)
        song.pcm.push_back({ song.length, static_cast<uint16_t>(0x1000 | (ch * 8 + 0x86)), 0 });
}

uint32_t songId(const std::string& name)
{
    // FNV-1a, folded to 28 bits: four 7-bit MIDI values.
    uint32_t h = 2166136261u;
    for (unsigned char c : name)
        h = (h ^ c) * 16777619u;
    return (h ^ (h >> 28)) & 0x0FFFFFFF;
}

} // namespace opm86
