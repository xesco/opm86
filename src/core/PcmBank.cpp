#include "PcmBank.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{

constexpr double kNativeRate = SegaPcm::kSampleRate / 2.0; // step 0x80 = 15625 Hz
constexpr double kPi = 3.14159265358979323846;

// RBJ low-pass biquad, used to band-limit audio before decimation.
struct LowPass
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    LowPass(double cutoff, double rate)
    {
        const double w = 2.0 * kPi * cutoff / rate;
        const double alpha = std::sin(w) / (2.0 * 0.7071);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 - std::cos(w)) / 2.0 / a0;
        b1 = (1.0 - std::cos(w)) / a0;
        b2 = b0;
        a1 = -2.0 * std::cos(w) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double process(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

class Noise
{
public:
    float next()
    {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<float>(state_ >> 8) / 8388608.0f - 1.0f;
    }

private:
    uint32_t state_ = 0x0D0E1986u;
};

using Buffer = std::vector<float>;

Buffer drum(double seconds)
{
    return Buffer(static_cast<size_t>(seconds * kNativeRate), 0.0f);
}

float env(size_t i, double decaySeconds)
{
    return static_cast<float>(std::exp(-static_cast<double>(i) / (decaySeconds * kNativeRate)));
}

Buffer kick()
{
    Buffer b = drum(0.45);
    double phase = 0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = static_cast<double>(i) / kNativeRate;
        phase += 2.0 * kPi * (45.0 + 110.0 * std::exp(-t * 30.0)) / kNativeRate;
        b[i] = static_cast<float>(std::sin(phase)) * env(i, 0.12);
    }
    return b;
}

Buffer tom(double pitch)
{
    Buffer b = drum(0.5);
    double phase = 0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = static_cast<double>(i) / kNativeRate;
        phase += 2.0 * kPi * pitch * (1.0 + 0.6 * std::exp(-t * 18.0)) / kNativeRate;
        b[i] = static_cast<float>(std::sin(phase)) * env(i, 0.14);
    }
    return b;
}

Buffer snare(double tone, double decay)
{
    Buffer b = drum(0.35);
    Noise noise;
    double phase = 0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        phase += 2.0 * kPi * tone / kNativeRate;
        b[i] = 0.4f * static_cast<float>(std::sin(phase)) * env(i, 0.04) + 0.6f * noise.next() * env(i, decay);
    }
    return b;
}

// Differentiated noise leaves mostly the top octaves, like a metal cymbal.
Buffer metal(double seconds, double decay)
{
    Buffer b = drum(seconds);
    Noise noise;
    float last = 0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        const float n = noise.next();
        b[i] = 0.7f * (n - last) * env(i, decay);
        last = n;
    }
    return b;
}

Buffer clap()
{
    Buffer b = drum(0.3);
    Noise noise;
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = static_cast<double>(i) / kNativeRate;
        const double burst = t < 0.03 ? std::exp(-std::fmod(t, 0.01) * 300.0) : std::exp(-(t - 0.03) * 18.0);
        b[i] = 0.8f * noise.next() * static_cast<float>(burst);
    }
    return b;
}

Buffer stick()
{
    Buffer b = drum(0.08);
    double phase = 0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        phase += 2.0 * kPi * 1700.0 / kNativeRate;
        b[i] = static_cast<float>(std::sin(phase) > 0 ? 0.6 : -0.6) * env(i, 0.012);
    }
    return b;
}

PadSample fromBuffer(const char* name, const Buffer& b)
{
    return makeSample(name, b.data(), b.size(), kNativeRate);
}

} // namespace

namespace
{

// Audio at sampleRate as chip bytes at rate: band-limited before dropping
// to a lower rate, then stepped through linearly, up to kMaxSampleBytes.
std::vector<uint8_t> toChip(std::vector<double> source, double sampleRate, double rate)
{
    std::vector<uint8_t> out;
    if (source.empty())
        return out;
    if (sampleRate > rate * 1.01)
    {
        LowPass a(rate * 0.45, sampleRate), b(rate * 0.45, sampleRate);
        for (double& x : source)
            x = b.process(a.process(x));
    }

    const size_t length = source.size();
    const double ratio = sampleRate / rate;
    const auto outLength = std::min(kMaxSampleBytes, static_cast<size_t>(static_cast<double>(length) / ratio));
    out.reserve(outLength);
    for (size_t i = 0; i < outLength; ++i)
    {
        const double pos = static_cast<double>(i) * ratio;
        const auto i0 = static_cast<size_t>(pos);
        const size_t i1 = std::min(i0 + 1, length - 1);
        const double frac = pos - static_cast<double>(i0);
        const double x = source[i0] + (source[i1] - source[i0]) * frac;
        const long q = std::lround(x * 127.0) + 0x80;
        out.push_back(static_cast<uint8_t>(std::clamp(q, 0L, 255L)));
    }
    return out;
}

// Trailing silence would only waste ROM.
void trimTail(std::vector<uint8_t>& data)
{
    while (!data.empty() && std::abs(static_cast<int>(data.back()) - 0x80) <= 1)
        data.pop_back();
}

} // namespace

