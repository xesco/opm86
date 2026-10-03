#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{

constexpr float kChipScale = 1.0f / 32768.0f;

} // namespace

Engine::Engine()
{
    setBank(buildBank(makeDefaultKit()));
    prepare(48000.0);
}

void Engine::prepare(double hostRate)
{
    hostRate_ = hostRate;
    fmResampler_.setup(FmSection::kSampleRate, hostRate);
    pcmResampler_.setup(SegaPcm::kSampleRate, hostRate);
    // The FM output is band-limited before decimation; the PCM output,
    // which is upsampled, has its images removed afterwards.
    fmFilter_.setup(15000.0, FmSection::kSampleRate);
    pcmFilter_.setup(14000.0, hostRate);
    reset();
}

void Engine::reset()
{
    rpn_.fill({ 127, 127 });
    chipPad_.fill(0);
    chipAge_.fill(0);
    drumPan_ = -1;
    drumBend_ = 0.0f;
    lastPad_ = -1;
    drumGate_ = false;
    fm_.reset();
    pcm_.reset();
    fmQueue_.clear();
    pcmQueue_.clear();
    fmSamples_ = pcmSamples_ = hostSamples_ = 0;
    fmResampler_.reset();
    pcmResampler_.reset();
    fmFilter_.reset();
    pcmFilter_.reset();
}

void Engine::setBank(std::shared_ptr<const PcmBank> bank)
{
    if (!bank)
        return;
    bank_ = std::move(bank);
    pcm_.reset();
    pcm_.setRom(bank_->rom.data());
}

void Engine::handleMidi(const uint8_t* data, int size)
{
    if (size < 1)
        return;
    const int part = data[0] & 0x0F;
    const int channel = part + 1;
    const int d1 = size > 1 ? data[1] & 0x7F : 0;
    const int d2 = size > 2 ? data[2] & 0x7F : 0;
    const bool drums = channel == drumChannel_ || drumChannel_ == kDrumsOnAllChannels;

    // A note on with velocity 0 is a note off.
    int status = data[0] & 0xF0;
    if (status == 0x90 && d2 == 0)
        status = 0x80;

    switch (status)
    {
    case 0x90:
        if (drums)
            triggerPad(d1 - kFirstPadNote, d2);
        else
            fm_.noteOn(part, d1, d2);
        break;
    case 0x80:
        if (drums)
        {
            // A drum note off ends a looping sample, or any sample in gated mode.
            const int pad = d1 - kFirstPadNote;
            if (pad >= 0 && pad < kPads && (drumGate_ || bank_->slots[static_cast<size_t>(pad)].loop))
                stopPad(pad, false);
        }
        else
            fm_.noteOff(part, d1);
        break;
    case 0xB0:
        // RPN 0 sets the pitch bend range: CC 101 and 100 select it, CC 6 sets it.
        if (d1 == 101)
            rpn_[static_cast<size_t>(part)].first = d2;
        else if (d1 == 100)
            rpn_[static_cast<size_t>(part)].second = d2;
        else if (d1 == 6 && rpn_[static_cast<size_t>(part)] == std::pair<int, int> { 0, 0 })
            fm_.setPartBendRange(part, d2);
        if (drums)
        {
            if (d1 == 10)
                drumPan_ = d2;
            else if (d1 == kDrumGateController)
                drumGate_ = d2 >= 64;
            break;
        }
        if (d1 == 1)
            fm_.setModWheel(part, static_cast<float>(d2) / 127.0f);
        else if (d1 == 10)
            fm_.setPartPan(part, d2 < 43 ? FmPan::Left : d2 > 85 ? FmPan::Right : FmPan::Both);
        else if (d1 == 11)
            fm_.setPartAttenuation(part, 127 - d2);
        else if (d1 == 64)
            fm_.setSustain(part, d2 >= 64);
        else if (d1 == 120 || d1 == 123)
            fm_.allNotesOff();
        break;
    case 0xE0:
    {
        const float bend = static_cast<float>((d2 << 7 | d1) - 8192) / 8192.0f;
        if (drums)
            setDrumBend(bend * static_cast<float>(fm_.partBendRange(part)));
        else
            fm_.setPitchBend(part, bend);
        break;
    }
    case 0xA0:
        // Polyphonic aftertouch sets the volume of a playing pad.
        if (drums && d1 >= kFirstPadNote && d1 < kFirstPadNote + kPads)
            setPadVolume(d1 - kFirstPadNote, d2);
        break;
    case 0xC0:
        if (!drums && !library_.empty())
            fm_.setPartPatch(part, library_[static_cast<size_t>(d1) % library_.size()]);
        break;
    default:
        break;
    }
}

