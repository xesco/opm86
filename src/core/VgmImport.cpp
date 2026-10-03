#include "VgmImport.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <map>
#include <tuple>

namespace opm86
{

namespace
{

constexpr int kSlotOffset[4] = { 0, 16, 8, 24 }; // M1, C1, M2, C2

uint32_t u32(const std::vector<uint8_t>& d, size_t at)
{
    if (at + 4 > d.size())
        return 0;
    return static_cast<uint32_t>(d[at]) | static_cast<uint32_t>(d[at + 1]) << 8
           | static_cast<uint32_t>(d[at + 2]) << 16 | static_cast<uint32_t>(d[at + 3]) << 24;
}

bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    z_stream z {};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
        return false;
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = static_cast<uInt>(in.size());

    std::array<uint8_t, 65536> chunk {};
    int result = Z_OK;
    while (result == Z_OK)
    {
        z.next_out = chunk.data();
        z.avail_out = static_cast<uInt>(chunk.size());
        result = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), chunk.data(), chunk.data() + (chunk.size() - z.avail_out));
    }
    inflateEnd(&z);
    return result == Z_STREAM_END;
}

// Length in bytes of each command, opcode included; 0 = unknown.
size_t commandLength(uint8_t op)
{
    if (op >= 0x30 && op <= 0x3F) return 2;
    if (op >= 0x40 && op <= 0x4E) return 3;
    if (op == 0x4F || op == 0x50) return 2;
    if (op >= 0x51 && op <= 0x5F) return 3;
    if (op == 0x61) return 3;
    if (op == 0x62 || op == 0x63) return 1;
    if (op == 0x68) return 12;
    if (op >= 0x70 && op <= 0x8F) return 1;
    if (op == 0x90 || op == 0x91 || op == 0x95) return 5;
    if (op == 0x92) return 6;
    if (op == 0x93) return 11;
    if (op == 0x94) return 2;
    if (op >= 0xA0 && op <= 0xBF) return 3;
    if (op >= 0xC0 && op <= 0xDF) return 4;
    if (op >= 0xE0) return 5;
    return 0;
}

struct PatchEntry
{
    FmPatch patch;
    int uses = 0;
};

struct SampleEntry
{
    uint32_t romOffset = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    bool loop = false;
    uint32_t loopStart = 0;
    std::map<uint8_t, int> steps;
    int uses = 0;
};

using SampleKey = std::pair<uint32_t, uint32_t>; // ROM bank offset, start address

// A channel's settings while being recorded, before patches are sorted.
struct RawState
{
    uint32_t time = 0;
    int patch = 0;        // unsorted patch index
    FmPatch registers;    // as the channel had them
    double carrierTl = 0; // mean carrier level
};

// A note or hit while it is being recorded, before patches and samples are sorted.
struct RawNote
{
    VgmNote note;
    std::vector<RawState> state;
};

struct RawHit
{
    VgmHit hit;
    SampleKey sample;
    bool closed = false;
    uint32_t naturalEnd = 0; // when a one-shot sample ends by itself
};

class Collector : public VgmVisitor
{
public:
    Collector()
    {
        for (int ch = 0; ch < 16; ++ch)
            pcm_[ch * 8 + 0x86] = 0x01;
        openNote_.fill(-1);
        lastNote_.fill(-1);
        openHit_.fill(-1);
        restartAt_.fill(kNever);
    }

    void begin(const VgmHeader& header) override
    {
        header_ = header;
        started_ = true;
        pcmRate_ = (header.segaPcmClock ? header.segaPcmClock : SegaPcm::kClock) / 128.0;
        // The game's notes sound at the real clock, which differs from the
        // 3.579545 MHz the key codes are defined for.
        clockShift_ = header.ym2151Clock ? 12.0 * std::log2(header.ym2151Clock / 3579545.0) : 0.0;
        // Same decoding as MAME: low byte is the bank shift, bits 16-23 extend the mask.
        const uint32_t pcmInterface = header.segaPcmInterface;
        shift_ = pcmInterface ? pcmInterface & 0xFF : 12;
        mask_ = 0x70 | ((pcmInterface >> 16) & 0xFC);
    }

    bool started() const { return started_; }
    const VgmHeader& header() const { return header_; }

    void wait(uint32_t samples) override
    {
        if (samples == 0)
            return;
        // A driver writes a patch one register at a time, so the channel's
        // settings are read once its writes for this instant are done.
        flush();
        time_ += samples;
    }

