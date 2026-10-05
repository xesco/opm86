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

// The chip-wide settings packed in 30 bits, for an atomic.
uint32_t pack(const ChipState& c)
{
    return static_cast<uint32_t>(c.lfoRate) | static_cast<uint32_t>(c.lfoWave & 3) << 8 | static_cast<uint32_t>(c.pmd & 0x7F) << 10
           | static_cast<uint32_t>(c.amd & 0x7F) << 17 | static_cast<uint32_t>(c.noise ? 1 : 0) << 24 | static_cast<uint32_t>(c.noiseFreq & 0x1F) << 25;
}

ChipState unpack(uint32_t v)
{
    return { static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8 & 3), static_cast<uint8_t>(v >> 10 & 0x7F),
             static_cast<uint8_t>(v >> 17 & 0x7F), (v >> 24 & 1) != 0, static_cast<uint8_t>(v >> 25 & 0x1F) };
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
    std::array<uint8_t, 256> regs {};
    std::array<uint8_t, 8> keys {};
    if (!snapshots.fm.empty())
    {
        const auto& last = snapshots.fm.back();
        state = last.state;
        regs = last.regs;
        keys = last.keys;
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
            snapshots.fm.push_back({ k, write, state, regs, keys });
        }
        while (write < song.ym.size() && song.ym[write].time <= now)
        {
            fm.write(0, static_cast<uint8_t>(song.ym[write].address));
            fm.write(1, song.ym[write].value);
            shadowWrite(regs, keys, song.ym[write]);
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
    out->fm.push_back({ 0, 0, state, {}, {} });
    SegaPcm pcm;
    pcm.reset();
    pcm.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
    out->pcm.push_back({ 0, 0, pcm });
    return out;
}

