#include "PreviewVoice.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{
constexpr float kChipScale = 1.0f / 32768.0f;
constexpr float kLevel = 0.7f; // the song player's FM and PCM level
} // namespace

PreviewVoice::PreviewVoice()
{
    regs_ = writtenRegs(blank_, 60, 0.0);
    lfoOnChip_.fill(-1);
    chip_.reset();
    prepare(48000.0);
}

void PreviewVoice::prepare(double hostRate)
{
    hostRate_ = hostRate;
    resampler_.setup(chipRate_, hostRate_);
    filter_.setup(15000.0, chipRate_);
    // As the song player sets its PCM up.
    pcmResampler_.setup(pcmSong_ ? pcmSong_->pcmRate : SegaPcm::kSampleRate, hostRate_);
    pcmFilter_.setup(14000.0, hostRate_);
}

void PreviewVoice::setPcm(std::shared_ptr<const VgmSong> song)
{
    pcmSong_ = std::move(song);
    pcm_.reset();
    if (pcmSong_)
        pcm_.setRom(pcmSong_->rom.data(), pcmSong_->rom.size(), pcmSong_->header.segaPcmInterface);
    else
        pcm_.setRom(nullptr, 0);
    pcmQuiet_ = true;
    pcmStopAt_ = UINT64_MAX;
    pcmSounding_.store(false, std::memory_order_relaxed);
    prepare(hostRate_);
}

void PreviewVoice::pcmOn(const PcmVoice& voice, double seconds)
{
    if (!pcmSong_)
        return;
    // Stopped first, so the new start address and flags take a fresh start.
    pcm_.write(0x86, pcm_.read(0x86) | 0x01);
    for (int i = 0; i < kPcmRegs; ++i)
        pcm_.write(pcmRegister(0, i), voice.regs[static_cast<size_t>(i)]);
    pcm_.write(0x86, voice.flags & 0xFE);
    pcmStopAt_ = seconds > 0.0 ? pcmSample_ + static_cast<uint64_t>(seconds * pcmSong_->pcmRate) : UINT64_MAX;
    if (pcmQuiet_)
    {
        pcmResampler_.reset();
        pcmFilter_.reset();
    }
    pcmQuiet_ = false;
    pcmSounding_.store(true, std::memory_order_relaxed);
}

void PreviewVoice::pcmOff()
{
    pcm_.write(0x86, pcm_.read(0x86) | 0x01);
}

void PreviewVoice::renderPcm(float* left, float* right, int count)
{
    constexpr int kChunk = 256;
    float l[kChunk], r[kChunk];
    float tail = 0.0f;
    for (int pos = 0; pos < count;)
    {
        const int n = std::min(count - pos, kChunk);
        pcmResampler_.process(l, r, n, [this](float& a, float& b) {
            if (pcmSample_++ >= pcmStopAt_)
                pcmOff(), pcmStopAt_ = UINT64_MAX;
            int32_t x = 0, y = 0;
            pcm_.generate(x, y);
            a = static_cast<float>(x) * kChipScale;
            b = static_cast<float>(y) * kChipScale;
        });
        for (int i = 0; i < n; ++i)
        {
            pcmFilter_.process(l[i], r[i]);
            left[pos + i] += l[i] * kLevel;
            right[pos + i] += r[i] * kLevel;
            tail = std::max({ tail, std::abs(l[i]), std::abs(r[i]) });
        }
        pos += n;
    }
    // Stopped, it adds nothing more once the filter's tail has died away.
    if (pcm_.read(0x86) & 0x01)
    {
        pcmSounding_.store(false, std::memory_order_relaxed);
        pcmQuiet_ = tail < 1e-7f;
    }
}

void PreviewVoice::setChip(double chipRate, double clockShift)
{
    chipRate_ = chipRate;
    clockShift_ = clockShift;
    chip_.reset();
    lfoOnChip_.fill(-1);
    channels_ = {};
    quiet_ = true;
    sounding_.store(false, std::memory_order_relaxed);
    pitch_.store(-1, std::memory_order_relaxed);
    prepare(hostRate_);
}

void PreviewVoice::write(int reg, int value)
{
    chip_.write(0, static_cast<uint8_t>(reg));
    chip_.write(1, static_cast<uint8_t>(value));
}

