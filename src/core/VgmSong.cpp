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
            starts.notes.push_back(time_);
        }
    }

    void segaPcm(uint16_t offset, uint8_t value) override
    {
        song_.pcm.push_back({ time_, static_cast<uint16_t>(offset & 0xFF), value });
        pcmRegs_[offset & 0xFF] = value;
        if ((offset & 0x87) == 0x86 && !(value & 1))
        {
            song_.pcmUsed = true;
            // A hit counts when it sounds: not at volume 0 on both sides.
            const int base = (offset & 0x78);
            if (pcmRegs_[static_cast<size_t>(base + 2)] != 0 || pcmRegs_[static_cast<size_t>(base + 3)] != 0)
                starts.drums.push_back(time_);
        }
    }

    void pcmRom(const uint8_t* data, size_t size, uint32_t, uint32_t start) override
    {
        for (size_t i = 0; i < size && start + i < song_.rom.size(); ++i)
            song_.rom[start + i] = data[i];
    }

    uint32_t time() const { return time_; }
    NoteStarts starts; // for the beat grid

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
// Fitting a beat grid to note starts ----------------------------------------
//
// A VGM file has no tempo, only the chips' writes and when they come, so the
// grid is found from when notes start, in these steps:
//
// 1. Starts within kMergeWindow of a first are one start: a game starts a
//    chord's voices, and its drum, one after another over a driver tick or
//    two.
// 2. The step, the shortest note the music keeps to, is the most common gap
//    between starts (roughStep), made exact by the step the starts line up
//    with best (alignStep) and by least squares (fitLine).
// 3. The grid is laid on the typical pitched note (centreOnStarts): the mean
//    of all starts lies late, since a game can trigger its drums a little
//    after the beat. With too few pitched notes, on the typical start.
// 4. Steps group into beats, in twos, or in threes as in 6/8 when the music
//    clearly repeats in bars of such beats (stepsPerBeat), each beat on the
//    step most starts fall on (beatPhase).
// 5. Beat 1 is the beat of the first start.

namespace
{

constexpr size_t kFewestStarts = 8;                        // with fewer starts the grid keeps its default, a beat of 0.5s
constexpr double kMergeWindow = 0.030 * kVgmRate;          // starts this close to a first are one
constexpr double kShortestGap = 0.025 * kVgmRate;          // gaps that vote for the step, from this...
constexpr double kLongestGap = 0.400 * kVgmRate;           // ...to this
constexpr double kBinWidth = 1.01;                         // a vote's bin, 1% wide
constexpr double kNearStep = 0.02;                         // gaps this close to the winning bins give the step, and steps this close are tried
constexpr int kSearchSteps = 400;                          // tried each way from the rough step, in kNearStep / kSearchSteps strides
constexpr double kOnGrid = 0.2;                            // a start within this many steps of the grid counts for the fit
constexpr double kOnBeat = 0.25;                           // and within this many, for the beats, the bars and beat 1
constexpr double kShortestBeat = 60.0 / 160.0 * kVgmRate;  // a beat is at least this long: 160 BPM
constexpr double kClearlyThrees = 0.2;                     // threes win when their bars repeat by this much more

struct StepGrid
{
    double step = 0.0;   // samples
    double origin = 0.0; // a start time the steps count from
};

// Sorted, each start within kMergeWindow of a first folded into it.
std::vector<double> mergeStarts(std::vector<uint32_t> raw)
{
    std::sort(raw.begin(), raw.end());
    std::vector<double> starts;
    for (const uint32_t t : raw)
        if (starts.empty() || t - starts.back() > kMergeWindow)
            starts.push_back(t);
    return starts;
}

// The most common gap between starts: gaps vote in bins kBinWidth wide, the
// best run of three bins wins, and the median of the gaps near it is the
// step. 0 when no gap is in range.
double roughStep(const std::vector<double>& starts)
{
    const int bins = static_cast<int>(std::ceil(std::log(kLongestGap / kShortestGap) / std::log(kBinWidth)));
    std::vector<int> votes(static_cast<size_t>(bins) + 1, 0);
    std::vector<double> gaps;
    for (size_t i = 1; i < starts.size(); ++i)
    {
        const double gap = starts[i] - starts[i - 1];
        if (gap < kShortestGap || gap > kLongestGap)
            continue;
        gaps.push_back(gap);
        ++votes[static_cast<size_t>(std::log(gap / kShortestGap) / std::log(kBinWidth))];
    }
    if (gaps.empty())
        return 0.0;
    int bestBin = 0, bestVotes = -1;
    for (int i = 1; i + 1 < bins; ++i)
        if (const int v = votes[static_cast<size_t>(i - 1)] + votes[static_cast<size_t>(i)] + votes[static_cast<size_t>(i + 1)]; v > bestVotes)
            bestVotes = v, bestBin = i;
    const double centre = kShortestGap * std::pow(kBinWidth, bestBin + 0.5);
    std::vector<double> near;
    for (const double g : gaps)
        if (std::abs(g / centre - 1.0) < kNearStep)
            near.push_back(g);
    std::nth_element(near.begin(), near.begin() + static_cast<long>(near.size() / 2), near.end());
    return near[near.size() / 2];
}

// The rough step is close but not exact, and a step a fraction off puts
// later starts on the wrong step number. Each start is an angle round a
// candidate step; of the steps within kNearStep, the one whose angles line
// up most is the music's, and their mean angle is its origin.
StepGrid alignStep(const std::vector<double>& starts, double rough)
{
    constexpr double kTwoPi = 6.283185307179586;
    StepGrid best { rough, starts.front() };
    double bestLength = -1.0;
    for (int i = -kSearchSteps; i <= kSearchSteps; ++i)
    {
        const double step = rough * (1.0 + i * kNearStep / kSearchSteps);
        double c = 0.0, s = 0.0;
        for (const double t : starts)
        {
            const double a = kTwoPi * std::fmod(t, step) / step;
            c += std::cos(a);
            s += std::sin(a);
        }
        if (const double length = std::hypot(c, s); length > bestLength)
            bestLength = length, best = { step, std::atan2(s, c) / kTwoPi * step };
    }
    return best;
}

// Least squares, twice: each start on the grid by its step number against
// its time. Starts off the grid, triplets or ornaments, are left out.
StepGrid fitLine(const std::vector<double>& starts, StepGrid grid)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        double sn = 0, st = 0, snn = 0, snt = 0, count = 0;
        for (const double t : starts)
        {
            const double n = std::round((t - grid.origin) / grid.step);
            if (std::abs(t - (grid.origin + n * grid.step)) > grid.step * kOnGrid)
                continue;
            sn += n, st += t, snn += n * n, snt += n * t, count += 1;
        }
        const double denom = count * snn - sn * sn;
        if (denom <= 0)
            break;
        grid.step = (count * snt - sn * st) / denom;
        grid.origin = (st - grid.step * sn) / count;
    }
    return grid;
}