ChipState chipAt(const VgmSong& song, const SongSnapshots& snapshots, double songTime)
{
    std::array<uint8_t, 256> regs {};
    std::array<uint8_t, 8> keys {};
    size_t write = 0;
    const uint64_t target = chipSample(songTime, song.fmRate);
    auto it = std::upper_bound(snapshots.fm.begin(), snapshots.fm.end(), target,
                               [](uint64_t s, const SongSnapshots::Fm& f) { return s < f.sample; });
    if (it != snapshots.fm.begin())
        regs = (it - 1)->regs, write = (it - 1)->write;
    // The writes after it, up to the chip sample the time falls on, as playback applies them.
    const double until = static_cast<double>(target) * kVgmRate / song.fmRate;
    for (; write < song.ym.size() && song.ym[write].time <= until; ++write)
        shadowWrite(regs, keys, song.ym[write]);
    return chipOf(regs);
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

void shadowWrite(std::array<uint8_t, 256>& regs, std::array<uint8_t, 8>& keys, const VgmWrite& w)
{
    const int reg = w.address & 0xFF;
    if (reg == 0x19 && (w.value & 0x80))
        regs[0x1A] = w.value;
    else if (reg != 0x1A)
        regs[static_cast<size_t>(reg)] = w.value;
    if (reg == 0x08)
        keys[w.value & 7] = w.value & 0x78;
}

namespace
{
// The YM2151's operator slots, in patch order M1, C1, M2, C2: M2 is the second slot block, C1 the third.
constexpr int kSlotOf[4] = { 0, 16, 8, 24 };
constexpr int kOperatorOfBlock[4] = { 0, 2, 1, 3 }; // a slot block's operator in patch order
} // namespace

uint8_t editedWrite(const PatchEdits* edits, const std::array<int, 8>& owner, uint16_t address, uint8_t value)
{
    if (!edits)
        return value;
    const int reg = address & 0xFF;
    if (address < 0x20 && address != 0x08)
    {
        // The chip-wide registers: a field the edits set takes their value;
        // any other keeps what the song writes. Register 0x19 holds the
        // vibrato depth with bit 7 set, the tremolo depth without; 0x1B keeps
        // its two output pins.
        const ChipEdits& c = edits->chip;
        switch (reg)
        {
            case 0x0F:
                return static_cast<uint8_t>((value & 0x60) | (c.noise ? (*c.noise ? 0x80 : 0) : (value & 0x80))
                                            | (c.noiseFreq ? (*c.noiseFreq & 0x1F) : (value & 0x1F)));
            case 0x18: return c.lfoRate.value_or(value);
            case 0x19:
                if (value & 0x80)
                    return c.pmd ? static_cast<uint8_t>(0x80 | (*c.pmd & 0x7F)) : value;
                return c.amd ? static_cast<uint8_t>(*c.amd & 0x7F) : value;
            case 0x1B: return static_cast<uint8_t>((value & 0xFC) | (c.lfoWave ? (*c.lfoWave & 3) : (value & 3)));
            default: return value;
        }
    }
    // A channel register's channel, an operator slot's, or the key on's.
    const int ch = reg == 0x08 ? value & 7 : reg & 7;
    const int entry = owner[static_cast<size_t>(ch)];
    const bool notePan = entry >= 0 && (entry & PatchEdits::kNotePan) != 0;
    const int track = entry >= 0 ? entry & ~PatchEdits::kNotePan : entry;
    if (reg == 0x08 && track >= 0 && track == edits->mutedTrack)
    {
        // Operators muted to audition are never keyed on, edited or not.
        const auto heard = static_cast<uint8_t>(~(edits->muted << 3));
        const bool edited = static_cast<size_t>(track) < edits->tracks.size() && edits->tracks[static_cast<size_t>(track)];
        const uint8_t on = edited ? keyOnMask(edits->tracks[static_cast<size_t>(track)]->edited) : 0x78;
        return static_cast<uint8_t>(value & ((on & heard) | 0x87));
    }
    if (track < 0 || static_cast<size_t>(track) >= edits->tracks.size() || !edits->tracks[static_cast<size_t>(track)])
        return value;
    const FmPatch& e = edits->tracks[static_cast<size_t>(track)]->edited;
    const FmPatch& o = edits->tracks[static_cast<size_t>(track)]->original;
    if (reg == 0x08)
    {
        // Operators switched off are never keyed on.
        return static_cast<uint8_t>(value & (keyOnMask(e) | 0x87));
    }
    // Field by field: a field the edit changed takes its value; any other
    // keeps what the song writes, mid-note changes and all. A level moves by
    // the edit's change.
    auto field = [value](int edited, int original, int shift, int bits) {
        const int mask = ((1 << bits) - 1) << shift;
        return edited == original ? (value & mask) : ((edited << shift) & mask);
    };
    if (reg >= 0x20 && reg < 0x28)
    {
        // Output sides as the song has them, unless the edit sets the
        // channel's and the note has no pan of its own.
        const uint8_t set = edits->tracks[static_cast<size_t>(track)]->sides[static_cast<size_t>(ch)];
        const int sides = set != 0 && !notePan ? set : (value & 0xC0);
        return static_cast<uint8_t>(sides | field(e.fb, o.fb, 3, 3) | field(e.alg, o.alg, 0, 3));
    }
    if (reg >= 0x38 && reg < 0x40)
        return static_cast<uint8_t>((value & 0x8C) | field(e.pms, o.pms, 4, 3) | field(e.ams, o.ams, 0, 2));
    if (reg < 0x40)
        return value;
    const auto& op = e.op[static_cast<size_t>(kOperatorOfBlock[(reg & 0x1F) >> 3])];
    const auto& was = o.op[static_cast<size_t>(kOperatorOfBlock[(reg & 0x1F) >> 3])];
    auto dt1 = [](int d) { return d >= 0 ? d : 4 - d; }; // the register's sign and size
    switch (reg & 0xE0)
    {
        case 0x40: return static_cast<uint8_t>((value & 0x80) | field(dt1(op.dt1), dt1(was.dt1), 4, 3) | field(op.mul, was.mul, 0, 4));
        case 0x60: return static_cast<uint8_t>((value & 0x80) | std::clamp((value & 0x7F) + op.tl - was.tl, 0, 127));
        case 0x80: return static_cast<uint8_t>((value & 0x20) | field(op.ks, was.ks, 6, 2) | field(op.ar, was.ar, 0, 5));
        case 0xA0: return static_cast<uint8_t>((value & 0x60) | field(op.amEn, was.amEn, 7, 1) | field(op.d1r, was.d1r, 0, 5));
        case 0xC0: return static_cast<uint8_t>((value & 0x20) | field(op.dt2, was.dt2, 6, 2) | field(op.d2r, was.d2r, 0, 5));
        default: return static_cast<uint8_t>(field(op.d1l, was.d1l, 4, 4) | field(op.rr, was.rr, 0, 4));
    }
}

std::array<RegWrite, 27> channelRewrite(const PatchEdits* edits, const std::array<int, 8>& owner,
                                        const std::array<uint8_t, 256>& regs, const std::array<uint8_t, 8>& keys, int ch)
{
    std::array<RegWrite, 27> out {};
    size_t n = 0;
    auto put = [&](int reg, uint8_t value) {
        out[n++] = { static_cast<uint8_t>(reg), editedWrite(edits, owner, static_cast<uint16_t>(reg), value) };
    };
    put(0x20 + ch, regs[static_cast<size_t>(0x20 + ch)]);
    put(0x38 + ch, regs[static_cast<size_t>(0x38 + ch)]);
    // The key on as the song has it now, less the operators switched off: an
    // operator keyed already carries on, one switched off releases.
    put(0x08, static_cast<uint8_t>(keys[static_cast<size_t>(ch)] | ch));
    for (const int base : { 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0 })
        for (const int slot : kSlotOf)
            put(base + slot + ch, regs[static_cast<size_t>(base + slot + ch)]);
    return out;
}

std::array<RegWrite, 5> chipRewrite(const PatchEdits* edits, const std::array<uint8_t, 256>& regs)
{
    const std::array<int, 8> none { -1, -1, -1, -1, -1, -1, -1, -1 };
    auto put = [&](int reg, uint8_t value) {
        return RegWrite { static_cast<uint8_t>(reg), editedWrite(edits, none, static_cast<uint16_t>(reg), value) };
    };
    return { put(0x0F, regs[0x0F]), put(0x18, regs[0x18]), put(0x19, static_cast<uint8_t>(regs[0x19] & 0x7F)),
             put(0x19, static_cast<uint8_t>(regs[0x1A] | 0x80)), put(0x1B, regs[0x1B]) };
}

bool advanceOwner(const PatchEdits& edits, size_t ch, double now, size_t& at, int& owner)
{
    const auto& v = edits.owner[ch];
    bool moved = false;
    for (; at < v.size() && v[at].first <= now; ++at)
    {
        moved = moved || owner != v[at].second;
        owner = v[at].second;
    }
    return moved;
}

void SongPlayer::rewrite(Deck& deck, int ch)
{
    for (const RegWrite& w : channelRewrite(deck.edits.get(), deck.editOwner, deck.regs, deck.keys, ch))
    {
        deck.fm.write(0, w.reg);
        deck.fm.write(1, w.value);
    }
}

void SongPlayer::rewriteChip(Deck& deck)
{
    for (const RegWrite& w : chipRewrite(deck.edits.get(), deck.regs))
    {
        deck.fm.write(0, w.reg);
        deck.fm.write(1, w.value);
    }
}

void SongPlayer::seekEdits(Deck& d)
{
    const bool had = d.edits != nullptr;
    const bool hadChip = had && d.edits->chip.any();
    d.edits = edits_;
    const double now = fmNow(d);
    for (size_t ch = 0; ch < 8; ++ch)
    {
        d.editOwner[ch] = -1;
        d.editAt[ch] = 0;
        if (d.edits)
        {
            const auto& v = d.edits->owner[ch];
            d.editAt[ch] = static_cast<size_t>(std::upper_bound(v.begin(), v.end(), now, [](double t, const std::pair<double, int>& x) {
                                                   return t < x.first;
                                               }) - v.begin());
            if (d.editAt[ch] > 0)
                d.editOwner[ch] = v[d.editAt[ch] - 1].second;
        }
        // Every channel as the edits have it now; with none left, as the song wrote it.
        if (d.edits || had)
            rewrite(d, static_cast<int>(ch));
    }
    // And the LFO and noise: a field set takes its value at once, and a field
    // no longer set goes back to the song's, so a song that never writes them
    // has them too.
    if (hadChip || (d.edits && d.edits->chip.any()))
        rewriteChip(d);
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
    deck.regs = fmSnap.regs;
    deck.keys = fmSnap.keys;
    deck.pcm = pcmSnap.chip;
    deck.pcm.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
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
            shadowWrite(deck.regs, deck.keys, song.ym[deck.ymWrite]);
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
    deck.stretched = speed_ != 1.0;
    deck.fmAnchor = deck.fmSample, deck.fmAnchorTime = static_cast<double>(deck.fmSample) * kVgmRate / song.fmRate;
    deck.pcmAnchor = deck.pcmSample, deck.pcmAnchorTime = static_cast<double>(deck.pcmSample) * kVgmRate / song.pcmRate;
    deck.lastGain.fill(1.0f);
    seekMix(deck, false);
    deck.edits = nullptr; // rewritten at the first render, as the snapshot holds the song as it is
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
    if (deck.edits != edits_)
        seekEdits(deck);
    if (deck.probeOwner != probeOwner_)
    {
        // The probe's cursors to where the deck is.
        deck.probeOwner = probeOwner_;
        const double now = fmNow(deck);
        for (size_t ch = 0; deck.probeOwner && ch < 8; ++ch)
        {
            const auto& v = deck.probeOwner->changes[ch];
            deck.probeAt[ch] = static_cast<size_t>(std::upper_bound(v.begin(), v.end(), now, [](double t, const ChannelMix::Change& x) {
                                                       return t < x.time;
                                                   }) - v.begin());
        }
    }
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
            // With edits, a channel taken over by another instrument gets that
            // instrument's registers, before the song's writes for its note.
            if (deck.edits)
                for (size_t ch = 0; ch < 8; ++ch)
                    if (advanceOwner(*deck.edits, ch, now, deck.editAt[ch], deck.editOwner[ch]))
                        rewrite(deck, static_cast<int>(ch));
            for (; deck.ymWrite < song.ym.size() && song.ym[deck.ymWrite].time <= now; ++deck.ymWrite)
            {
                const VgmWrite& w = song.ym[deck.ymWrite];
                shadowWrite(deck.regs, deck.keys, w);
                deck.fm.write(0, static_cast<uint8_t>(w.address));
                deck.fm.write(1, editedWrite(deck.edits.get(), deck.editOwner, w.address, w.value));
            }
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
            // The probed channels, summed, for a display, those the owner has
            // now, heard or not; every 64 samples, the envelopes of the one
            // whose operators are loudest.
            auto channels = static_cast<uint32_t>(probe_.load(std::memory_order_relaxed)) & 0xFF;
            if (measure && channels != 0 && deck.probeOwner)
                for (uint32_t ch = 0; ch < 8; ++ch)
                {
                    const auto& v = deck.probeOwner->changes[ch];
                    size_t& i = deck.probeAt[ch];
                    while (i < v.size() && v[i].time <= now)
                        ++i;
                    if (i == 0 || v[i - 1].gain <= 0.0f)
                        channels &= ~(1u << ch);
                }
            if (measure && (probe_.load(std::memory_order_relaxed) & 0xFF) != 0)
            {
                const uint32_t at = probeAt_.load(std::memory_order_relaxed);
                probeOutput_[at % kProbeSize].store(static_cast<float>(deck.fm.channelsOutput(channels)) * kChipScale, std::memory_order_relaxed);
                probeAt_.store(at + 1, std::memory_order_release);
                if (at % 64 == 0)
                {
                    float best[4] {}, levels[4];
                    int bestStages[4] {}, stages[4];
                    float loudest = -1.0f;
                    for (uint32_t ch = 0; ch < 8; ++ch)
                        if ((channels >> ch) & 1)
                        {
                            deck.fm.envelopes(ch, levels, stages);
                            const float sum = levels[0] + levels[1] + levels[2] + levels[3];
                            if (sum > loudest)
                                loudest = sum, std::copy(levels, levels + 4, best), std::copy(stages, stages + 4, bestStages);
                        }
                    for (size_t op = 0; op < 4; ++op)
                        probeLevel_[op].store(best[op], std::memory_order_relaxed), probeStage_[op].store(bestStages[op], std::memory_order_relaxed);
                }
            }
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
        deck.songTime += n * speed_ * kVgmRate / hostRate_;
    }
    if (deck.gain == 0.0f && deck.target == 0.0f)
        deck.on = false;
    if (measure)
    {
        for (size_t c = 0; c < peaks.size(); ++c)
        {
            // FM channels reach 32767; a PCM channel 127 x 127.
            const float level = static_cast<float>(peaks[c]) * (c < 8 ? kChipScale : 1.0f / 16129.0f);
            if (level > peaks_[c].load(std::memory_order_relaxed))
                peaks_[c].store(level, std::memory_order_relaxed);
        }
        // The song's LFO and noise where it is, for the chip panel.
        gameChip_.store(pack(chipOf(deck.regs)), std::memory_order_relaxed);
    }
}