void PreviewVoice::writeChannel(int ch)
{
    auto regs = regs_;
    const Channel& c = channels_[static_cast<size_t>(ch)];
    const auto key = writtenRegs(blank_, c.key + c.fine / 64.0, clockShift_);
    regs[1] = key[1], regs[2] = key[2];
    regs[0] = static_cast<uint8_t>((regs[0] & 0x3F) | (c.sides & 0xC0));
    if (c.quieter != 0)
        for (size_t op = 0; op < 4; ++op)
            if ((carrierSlots(regs[0] & 7) >> op) & 1)
                regs[5 + 6 * op] = static_cast<uint8_t>(std::clamp((regs[5 + 6 * op] & 0x7F) + c.quieter, 0, 127));
    for (int i = 0; i < kFmRegs; ++i)
        write(fmRegister(ch, i), regs[static_cast<size_t>(i)]);
}

void PreviewVoice::writeLfo()
{
    // Register 0x19 takes the vibrato depth with bit 7 set, the tremolo depth without.
    static constexpr int kRegs[4] = { 0x18, 0x19, 0x19, 0x1B };
    for (size_t i = 0; i < 4; ++i)
    {
        const int value = i == 1 ? 0x80 | lfo_[i] : lfo_[i];
        if (lfoOnChip_[i] != value)
            write(kRegs[i], value), lfoOnChip_[i] = value;
    }
}

void PreviewVoice::setPatch(const FmPatch& patch)
{
    regs_ = writtenRegs(patch, 60, clockShift_);
    patchKeys_ = keyOnMask(patch);
    keys_ = static_cast<uint8_t>(patchKeys_ & ~(muted_ << 3));
    lfo_ = { patch.lfoFreq, static_cast<uint8_t>(patch.pmd & 0x7F), static_cast<uint8_t>(patch.amd & 0x7F), static_cast<uint8_t>(patch.lfoWave & 3) };
    writeLfo();
    // The channels sounding take the new sound at once; a held key keeps its
    // operators keyed, less those switched off, as the song player does.
    for (int ch = 0; ch < kChannels; ++ch)
    {
        const Channel& c = channels_[static_cast<size_t>(ch)];
        if (c.key < 0)
            continue;
        writeChannel(ch);
        if (c.held && !c.pending)
            write(0x08, keys_ | ch);
    }
}

void PreviewVoice::setMuted(uint8_t operators)
{
    muted_ = static_cast<uint8_t>(operators & 0xF);
    keys_ = static_cast<uint8_t>(patchKeys_ & ~(muted_ << 3));
    for (int ch = 0; ch < kChannels; ++ch)
    {
        const Channel& c = channels_[static_cast<size_t>(ch)];
        if (c.key >= 0 && c.held && !c.pending)
            write(0x08, keys_ | ch);
    }
}

void PreviewVoice::noteOn(int key, double seconds, int fine, int quieter, uint8_t sides)
{
    if (key < 0 || key > 127)
        return;
    // The channel playing the key already, else the one let go longest ago,
    // else the one keyed longest ago.
    int ch = -1;
    for (int k = 0; ch < 0 && k < kChannels; ++k)
        if (channels_[static_cast<size_t>(k)].held && channels_[static_cast<size_t>(k)].key == key)
            ch = k;
    if (ch < 0)
    {
        ch = 0;
        for (int k = 1; k < kChannels; ++k)
        {
            const Channel &c = channels_[static_cast<size_t>(k)], &best = channels_[static_cast<size_t>(ch)];
            if ((!c.held && best.held) || (c.held == best.held && c.since < best.since))
                ch = k;
        }
    }
    Channel& c = channels_[static_cast<size_t>(ch)];
    const bool keyed = c.held;
    c.key = key;
    c.fine = fine;
    c.quieter = quieter;
    c.sides = sides;
    c.held = true;
    c.since = ++clock_;
    c.releaseAt = seconds > 0.0 ? sample_ + static_cast<uint64_t>(seconds * chipRate_) : UINT64_MAX;
    lastKey_ = key;
    writeLfo();
    writeChannel(ch);
    // A key already down is keyed off for a chip sample first, so the key on
    // starts its envelopes again.
    write(0x08, keyed ? ch : keys_ | ch);
    c.pending = keyed;
    quiet_ = false;
}

void PreviewVoice::release(int ch)
{
    Channel& c = channels_[static_cast<size_t>(ch)];
    if (!c.held)
        return;
    c.held = c.pending = false;
    c.since = ++clock_;
    c.releaseAt = UINT64_MAX;
    write(0x08, ch);
}