void Engine::triggerPad(int pad, int velocity)
{
    if (pad < 0 || pad >= kPads)
        return;
    const PcmSlot& slot = bank_->slots[static_cast<size_t>(pad)];
    if (!slot.valid)
        return;
    const PadParams& p = padParams_[static_cast<size_t>(pad)];

    if (p.choke != 0)
        for (int other = 0; other < kPads; ++other)
            if (other != pad && padParams_[static_cast<size_t>(other)].choke == p.choke)
                stopPad(other, true);

    // Any free chip channel, else the one that started longest ago, so a
    // pad can sound twice at once, as the game's samples do.
    int ch = -1;
    for (int c = 0; c < SegaPcm::kChannels && ch < 0; ++c)
        if (pcm_.read(c * 8 + 0x86) & 1)
            ch = c;
    if (ch < 0)
        ch = static_cast<int>(std::min_element(chipAge_.begin(), chipAge_.end()) - chipAge_.begin());
    chipPad_[static_cast<size_t>(ch)] = pad;
    chipAge_[static_cast<size_t>(ch)] = ++hitCount_;
    lastPad_ = pad;

    // Pitch bend on the drum channel shifts the pitch of each new hit.
    const double step = slot.step * std::pow(2.0, (p.tune + drumBend_) / 12.0);

    // Written the way the game's driver does: stop the channel, set it up,
    // then clear the off bit.
    const int base = ch * 8;
    const auto flags = static_cast<uint8_t>(slot.bank << 4 | (slot.loop ? 0 : 0x02));
    pcm_.write(base + 0x86, flags | 0x01);
    setChannelVolume(ch, velocity);
    pcm_.write(base + 0x04, static_cast<uint8_t>(slot.loopStart));
    pcm_.write(base + 0x05, static_cast<uint8_t>(slot.loopStart >> 8));
    pcm_.write(base + 0x06, slot.endPage);
    pcm_.write(base + 0x07, static_cast<uint8_t>(std::clamp(std::lround(step), 1L, 255L)));
    pcm_.write(base + 0x84, static_cast<uint8_t>(slot.start));
    pcm_.write(base + 0x85, static_cast<uint8_t>(slot.start >> 8));
    pcm_.write(base + 0x86, flags);
}

void Engine::setDrumBend(float semitones)
{
    // The bend sets the pitch of new hits, and of the latest hit of the
    // pad that the last note on or aftertouch named.
    drumBend_ = semitones;
    if (lastPad_ < 0)
        return;
    const int ch = latestChannel(lastPad_);
    if (ch < 0)
        return;
    const PcmSlot& slot = bank_->slots[static_cast<size_t>(lastPad_)];
    const double step = slot.step * std::pow(2.0, (padParams_[static_cast<size_t>(lastPad_)].tune + drumBend_) / 12.0);
    pcm_.write(ch * 8 + 0x07, static_cast<uint8_t>(std::clamp(std::lround(step), 1L, 255L)));
}

int Engine::latestChannel(int pad) const
{
    int found = -1;
    for (int c = 0; c < SegaPcm::kChannels; ++c)
        if (chipPad_[static_cast<size_t>(c)] == pad && !(pcm_.read(c * 8 + 0x86) & 1)
            && (found < 0 || chipAge_[static_cast<size_t>(c)] > chipAge_[static_cast<size_t>(found)]))
            found = c;
    return found;
}

void Engine::setPadVolume(int pad, int volume)
{
    if (pad < 0 || pad >= kPads)
        return;
    lastPad_ = pad;
    const int ch = latestChannel(pad);
    if (ch >= 0)
        setChannelVolume(ch, volume);
}