PadSample makeSample(const std::string& name, const float* audio, size_t length, double sampleRate)
{
    PadSample s;
    s.name = name;
    if (audio == nullptr || length == 0 || sampleRate <= 0)
        return s;
    s.data = toChip(std::vector<double>(audio, audio + length), sampleRate, kNativeRate);
    trimTail(s.data);
    return s;
}

PadSample encodeSample(const std::string& name, const float* const* channels, int channelCount, size_t length, double sampleRate,
                       const SampleEncoding& encoding)
{
    PadSample s;
    s.name = name;
    s.step = std::max<uint8_t>(encoding.step, 1);
    s.left = s.right = 127;
    if (channels == nullptr || channelCount <= 0 || length == 0 || sampleRate <= 0)
        return s;

    // Mono: the channels' mean.
    std::vector<double> mono(length, 0.0);
    for (int c = 0; c < channelCount; ++c)
        for (size_t i = 0; i < length; ++i)
            mono[i] += static_cast<double>(channels[c][i]) / static_cast<double>(channelCount);
    double peak = 0.0;
    for (const double x : mono)
        peak = std::max(peak, std::abs(x));

    // Silence is anything under half the smallest step the chip's eight bits
    // keep, of full scale or, normalised, of the peak.
    if (encoding.trim)
    {
        const double quiet = (encoding.normalise ? peak : 1.0) / 256.0;
        auto loud = [quiet](double x) { return std::abs(x) > quiet; };
        const auto first = std::find_if(mono.begin(), mono.end(), loud);
        const auto last = std::find_if(mono.rbegin(), mono.rend(), loud).base();
        mono = first < last ? std::vector<double>(first, last) : std::vector<double>();
    }
    if (encoding.normalise && peak > 0.0)
        for (double& x : mono)
            x /= peak;

    s.data = toChip(std::move(mono), sampleRate, static_cast<double>(s.step) * SegaPcm::kSampleRate / 256.0);
    if (encoding.trim)
        trimTail(s.data);
    return s;
}

RomRanges samplePages(const std::vector<PadSample>& samples)
{
    RomRanges out;
    for (const PadSample& s : samples)
    {
        if (s.data.empty())
            continue;
        const size_t end = std::min<size_t>(0x10000, (static_cast<size_t>(s.address) + s.data.size() + 0xFF) & ~size_t { 0xFF });
        out.emplace_back(s.romOffset + (s.address & 0xFF00u), s.romOffset + end);
    }
    return out;
}

bool placeSample(size_t& romSize, uint32_t interfaceRegister, RomRanges& used, PadSample& sample, size_t maxSize)
{
    maxSize = std::min(maxSize, kMaxRomSize);
    const size_t bytes = std::min(sample.data.size(), kMaxSampleBytes);
    if (bytes == 0)
        return false;
    const size_t pages = (bytes + 0xFF) >> 8;

    // Each bank the flags' bank bits select, once, by its place in the ROM.
    const SegaPcm::Banking banking = SegaPcm::banking(interfaceRegister);
    std::vector<std::pair<size_t, uint8_t>> banks;
    for (uint32_t flags = 0; flags < 256; ++flags)
    {
        const auto bits = static_cast<uint8_t>(flags & banking.mask);
        const size_t offset = static_cast<size_t>(bits) << banking.shift;
        if (offset < maxSize && std::none_of(banks.begin(), banks.end(), [offset](const auto& b) { return b.first == offset; }))
            banks.emplace_back(offset, bits);
    }
    std::sort(banks.begin(), banks.end());

    for (const auto& [offset, bits] : banks)
    {
        // Pages 0 to 0xFE: an end page of 0xFF wraps to 0, and the sample never stops.
        for (size_t page = 0; page + pages <= 0xFF;)
        {
            const size_t from = offset + (page << 8), to = from + (pages << 8);
            if (to > maxSize)
                break;
            const auto clash = std::find_if(used.begin(), used.end(), [from, to](const auto& r) { return r.first < to && r.second > from; });
            if (clash != used.end())
            {
                // On past the range in the way, to the page after it.
                page = std::max(page + 1, (clash->second - offset + 0xFF) >> 8);
                continue;
            }
            if (romSize < to)
                romSize = std::min(maxSize, offset + 0x10000);
            sample.data.resize(bytes);
            sample.romOffset = static_cast<uint32_t>(offset);
            sample.bank = bits;
            sample.address = static_cast<uint16_t>(page << 8);
            used.emplace_back(from, to);
            return true;
        }
    }
    return false;
}