void PreviewVoice::noteOff(int key)
{
    for (int ch = 0; ch < kChannels; ++ch)
        if (channels_[static_cast<size_t>(ch)].key == key)
            release(ch);
}

void PreviewVoice::releaseAll()
{
    for (int ch = 0; ch < kChannels; ++ch)
        release(ch);
}

void PreviewVoice::stop()
{
    pcmOff();
    // Release rate 15 and the first decay's level at the bottom on every
    // operator, register 0xE0 of each slot, end a sound in a few
    // milliseconds. A channel with no key is left out by
    // setPatch, so the tail keeps that release.
    for (int ch = 0; ch < kChannels; ++ch)
    {
        Channel& c = channels_[static_cast<size_t>(ch)];
        if (c.key < 0)
            continue;
        release(ch);
        for (int op = 0; op < 4; ++op)
            write(0xE0 + ch + 8 * op, 0xFF);
        c.key = -1;
    }
}

void PreviewVoice::step(float& left, float& right)
{
    for (int ch = 0; ch < kChannels; ++ch)
        if (channels_[static_cast<size_t>(ch)].held && sample_ >= channels_[static_cast<size_t>(ch)].releaseAt)
            release(ch);
    ymfm::ym2151::output_data data;
    chip_.generate(&data, 0xFF);
    // The keys waiting for a sample keyed off have had it.
    for (int ch = 0; ch < kChannels; ++ch)
        if (Channel& c = channels_[static_cast<size_t>(ch)]; c.pending)
            write(0x08, keys_ | ch), c.pending = false;
    left = static_cast<float>(data.data[0]) * kChipScale;
    right = static_cast<float>(data.data[1]) * kChipScale;
    const uint32_t at = probeAt_.load(std::memory_order_relaxed);
    probeOutput_[at % kProbeSize].store((left + right) * 0.5f, std::memory_order_relaxed);
    probeAt_.store(at + 1, std::memory_order_release);
    filter_.process(left, right);
    if (sample_++ % 64 == 0)
        updateProbe();
}

void PreviewVoice::updateProbe()
{
    float best[4] {}, levels[4];
    int bestStages[4] {}, stages[4];
    float loudest = 0.0f;
    int lowest = -1;
    for (int ch = 0; ch < kChannels; ++ch)
    {
        const Channel& c = channels_[static_cast<size_t>(ch)];
        if (c.held)
            lowest = lowest < 0 ? c.key : std::min(lowest, c.key);
        chip_.envelopes(static_cast<uint32_t>(ch), levels, stages);
        const float sum = levels[0] + levels[1] + levels[2] + levels[3];
        if (sum > loudest)
            loudest = sum, std::copy(levels, levels + 4, best), std::copy(stages, stages + 4, bestStages);
    }
    quiet_ = lowest < 0 && loudest <= 0.0f;
    for (size_t op = 0; op < 4; ++op)
        probeLevel_[op].store(best[op], std::memory_order_relaxed), probeStage_[op].store(bestStages[op], std::memory_order_relaxed);
    sounding_.store(!quiet_, std::memory_order_relaxed);
    pitch_.store(quiet_ ? -1 : lowest >= 0 ? lowest : lastKey_, std::memory_order_relaxed);
}

void PreviewVoice::render(float* left, float* right, int count)
{
    if (!pcmQuiet_)
        renderPcm(left, right, count);
    // Silent and nothing keyed: the chip would add nothing.
    if (quiet_)
        return;
    constexpr int kChunk = 256;
    float l[kChunk], r[kChunk];
    for (int pos = 0; pos < count;)
    {
        const int n = std::min(count - pos, kChunk);
        resampler_.process(l, r, n, [this](float& a, float& b) { step(a, b); });
        for (int i = 0; i < n; ++i)
        {
            left[pos + i] += l[i] * kLevel;
            right[pos + i] += r[i] * kLevel;
        }
        pos += n;
    }
}

void PreviewVoice::probeOutput(float* out, int count) const
{
    const uint32_t end = probeAt_.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        out[i] = probeOutput_[(end - static_cast<uint32_t>(count) + static_cast<uint32_t>(i)) % kProbeSize].load(std::memory_order_relaxed);
}

void PreviewVoice::probeEnvelopes(float* levels, int* stages) const
{
    for (size_t op = 0; op < 4; ++op)
    {
        levels[op] = probeLevel_[op].load(std::memory_order_relaxed);
        stages[op] = probeStage_[op].load(std::memory_order_relaxed);
    }
}

} // namespace opm86