    void ym2151(uint8_t reg, uint8_t value) override
    {
        const uint8_t before = ym_[reg];
        if (reg == 0x19)
            (value & 0x80 ? pmd_ : amd_) = value & 0x7F;
        else
            ym_[reg] = value;
        if (before != value && ((reg >= 0x20 && reg < 0x28) || (reg >= 0x38 && reg < 0x40) || reg >= 0x40))
            dirty_[reg & 7] = true;

        if (reg == 0x08)
        {
            const int ch = value & 7;
            closeNote(ch);
            if ((value & 0x78) != 0)
                openNote(ch);
        }
        else if (reg >= 0x28 && reg < 0x38)
        {
            // A key code or fraction change moves the pitch of a held note.
            const int ch = reg & 7;
            if (openNote_[ch] >= 0)
            {
                auto& pitch = notes_[static_cast<size_t>(openNote_[ch])].note.pitch;
                const double now = notePitch(ch);
                if (std::abs(now - pitch.back().second) > 1e-6)
                {
                    if (pitch.back().first == time_)
                        pitch.back().second = now;
                    else
                        pitch.emplace_back(time_, now);
                }
            }
        }
    }

    void segaPcm(uint16_t offset, uint8_t value) override
    {
        const int reg = offset & 0xFF;
        const uint8_t before = pcm_[reg];
        pcm_[reg] = value;
        const int ch = (reg >> 3) & 15;
        const int field = reg & 0x87;
        if (field == 0x02 || field == 0x03 || field == 0x07)
        {
            // A volume or pitch change of a playing sample: a fade, a pan or a bend.
            if (openHit_[ch] >= 0 && before != value)
            {
                auto& state = hits_[static_cast<size_t>(openHit_[ch])].hit.state;
                const VgmHitState now = hitState(ch);
                if (state.back().time == time_)
                    state.back() = now;
                else
                    state.push_back(now);
            }
            return;
        }
        // On a playing channel, a new address restarts the sample once both
        // of its bytes are written; the later key on only keeps it running.
        if ((field == 0x84 || field == 0x85) && playing(ch))
            restartAt_[ch] = time_;
        if (field != 0x86)
            return;
        if (!(value & 1))
            openHit(ch, !(before & 1) && restartAt_[ch] != kNever ? restartAt_[ch] : time_);
        else if (!(before & 1))
            closeHit(ch, time_);
        restartAt_[ch] = kNever;
    }

    void pcmRom(const uint8_t* data, size_t size, uint32_t romSize, uint32_t start) override
    {
        if (rom_.size() < std::max<size_t>(romSize, start + size))
            rom_.resize(std::max<size_t>(romSize, start + size), 0x80);
        std::copy_n(data, size, rom_.begin() + static_cast<long>(start));
    }