void writeSample(std::vector<uint8_t>& rom, const PadSample& sample)
{
    const size_t from = sample.romOffset + sample.address, to = (from + sample.data.size() + 0xFF) & ~size_t { 0xFF };
    if (sample.data.empty() || to > rom.size())
        return;
    std::copy(sample.data.begin(), sample.data.end(), rom.begin() + static_cast<long>(from));
    std::fill(rom.begin() + static_cast<long>(from + sample.data.size()), rom.begin() + static_cast<long>(to), uint8_t { 0x80 });
}

std::shared_ptr<const PcmBank> buildBank(const PadSet& pads)
{
    auto bank = std::make_shared<PcmBank>();
    constexpr int kBanks = static_cast<int>(SegaPcm::kRomSize / 0x10000);
    std::array<size_t, kBanks> used {};

    for (int pad = 0; pad < kPads; ++pad)
    {
        const PadSample& s = pads[pad];
        if (s.data.empty())
            continue;

        // The chip ends a sample on a 256-byte page, and an end page of
        // 0xFF wraps to 0, so each bank holds at most 0xFF00 bytes.
        const size_t want = (std::min(s.data.size(), kMaxSampleBytes) + 0xFF) & ~size_t { 0xFF };
        // First bank with room; failing that, the emptiest one.
        int best = -1;
        for (int b = 0; b < kBanks && best < 0; ++b)
            if (kMaxSampleBytes - used[b] >= want)
                best = b;
        if (best < 0)
            best = static_cast<int>(std::min_element(used.begin(), used.end()) - used.begin());

        const size_t room = kMaxSampleBytes - used[best];
        const size_t size = std::min(want, room);
        if (size < 0x100)
            continue;

        const size_t start = used[best];
        const size_t copy = std::min(size, s.data.size());
        std::copy_n(s.data.begin(), copy, bank->rom.begin() + static_cast<long>(best * 0x10000 + start));
        used[best] += size;

        PcmSlot& slot = bank->slots[pad];
        slot.valid = true;
        slot.bank = static_cast<uint8_t>(best);
        slot.start = static_cast<uint16_t>(start);
        slot.endPage = static_cast<uint8_t>((start + size) / 0x100 - 1);
        slot.step = s.step;
        slot.loop = s.loop && s.loopStart < copy;
        slot.loopStart = static_cast<uint16_t>(start + (slot.loop ? s.loopStart : 0));
    }
    return bank;
}

PadSet makeDefaultKit()
{
    PadSet kit;
    kit[0] = fromBuffer("Kick", kick());
    kit[1] = fromBuffer("Side Stick", stick());
    kit[2] = fromBuffer("Snare", snare(190.0, 0.07));
    kit[3] = fromBuffer("Clap", clap());
    kit[4] = fromBuffer("Snare 2", snare(230.0, 0.05));
    kit[5] = fromBuffer("Low Tom", tom(90.0));
    kit[6] = fromBuffer("Closed Hat", metal(0.08, 0.02));
    kit[7] = fromBuffer("Low Tom 2", tom(110.0));
    kit[8] = fromBuffer("Pedal Hat", metal(0.1, 0.03));
    kit[9] = fromBuffer("Mid Tom", tom(135.0));
    kit[10] = fromBuffer("Open Hat", metal(0.5, 0.15));
    kit[11] = fromBuffer("Mid Tom 2", tom(160.0));
    kit[12] = fromBuffer("High Tom", tom(190.0));
    kit[13] = fromBuffer("Crash", metal(1.4, 0.45));
    kit[14] = fromBuffer("High Tom 2", tom(225.0));
    kit[15] = fromBuffer("Ride", metal(1.2, 0.35));
    return kit;
}

} // namespace opm86
