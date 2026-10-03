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

PadSample makeSample(const std::string& name, const float* audio, size_t length, double sampleRate)
{
    PadSample s;
    s.name = name;
    if (audio == nullptr || length == 0 || sampleRate <= 0)
        return s;

    // Band-limit before dropping to the chip rate, then step through linearly.
    std::vector<double> source(audio, audio + length);
    if (sampleRate > kNativeRate * 1.01)
    {
        LowPass a(kNativeRate * 0.45, sampleRate), b(kNativeRate * 0.45, sampleRate);
        for (double& x : source)
            x = b.process(a.process(x));
    }

    const double ratio = sampleRate / kNativeRate;
    const auto outLength = std::min(kMaxSampleBytes, static_cast<size_t>(static_cast<double>(length) / ratio));
    s.data.reserve(outLength);
    for (size_t i = 0; i < outLength; ++i)
    {
        const double pos = static_cast<double>(i) * ratio;
        const auto i0 = static_cast<size_t>(pos);
        const size_t i1 = std::min(i0 + 1, length - 1);
        const double frac = pos - static_cast<double>(i0);
        const double x = source[i0] + (source[i1] - source[i0]) * frac;
        const long q = std::lround(x * 127.0) + 0x80;
        s.data.push_back(static_cast<uint8_t>(std::clamp(q, 0L, 255L)));
    }

    // Trailing silence would only waste ROM.
    while (!s.data.empty() && std::abs(static_cast<int>(s.data.back()) - 0x80) <= 1)
        s.data.pop_back();
    return s;
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