    void finish(VgmImport& out, const std::string& name, double pcmRate)
    {
        flush();
        out.length = time_;

        // Patches a note starts with, most used first, and where each old
        // index went. A register state no note starts with is only a patch
        // half written by the driver, and gets no entry of its own.
        std::vector<int> patchOrder;
        for (size_t i = 0; i < patches_.size(); ++i)
            if (patches_[i].uses > 0)
                patchOrder.push_back(static_cast<int>(i));
        std::stable_sort(patchOrder.begin(), patchOrder.end(),
                         [&](int a, int b) { return patches_[static_cast<size_t>(a)].uses > patches_[static_cast<size_t>(b)].uses; });
        std::vector<int> patchIndex(patches_.size(), -1);
        for (size_t i = 0; i < patchOrder.size(); ++i)
        {
            FmPatch p = patches_[static_cast<size_t>(patchOrder[i])].patch;
            p.name = name + " FM " + std::to_string(i + 1);
            p.group = name;
            out.patches.push_back(p);
            patchIndex[static_cast<size_t>(patchOrder[i])] = static_cast<int>(i);
        }

        // The next note on each note's channel, which a half-written patch
        // during a release is being loaded for.
        std::vector<int> nextOnChannel(notes_.size(), -1);
        std::array<int, 8> latest {};
        latest.fill(-1);
        for (size_t i = 0; i < notes_.size(); ++i)
        {
            const int ch = notes_[i].note.channel;
            if (latest[static_cast<size_t>(ch)] >= 0)
                nextOnChannel[static_cast<size_t>(latest[static_cast<size_t>(ch)])] = static_cast<int>(i);
            latest[static_cast<size_t>(ch)] = static_cast<int>(i);
        }

        // A note's level is its carrier attenuation against the loudest use
        // of its patch, in the chip's 0.75 dB steps.
        for (size_t k = 0; k < notes_.size(); ++k)
        {
            RawNote& raw = notes_[k];
            VgmNote note = raw.note;
            const uint32_t end = note.end > note.start ? note.end : time_;
            int current = raw.state.front().patch;
            for (const RawState& r : raw.state)
            {
                int source = r.patch;
                if (patches_[static_cast<size_t>(source)].uses == 0)
                {
                    const int next = nextOnChannel[k];
                    source = r.time >= end && next >= 0 ? notes_[static_cast<size_t>(next)].state.front().patch : current;
                }
                current = source;
                const FmPatch& patch = patches_[static_cast<size_t>(source)].patch;
                VgmNoteState state;
                state.time = r.time;
                state.patch = patchIndex[static_cast<size_t>(source)];
                state.attenuation = std::clamp(static_cast<int>(std::lround(r.carrierTl - carrierTl(patch))), 0, 127);
                state.pan = r.registers.pan;
                const bool changed = note.state.empty() || note.state.back().patch != state.patch
                                     || note.state.back().attenuation != state.attenuation
                                     || note.state.back().pan != state.pan;
                if (changed)
                    note.state.push_back(state);
            }
            note.patch = note.state.front().patch;
            if (note.end <= note.start)
                note.end = time_;
            out.notes.push_back(std::move(note));
        }

        std::vector<SampleKey> sampleOrder;
        for (auto& [key, entry] : samples_)
            sampleOrder.push_back(key);
        std::stable_sort(sampleOrder.begin(), sampleOrder.end(),
                         [&](const SampleKey& a, const SampleKey& b) { return samples_[a].uses > samples_[b].uses; });
        std::map<SampleKey, int> sampleIndex;

        for (size_t i = 0; i < sampleOrder.size(); ++i)
        {
            const SampleEntry& s = samples_[sampleOrder[i]];
            sampleIndex[sampleOrder[i]] = static_cast<int>(i);
            PadSample pad;
            pad.name = name + " PCM " + std::to_string(i + 1);
            pad.group = name;
            for (uint32_t a = s.start; a < s.end; ++a)
            {
                const size_t at = s.romOffset + a;
                pad.data.push_back(at < rom_.size() ? rom_[at] : 0x80);
            }
            if (pad.data.size() > kMaxSampleBytes)
                pad.data.resize(kMaxSampleBytes);
            pad.step = std::max_element(s.steps.begin(), s.steps.end(),
                                        [](auto& a, auto& b) { return a.second < b.second; })->first;
            pad.loop = s.loop && s.loopStart >= s.start && s.loopStart < s.end;
            pad.loopStart = pad.loop ? s.loopStart - s.start : 0;
            out.samples.push_back(std::move(pad));
        }

        // A one-shot sample ends by itself; the log only shows explicit stops.
        for (RawHit& raw : hits_)
        {
            VgmHit hit = raw.hit;
            hit.sample = sampleIndex[raw.sample];
            const PadSample& pad = out.samples[static_cast<size_t>(hit.sample)];
            // At the song's end a looping sample stops; a one-shot plays out.
            if (!raw.closed)
                hit.end = pad.loop ? time_ : kNever;
            else
            {
                // Settings written just before a new sound starts on the
                // channel are that sound's, written ahead; the hit they land
                // on plays them for only a few samples.
                constexpr uint32_t kAhead = 4;
                while (hit.state.size() > 1 && hit.state.back().time + kAhead >= hit.end)
                    hit.state.pop_back();
            }
            if (!pad.loop)
            {
                const double seconds = static_cast<double>(pad.data.size()) * 256.0 / std::max<uint8_t>(hit.state.front().step, 1) / pcmRate;
                hit.end = std::min(hit.end, hit.start + static_cast<uint32_t>(seconds * kVgmRate) + 1);
            }
            out.hits.push_back(hit);
        }
    }

private:
    // The sounding pitch of a channel as a MIDI note number with a fraction.
    double notePitch(int ch) const
    {
        const int kc = ym_[0x28 + ch];
        const int code = kc & 15;
        // Codes 3, 7, 11 and 15 are unused and play as the code above them.
        const double semitone = (kc >> 4 & 7) * 12 + code - code / 4 + (ym_[0x30 + ch] >> 2) / 64.0;
        return 69.0 + semitone - 56.0 + clockShift_;
    }