void Engine::setChannelVolume(int ch, int volume)
{
    // The drum channel's pan (CC 10), once sent, replaces the pad's pan setting.
    const PadParams& p = padParams_[static_cast<size_t>(chipPad_[static_cast<size_t>(ch)])];
    const float pan = drumPan_ >= 0 ? static_cast<float>(std::clamp(drumPan_ - 64, -63, 63)) / 63.0f
                                    : static_cast<float>(std::clamp(p.pan, -63, 63)) / 63.0f;
    const float gain = static_cast<float>(p.level) * static_cast<float>(volume) / 127.0f;
    const auto left = std::lround(gain * std::min(1.0f, 1.0f - pan));
    const auto right = std::lround(gain * std::min(1.0f, 1.0f + pan));
    pcm_.write(ch * 8 + 0x02, static_cast<uint8_t>(std::clamp(left, 0L, 127L)));
    pcm_.write(ch * 8 + 0x03, static_cast<uint8_t>(std::clamp(right, 0L, 127L)));
}

void Engine::stopPad(int pad, bool all)
{
    if (!all)
    {
        // The oldest playing hit, as MIDI pairs each note off with the
        // earliest note on of the same note.
        int ch = -1;
        for (int c = 0; c < SegaPcm::kChannels; ++c)
            if (chipPad_[static_cast<size_t>(c)] == pad && !(pcm_.read(c * 8 + 0x86) & 1)
                && (ch < 0 || chipAge_[static_cast<size_t>(c)] < chipAge_[static_cast<size_t>(ch)]))
                ch = c;
        if (ch >= 0)
            pcm_.write(ch * 8 + 0x86, pcm_.read(ch * 8 + 0x86) | 0x01);
        return;
    }
    for (int c = 0; c < SegaPcm::kChannels; ++c)
        if (chipPad_[static_cast<size_t>(c)] == pad)
            pcm_.write(c * 8 + 0x86, pcm_.read(c * 8 + 0x86) | 0x01);
}

void Engine::scheduleMidi(const uint8_t* data, int size, double hostTime)
{
    if (size < 1)
        return;
    // Drum messages go to the PCM chip's queue, all others to the FM chip's.
    const int channel = (data[0] & 0x0F) + 1;
    const bool drums = (data[0] & 0xF0) != 0xF0
                       && (channel == drumChannel_ || drumChannel_ == kDrumsOnAllChannels);
    const double rate = drums ? SegaPcm::kSampleRate : FmSection::kSampleRate;

    Pending p {};
    p.chipSample = static_cast<uint64_t>(std::max(0.0, std::ceil(hostTime * rate / hostRate_ - 1e-9)));
    p.size = std::min(size, 3);
    std::copy_n(data, p.size, p.bytes);
    if (!(drums ? pcmQueue_ : fmQueue_).push(p))
        handleMidi(data, size); // queue full: late rather than lost
}

void Engine::applyDue(Queue& queue, uint64_t chipSample)
{
    while (const Pending* p = queue.front())
    {
        if (p->chipSample > chipSample)
            break;
        handleMidi(p->bytes, p->size);
        queue.pop();
    }
}

void Engine::render(float* left, float* right, int count)
{
    hostSamples_ += static_cast<uint64_t>(std::max(count, 0));
    constexpr int kChunk = 256;
    float fmL[kChunk], fmR[kChunk], pcmL[kChunk], pcmR[kChunk];

    while (count > 0)
    {
        const int n = std::min(count, kChunk);

        fmResampler_.process(fmL, fmR, n, [this](float& l, float& r) {
            applyDue(fmQueue_, fmSamples_++);
            int32_t a = 0, b = 0;
            fm_.generate(a, b);
            l = static_cast<float>(a) * kChipScale;
            r = static_cast<float>(b) * kChipScale;
            fmFilter_.process(l, r);
        });

        pcmResampler_.process(pcmL, pcmR, n, [this](float& l, float& r) {
            applyDue(pcmQueue_, pcmSamples_++);
            int32_t a = 0, b = 0;
            pcm_.generate(a, b);
            l = static_cast<float>(a) * kChipScale;
            r = static_cast<float>(b) * kChipScale;
        });

        for (int i = 0; i < n; ++i)
        {
            pcmFilter_.process(pcmL[i], pcmR[i]);
            left[i] = fmL[i] * fmLevel_ + pcmL[i] * pcmLevel_;
            right[i] = fmR[i] * fmLevel_ + pcmR[i] * pcmLevel_;
        }

        left += n;
        right += n;
        count -= n;
    }
}

} // namespace opm86