ChipState SongPlayer::gameChip() const
{
    return unpack(gameChip_.load(std::memory_order_relaxed));
}

void SongPlayer::probeOutput(float* out, int count) const
{
    const uint32_t end = probeAt_.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        out[i] = probeOutput_[(end - static_cast<uint32_t>(count) + static_cast<uint32_t>(i)) % kProbeSize].load(std::memory_order_relaxed);
}

void SongPlayer::probeEnvelopes(float* levels, int* stages) const
{
    for (size_t op = 0; op < 4; ++op)
    {
        levels[op] = probeLevel_[op].load(std::memory_order_relaxed);
        stages[op] = probeStage_[op].load(std::memory_order_relaxed);
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
    // With a loop, the block is cut where the deck heard reaches its end,
    // and playback goes on from its start with the next sample. A loop no
    // longer than the crossfade plays through, so a jump never seeks the
    // deck still fading out.
    const double step = speed_ * kVgmRate / hostRate_;
    const bool looping = loopEnd_ - loopStart_ > step / static_cast<double>(fade_);
    for (int done = 0; done < count;)
    {
        int n = count - done;
        bool wrap = false;
        const Deck& heard = decks_[static_cast<size_t>(active_)];
        if (looping && heard.on && heard.target > 0.0f && heard.songTime < loopEnd_)
        {
            const double until = std::ceil((loopEnd_ - heard.songTime) / step);
            if (until <= static_cast<double>(n))
                n = std::max(1, static_cast<int>(until)), wrap = true;
        }
        for (Deck& deck : decks_)
            if (deck.on)
                renderDeck(deck, left + done, right + done, n);
        done += n;
        if (wrap)
            play(loopStart_);
    }
}

} // namespace opm86