    static double carrierTl(const FmPatch& p)
    {
        double sum = 0;
        int count = 0;
        for (int i = 0; i < 4; ++i)
            if ((kCarrierMask[p.alg] >> i) & 1)
                sum += p.op[static_cast<size_t>(i)].tl, ++count;
        return count ? sum / count : 0.0;
    }

    void openNote(int ch)
    {
        const FmPatch patch = channelPatch(ch);
        RawNote raw;
        raw.note.start = time_;
        raw.note.channel = ch;
        raw.note.pitch.emplace_back(time_, notePitch(ch));
        raw.state.push_back({ time_, recordPatch(patch, true), patch, carrierTl(patch) });
        openNote_[ch] = lastNote_[ch] = static_cast<int>(notes_.size());
        dirty_[ch] = false;
        notes_.push_back(std::move(raw));
    }

    // Record the settings of each channel that changed since its last note began.
    void flush()
    {
        for (int ch = 0; ch < 8; ++ch)
        {
            if (!dirty_[ch] || lastNote_[ch] < 0)
                continue;
            dirty_[ch] = false;
            auto& states = notes_[static_cast<size_t>(lastNote_[ch])].state;
            const FmPatch patch = channelPatch(ch);
            const RawState& last = states.back();
            const bool samePatchNow = samePatch(last.registers, patch, kCarrierMask[patch.alg]);
            const int index = samePatchNow ? last.patch : recordPatch(patch, false);
            if (samePatchNow)
                lowerCarrierTl(patches_[static_cast<size_t>(index)].patch, patch);
            const double tl = carrierTl(patch);
            if (index == last.patch && tl == last.carrierTl && patch.pan == last.registers.pan)
                continue;
            const RawState next { time_, index, patch, tl };
            if (last.time == time_)
                states.back() = next;
            else
                states.push_back(next);
        }
    }

    void closeNote(int ch)
    {
        if (openNote_[ch] < 0)
            return;
        notes_[static_cast<size_t>(openNote_[ch])].note.end = time_;
        openNote_[ch] = -1;
    }

    // Whether the chip channel still sounds: the log shows only the stops
    // the game writes, so a one-shot sample's own end is worked out.
    bool playing(int ch) const
    {
        if ((pcm_[ch * 8 + 0x86] & 1) || openHit_[ch] < 0)
            return false;
        return time_ < hits_[static_cast<size_t>(openHit_[ch])].naturalEnd;
    }

    void openHit(int ch, uint32_t start)
    {
        closeHit(ch, start);
        RawHit raw;
        raw.hit.start = start;
        raw.hit.state.push_back(hitState(ch));
        raw.hit.state.back().time = start;
        const uint8_t* r = &pcm_[ch * 8];
        if (!(r[0x86] & 0x02))
            raw.naturalEnd = kNever; // loops until stopped
        else
        {
            const uint32_t from = static_cast<uint32_t>(r[0x85]) << 8 | r[0x84];
            uint32_t to = (static_cast<uint32_t>(r[0x06]) + 1) << 8;
            if (to <= from)
                to = 0x10000;
            const double seconds = (to - from) * 256.0 / std::max<uint8_t>(r[0x07], 1) / pcmRate_;
            raw.naturalEnd = start + static_cast<uint32_t>(seconds * kVgmRate);
        }
        raw.sample = recordSample(ch);
        openHit_[ch] = static_cast<int>(hits_.size());
        hits_.push_back(raw);
    }

    VgmHitState hitState(int ch) const
    {
        const uint8_t* r = &pcm_[ch * 8];
        return { time_, r[0x02] & 0x7F, r[0x03] & 0x7F, r[0x07] };
    }

    void closeHit(int ch, uint32_t end)
    {
        if (openHit_[ch] < 0)
            return;
        RawHit& raw = hits_[static_cast<size_t>(openHit_[ch])];
        raw.hit.end = end;
        raw.closed = true;
        openHit_[ch] = -1;
    }