// Moves the grid by the median of the given starts' distances to it, so it
// lies on their typical start rather than on the mean least squares gives.
StepGrid centreOnStarts(const std::vector<double>& starts, StepGrid grid)
{
    std::vector<double> offsets;
    for (const double t : starts)
    {
        const double n = std::round((t - grid.origin) / grid.step);
        if (const double d = t - (grid.origin + n * grid.step); std::abs(d) <= grid.step * kOnGrid)
            offsets.push_back(d);
    }
    if (offsets.empty())
        return grid;
    std::nth_element(offsets.begin(), offsets.begin() + static_cast<long>(offsets.size() / 2), offsets.end());
    grid.origin += offsets[offsets.size() / 2];
    return grid;
}

// The share of starts with another start one bar of four beats later.
double barRepeats(const std::vector<double>& starts, double step, int stepsPerBeat)
{
    const double bar = 4.0 * step * stepsPerBeat, tolerance = step * kOnBeat;
    int hits = 0;
    for (const double t : starts)
        if (const auto it = std::lower_bound(starts.begin(), starts.end(), t + bar - tolerance);
            it != starts.end() && *it <= t + bar + tolerance)
            ++hits;
    return static_cast<double>(hits) / static_cast<double>(starts.size());
}

// Steps group into beats in twos, 1, 2, 4... steps, the fewest that make a
// beat of kShortestBeat or more; or in threes, 3, 6, 12... steps, as in 6/8
// or 12/8, when a bar of four such beats is what the music repeats,
// clearly more than a bar of the twos' beats.
int stepsPerBeat(const std::vector<double>& starts, double step)
{
    auto fewest = [step](int first, int most) {
        int m = first;
        while (step * m < kShortestBeat && m < most)
            m *= 2;
        return m;
    };
    const int twos = fewest(1, 16), threes = fewest(3, 48);
    return barRepeats(starts, step, threes) >= barRepeats(starts, step, twos) + kClearlyThrees ? threes : twos;
}

// The step a beat starts on: of the stepsPerBeat steps it could be, the one
// most starts fall on. Returns that step's time.
double beatPhase(const std::vector<double>& starts, const StepGrid& grid, int stepsPerBeat)
{
    int bestShift = 0, bestCount = -1;
    for (int shift = 0; shift < stepsPerBeat; ++shift)
    {
        int hits = 0;
        for (const double t : starts)
        {
            const long n = std::lround((t - grid.origin) / grid.step);
            if (((n - shift) % stepsPerBeat + stepsPerBeat) % stepsPerBeat == 0 && std::abs(t - (grid.origin + n * grid.step)) < grid.step * kOnBeat)
                ++hits;
        }
        if (hits > bestCount)
            bestCount = hits, bestShift = shift;
    }
    return grid.origin + bestShift * grid.step;
}

} // namespace

BeatGrid fitBeatGrid(const NoteStarts& given)
{
    BeatGrid grid;
    std::vector<uint32_t> all = given.notes;
    all.insert(all.end(), given.drums.begin(), given.drums.end());
    const std::vector<double> starts = mergeStarts(std::move(all)); // step 1
    if (starts.size() < kFewestStarts)
        return grid;
    const double rough = roughStep(starts); // step 2
    if (rough <= 0.0)
        return grid;
    const StepGrid fitted = fitLine(starts, alignStep(starts, rough));
    const std::vector<double> notes = mergeStarts(given.notes); // step 3
    const StepGrid steps = centreOnStarts(notes.size() >= kFewestStarts ? notes : starts, fitted);
    const int perBeat = stepsPerBeat(starts, steps.step); // step 4
    const double phase = beatPhase(starts, steps, perBeat);

    // Step 5: the beat of the first start, or the one just before it.
    grid.length = steps.step * perBeat;
    grid.first = phase + std::floor((starts.front() - phase) / grid.length + kOnBeat) * grid.length;
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

    const BeatGrid grid = fitBeatGrid(recorder.starts);
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