    FmPatch channelPatch(int ch) const
    {
        FmPatch p;
        const uint8_t con = ym_[0x20 + ch];
        p.alg = con & 7;
        p.fb = (con >> 3) & 7;
        p.pan = (con & 0xC0) == 0x40 ? FmPan::Left : (con & 0xC0) == 0x80 ? FmPan::Right : FmPan::Both;
        p.pms = (ym_[0x38 + ch] >> 4) & 7;
        p.ams = ym_[0x38 + ch] & 3;
        p.lfoFreq = ym_[0x18];
        p.lfoWave = ym_[0x1B] & 3;
        p.amd = amd_;
        p.pmd = pmd_;

        for (int i = 0; i < 4; ++i)
        {
            const int s = ch + kSlotOffset[i];
            FmOperator& o = p.op[static_cast<size_t>(i)];
            const int dt1 = (ym_[0x40 + s] >> 4) & 7;
            o.dt1 = static_cast<int8_t>(dt1 < 4 ? dt1 : 4 - dt1);
            o.mul = ym_[0x40 + s] & 15;
            o.tl = ym_[0x60 + s] & 0x7F;
            o.ks = ym_[0x80 + s] >> 6;
            o.ar = ym_[0x80 + s] & 31;
            o.amEn = (ym_[0xA0 + s] & 0x80) != 0;
            o.d1r = ym_[0xA0 + s] & 31;
            o.dt2 = ym_[0xC0 + s] >> 6;
            o.d2r = ym_[0xC0 + s] & 31;
            o.d1l = ym_[0xE0 + s] >> 4;
            o.rr = ym_[0xE0 + s] & 15;
        }
        return p;
    }

    // Drivers scale carrier levels for note volume, so carrier TL is left
    // out of the comparison and the loudest setting is kept. Only a key on
    // counts as a use of the patch.
    int recordPatch(const FmPatch& p, bool keyOn)
    {
        const uint8_t carriers = kCarrierMask[p.alg];
        for (size_t index = 0; index < patches_.size(); ++index)
        {
            PatchEntry& e = patches_[index];
            if (!samePatch(e.patch, p, carriers))
                continue;
            lowerCarrierTl(e.patch, p);
            e.uses += keyOn ? 1 : 0;
            return static_cast<int>(index);
        }
        patches_.push_back({ p, keyOn ? 1 : 0 });
        return static_cast<int>(patches_.size() - 1);
    }

    static void lowerCarrierTl(FmPatch& kept, const FmPatch& seen)
    {
        const uint8_t carriers = kCarrierMask[kept.alg];
        for (size_t i = 0; i < 4; ++i)
            if ((carriers >> i) & 1)
                kept.op[i].tl = std::min(kept.op[i].tl, seen.op[i].tl);
    }

    static bool samePatch(const FmPatch& a, const FmPatch& b, uint8_t carriers)
    {
        if (a.alg != b.alg || a.fb != b.fb || a.pms != b.pms || a.ams != b.ams)
            return false;
        for (size_t i = 0; i < 4; ++i)
        {
            const FmOperator& x = a.op[i];
            const FmOperator& y = b.op[i];
            const bool carrier = (carriers >> i) & 1;
            if (std::tie(x.ar, x.d1r, x.d2r, x.rr, x.d1l, x.mul, x.dt1, x.dt2, x.ks, x.amEn)
                    != std::tie(y.ar, y.d1r, y.d2r, y.rr, y.d1l, y.mul, y.dt1, y.dt2, y.ks, y.amEn)
                || (!carrier && x.tl != y.tl))
                return false;
        }
        // A volume change moves every carrier by the same amount; carriers
        // that differ by different amounts belong to another instrument.
        int offset = 0;
        bool first = true;
        for (size_t i = 0; i < 4; ++i)
        {
            if (!((carriers >> i) & 1))
                continue;
            const int d = static_cast<int>(a.op[i].tl) - static_cast<int>(b.op[i].tl);
            if (!first && d != offset)
                return false;
            offset = d;
            first = false;
        }
        return true;
    }

    SampleKey recordSample(int ch)
    {
        const uint8_t* r = &pcm_[ch * 8];
        const uint8_t flags = r[0x86];
        const uint32_t romOffset = static_cast<uint32_t>(flags & mask_) << shift_;
        const uint32_t start = static_cast<uint32_t>(r[0x85]) << 8 | r[0x84];
        uint32_t end = (static_cast<uint32_t>(r[0x06]) + 1) << 8;
        if (end <= start)
            end = 0x10000;

        const SampleKey key { romOffset, start };
        SampleEntry& s = samples_[key];
        s.romOffset = romOffset;
        s.start = start;
        s.end = std::max(s.end, end);
        s.loop = !(flags & 0x02);
        s.loopStart = static_cast<uint32_t>(r[0x05]) << 8 | r[0x04];
        ++s.steps[r[0x07]];
        ++s.uses;
        return key;
    }

    VgmHeader header_;
    bool started_ = false;
    double pcmRate_ = SegaPcm::kSampleRate;
    uint32_t time_ = 0;
    double clockShift_ = 0;
    std::array<uint8_t, 256> ym_ {};
    uint8_t amd_ = 0, pmd_ = 0;
    std::array<uint8_t, 256> pcm_ {};
    std::vector<uint8_t> rom_;
    uint32_t shift_ = 12, mask_ = 0x70;
    std::vector<PatchEntry> patches_;
    std::map<SampleKey, SampleEntry> samples_;
    std::vector<RawNote> notes_;
    std::vector<RawHit> hits_;
    std::array<int, 8> openNote_ {};
    std::array<int, 8> lastNote_ {};
    std::array<bool, 8> dirty_ {};
    std::array<int, 16> openHit_ {};
    static constexpr uint32_t kNever = 0xFFFFFFFF;
    std::array<uint32_t, 16> restartAt_ {}; // last address write on a playing channel
};

} // namespace

std::string parseVgm(const std::vector<uint8_t>& file, VgmVisitor& visitor)
{
    VgmHeader header;
    std::vector<uint8_t> inflated;
    const std::vector<uint8_t>* data = &file;
    if (file.size() > 2 && file[0] == 0x1F && file[1] == 0x8B)
    {
        if (!gunzip(file, inflated))
            return "The .vgz file could not be decompressed.";
        data = &inflated;
    }
    const std::vector<uint8_t>& d = *data;

    if (d.size() < 0x40 || d[0] != 'V' || d[1] != 'g' || d[2] != 'm' || d[3] != ' ')
        return "This is not a VGM file.";

    const uint32_t version = u32(d, 0x08);
    const uint32_t dataOffset = u32(d, 0x34);
    size_t pos = version >= 0x150 && dataOffset != 0 ? 0x34 + dataOffset : 0x40;
    header.ym2151Clock = u32(d, 0x30) & 0x3FFFFFFF;
    header.segaPcmClock = pos > 0x38 ? u32(d, 0x38) : 0;
    header.segaPcmInterface = pos > 0x3C ? u32(d, 0x3C) : 0;
    if (header.ym2151Clock == 0 && header.segaPcmClock == 0)
        return "This VGM file uses neither a YM2151 nor a SegaPCM chip.";
    visitor.begin(header);

    while (pos < d.size())
    {
        const uint8_t op = d[pos];
        if (op == 0x66)
            break;

        if (op == 0x67)
        {
            if (pos + 7 > d.size())
                break;
            const uint8_t type = d[pos + 2];
            const uint32_t size = u32(d, pos + 3) & 0x7FFFFFFF;
            const size_t body = pos + 7;
            if (body + size > d.size())
                break;
            if (type == 0x80 && size > 8)
                visitor.pcmRom(&d[body + 8], size - 8, u32(d, body), u32(d, body + 4));
            pos = body + size;
            continue;
        }

        const size_t length = commandLength(op);
        if (length == 0 || pos + length > d.size())
        {
            char message[64];
            std::snprintf(message, sizeof message, "Unknown VGM command 0x%02X at offset 0x%zX.", op, pos);
            return message;
        }

        if (op == 0x61)
            visitor.wait(static_cast<uint32_t>(d[pos + 1] | d[pos + 2] << 8));
        else if (op == 0x62)
            visitor.wait(735);
        else if (op == 0x63)
            visitor.wait(882);
        else if (op >= 0x70 && op <= 0x7F)
            visitor.wait(static_cast<uint32_t>(op & 15) + 1);
        else if (op >= 0x80 && op <= 0x8F)
            visitor.wait(static_cast<uint32_t>(op & 15));
        else if (op == 0x54)
            visitor.ym2151(d[pos + 1], d[pos + 2]);
        else if (op == 0xC0)
            visitor.segaPcm(static_cast<uint16_t>(d[pos + 1] | d[pos + 2] << 8), d[pos + 3]);
        pos += length;
    }
    return {};
}

VgmImport importVgm(const std::vector<uint8_t>& file, const std::string& name)
{
    VgmImport out;
    Collector collector;
    out.error = parseVgm(file, collector);
    if (!collector.started())
        return out;

    out.ym2151Clock = collector.header().ym2151Clock;
    out.segaPcmClock = collector.header().segaPcmClock;
    const double pcmClock = out.segaPcmClock ? out.segaPcmClock : SegaPcm::kClock;
    collector.finish(out, name, pcmClock / 128.0);
    if (out.error.empty() && out.patches.empty() && out.samples.empty())
        out.error = "No notes were found in this VGM file.";
    return out;
}

} // namespace opm86
