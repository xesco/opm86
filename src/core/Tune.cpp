#include "Tune.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace opm86
{

namespace
{

constexpr int kFmChannels = 8;
constexpr int kPcmChannels = SegaPcm::kChannels;

constexpr int kStartAddress = 6; // index of 0x84 in pcmRegister's list

// Which of a channel's operators (bit k for slot ch + 8k: M1, M2, C1, C2)
// are carriers, heard directly, for each connection.
constexpr uint8_t kCarriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xC, 0xE, 0xE, 0xF };

uint8_t attenuate(uint8_t totalLevel, int steps)
{
    return static_cast<uint8_t>(std::clamp((totalLevel & 0x7F) + steps, 0, 127));
}

uint8_t scaleVolume(uint8_t volume, float gain)
{
    return static_cast<uint8_t>(std::min(127L, std::lround(static_cast<float>(volume & 0x7F) * gain)));
}

bool isKeyOn(uint16_t address, uint8_t value) { return address == 0x08 && (value & 0x78) != 0; }
bool isKeyOff(uint16_t address, uint8_t value) { return address == 0x08 && (value & 0x78) == 0; }
bool isTrigger(uint16_t address, uint8_t value) { return (address & 0x87) == 0x86 && !(value & 1); }

// Key codes number notes from C#, skipping every fourth code.
double keyCodePitch(uint8_t kc, uint8_t kf)
{
    const int code = kc & 15;
    return (kc >> 4 & 7) * 12 + code - code / 4 + (kf >> 2) / 64.0;
}

constexpr uint8_t kCode[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 }; // the key codes of the notes from C#

uint8_t transposeKeyCode(uint8_t kc, int semitones)
{
    if (semitones == 0)
        return kc; // the game's own value, even an unused code
    const int code = kc & 15;
    const int n = std::clamp((kc >> 4 & 7) * 12 + code - code / 4 + semitones, 0, 95);
    return static_cast<uint8_t>((n / 12) << 4 | kCode[n % 12]);
}

// Whether an edit sets a ripped hit's loop address, registers 0x04 and 0x05:
// a loop point of its own, a start or an end the loop must stay between, or
// a loop switched on, where a one-shot's leftover address would not do.
bool setsLoop(const SampleEdit& e)
{
    return e.loopStart || e.start || e.endPage || e.loop.value_or(false);
}

// A write on its way into the built song. At one time, writes keep their
// place in the original log; corrections go just before the key on or
// trigger they prepare.
struct Out
{
    int64_t time;
    uint64_t key;
    uint16_t address;
    uint8_t value;
    int request = -1; // the key on or trigger of this request in the list below
};

struct Request
{
    int channel;
    std::array<uint8_t, kFmRegs> regs; // FM uses 28; drums the first 8
    bool forceAddress = false;         // drums: write the start address whatever the copy says
};

// What a song's SegaPCM reads of its ROM as it plays.
struct RomReads
{
    RomRanges ranges;      // whole pages, at their offsets as the chip masks them
    size_t span = 1;       // the ROM's size rounded up to a power of two, as the chip masks with it
    bool mirrored = false; // a read lands past the span, so a larger ROM would read other bytes there
};

// Follows each SegaPCM channel through a song's writes as the chip plays
// it: from a trigger or a new address on, at its step, to its end page and
// round its loop, or stopped there by a one-shot, until the next write
// that changes them, in the bank its flags select. Every page it passes is
// read. One chip sample more than the timing gives counts as read, so a
// page reached by a byte is never left out.
RomReads romReads(const VgmSong& song)
{
    RomReads out;
    while (out.span < song.rom.size() && out.span < 0x80000000u)
        out.span <<= 1;
    std::vector<bool> read((out.span + 0xFF) >> 8, false);
    const SegaPcm::Banking banking = SegaPcm::banking(song.header.segaPcmInterface);

    // The pages of a bank from one address to another, as the chip counts
    // addresses (the byte in the bank x 256), wrapping in the bank.
    auto mark = [&](uint32_t bank, double from, double to) {
        const auto first = static_cast<int64_t>(from) >> 16, last = std::min(static_cast<int64_t>(to) >> 16, first + 255);
        for (int64_t p = first; p <= last; ++p)
        {
            const size_t offset = static_cast<size_t>(bank) + (static_cast<size_t>(p & 0xFF) << 8);
            out.mirrored = out.mirrored || offset + 0xFF >= out.span;
            read[(offset & (out.span - 1)) >> 8] = true;
            read[((offset + 0xFF) & (out.span - 1)) >> 8] = true;
        }
    };

    constexpr double kAddresses = 0x1000000; // a channel's address wraps at 24 bits
    std::array<uint8_t, 256> ram {};
    for (size_t ch = 0; ch < SegaPcm::kChannels; ++ch)
        ram[ch * 8 + 0x86] = 0x01;
    std::array<double, SegaPcm::kChannels> address {}; // per channel, where it plays
    std::array<double, SegaPcm::kChannels> until {};   // and the song time it has played to

    // A channel played on to a song time, its address written back as the chip writes it.
    auto advance = [&](size_t ch, double time) {
        uint8_t* regs = &ram[ch * 8];
        const double samples = (time - until[ch]) * song.pcmRate / kVgmRate;
        until[ch] = time;
        if ((regs[0x86] & 1) || samples <= 0.0)
            return;
        const uint32_t bank = banking.offset(regs[0x86]);
        const auto endPage = static_cast<uint8_t>(regs[6] + 1);
        const double end = static_cast<double>(endPage) * 0x10000, step = regs[7];
        const double loop = static_cast<double>(regs[5] << 16 | regs[4] << 8);
        // How far an address is from the end page; none on it, as the chip loops or stops there at once.
        auto distance = [&](double from) { return (static_cast<int64_t>(from) >> 16) == endPage ? 0.0 : std::fmod(end - from + kAddresses, kAddresses); };
        double& a = address[ch];
        double left = samples * step;
        const double toEnd = distance(a);
        if (toEnd > 0.0)
            mark(bank, a, a + std::min(left + step, toEnd - 1.0));
        if (left < toEnd)
            a += left;
        else if (regs[0x86] & 2)
            regs[0x86] |= 1;
        else
        {
            left -= toEnd;
            const double lap = distance(loop);
            if (lap <= 0.0)
                mark(bank, loop, loop + step), a = loop;
            else if (left >= lap)
                mark(bank, loop, loop + lap - 1.0), a = loop + std::fmod(left, lap);
            else
                mark(bank, loop, loop + std::min(left + step, lap - 1.0)), a = loop + left;
        }
        a = std::fmod(a, kAddresses);
        regs[0x84] = static_cast<uint8_t>(static_cast<int64_t>(a) >> 8);
        regs[0x85] = static_cast<uint8_t>(static_cast<int64_t>(a) >> 16);
    };

    // The writes as the player applies them, its two stop markers too (see writePcm).
    for (const VgmWrite& w : song.pcm)
    {
        const auto offset = static_cast<size_t>(w.address & 0xFF);
        const size_t ch = (offset >> 3) & 15;
        advance(ch, w.time);
        if (w.address & 0x2000)
            ram[offset] |= 0x01;
        else if (w.address & 0x1000)
        {
            if (!(ram[offset] & 0x02))
                ram[offset] |= 0x01;
        }
        else
        {
            ram[offset] = w.value;
            const size_t reg = offset & 0x87;
            if (reg == 0x84 || reg == 0x85)
                address[ch] = static_cast<double>(ram[ch * 8 + 0x85] << 16 | ram[ch * 8 + 0x84] << 8);
        }
    }
    // Whatever still plays at the end plays on to its end page, or round its loop.
    for (size_t ch = 0; ch < SegaPcm::kChannels; ++ch)
        advance(ch, 1e12);

    for (size_t p = 0; p < read.size();)
    {
        size_t q = p;
        while (q < read.size() && read[q])
            ++q;
        if (q > p)
            out.ranges.emplace_back(p << 8, q << 8);
        p = q + 1;
    }
    return out;
}

} // namespace

uint16_t pcmRegister(int channel, int index)
{
    static constexpr uint16_t kRegs[kPcmRegs] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x84, 0x85 };
    return static_cast<uint16_t>(kRegs[index] + channel * 8);
}

uint8_t transposeStep(uint8_t step, int semitones, int fine)
{
    if (semitones == 0 && fine == 0)
        return step;
    const double moved = step * std::pow(2.0, (semitones + fine / 64.0) / 12.0);
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(moved)), 1, 255));
}

std::pair<uint8_t, uint8_t> shiftKey(uint8_t kc, uint8_t kf, int semitones, int fine)
{
    if (fine == 0)
        return { transposeKeyCode(kc, semitones), kf };
    // In 64ths of a semitone from the lowest key code; the fraction's two low
    // bits are not part of the pitch and stay as they were.
    const int code = kc & 15;
    const int steps = std::clamp(((kc >> 4 & 7) * 12 + code - code / 4 + semitones) * 64 + (kf >> 2) + fine, 0, 95 * 64 + 63);
    const int n = steps / 64;
    return { static_cast<uint8_t>((n / 12) << 4 | kCode[n % 12]), static_cast<uint8_t>((steps % 64) << 2 | (kf & 3)) };
}

uint8_t carrierSlots(int connection)
{
    return kCarriers[connection & 7];
}

std::array<uint8_t, 2> noteVolumes(int left, int right, const TuneNote& note)
{
    double l = left & 0x7F, r = right & 0x7F;
    if (note.pan)
    {
        const double balance = std::clamp(*note.pan, -TuneNote::kPanRange, TuneNote::kPanRange) / static_cast<double>(TuneNote::kPanRange);
        const double sum = l + r;
        l = sum * (1.0 - balance) / 2.0;
        r = sum * (1.0 + balance) / 2.0;
    }
    if (note.velocity != 0)
    {
        const double gain = std::pow(10.0, note.velocityDb() / 20.0);
        l *= gain;
        r *= gain;
    }
    return { static_cast<uint8_t>(std::clamp(std::lround(l), 0L, 127L)), static_cast<uint8_t>(std::clamp(std::lround(r), 0L, 127L)) };
}

uint8_t scaleTo(uint8_t value, uint8_t from, uint8_t to, int high)
{
    if (from == to)
        return value;
    if (from == 0)
        return to;
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(static_cast<double>(value) * to / from)), 0, high));
}

uint16_t sampleFirst(const PadSample& sample)
{
    return sample.address;
}

uint8_t sampleLastPage(const PadSample& sample)
{
    const uint32_t end = std::max<uint32_t>(sample.address + 1, static_cast<uint32_t>(sample.address + sample.data.size()));
    return static_cast<uint8_t>(std::min<uint32_t>((end - 1) >> 8, 0xFF));
}

SampleEdit SampleEdit::fitted(const PadSample& sample) const
{
    SampleEdit e = *this;
    const int first = sampleFirst(sample), lastPage = sampleLastPage(sample);
    if (e.endPage)
        e.endPage = static_cast<uint8_t>(std::clamp<int>(*e.endPage, first >> 8, lastPage));
    const int end = ((e.endPage ? *e.endPage : lastPage) + 1) << 8; // the first address past the sample
    if (e.start)
        e.start = static_cast<uint16_t>(std::clamp<int>(*e.start, first, end - 1));
    const int from = e.start ? *e.start : first;
    if (e.loopStart)
        e.loopStart = static_cast<uint16_t>(std::clamp<int>(*e.loopStart, from, end - 1));
    if (e.left)
        e.left = static_cast<uint8_t>(std::min<int>(*e.left, 127));
    if (e.right)
        e.right = static_cast<uint8_t>(std::min<int>(*e.right, 127));
    if (e.step)
        e.step = static_cast<uint8_t>(std::max<int>(*e.step, 1));
    return e;
}

uint16_t fmRegister(int channel, int index)
{
    static constexpr uint16_t kChannelRegs[4] = { 0x20, 0x28, 0x30, 0x38 };
    static constexpr uint16_t kOperatorRegs[6] = { 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0 };
    if (index < 4)
        return static_cast<uint16_t>(kChannelRegs[index] + channel);
    const int op = (index - 4) / 6, reg = (index - 4) % 6;
    return static_cast<uint16_t>(kOperatorRegs[reg] + channel + 8 * op);
}

std::array<uint8_t, kFmRegs> writtenRegs(const FmPatch& p, double key, double clockShift)
{
    static constexpr int kOperatorOfBlock[4] = { 0, 2, 1, 3 }; // slot blocks M1, M2, C1, C2 in patch order
    const double note = std::clamp(key - 13.0 - clockShift, 0.0, 95.0 + 63.0 / 64.0);
    const int n = static_cast<int>(note);
    std::array<uint8_t, kFmRegs> r {};
    r[0] = static_cast<uint8_t>(0xC0 | (p.fb & 7) << 3 | (p.alg & 7));
    r[1] = static_cast<uint8_t>((n / 12) << 4 | kCode[n % 12]);
    r[2] = static_cast<uint8_t>(std::min(63, static_cast<int>((note - n) * 64.0)) << 2);
    r[3] = static_cast<uint8_t>((p.pms & 7) << 4 | (p.ams & 3));
    for (size_t b = 0; b < 4; ++b)
    {
        const FmOperator& o = p.op[static_cast<size_t>(kOperatorOfBlock[b])];
        const int dt1 = o.dt1 >= 0 ? o.dt1 : 4 - o.dt1;
        uint8_t* v = &r[4 + 6 * b];
        v[0] = static_cast<uint8_t>(dt1 << 4 | (o.mul & 15));
        v[1] = static_cast<uint8_t>(o.tl & 0x7F);
        v[2] = static_cast<uint8_t>((o.ks & 3) << 6 | (o.ar & 31));
        v[3] = static_cast<uint8_t>((o.amEn ? 0x80 : 0) | (o.d1r & 31));
        v[4] = static_cast<uint8_t>((o.dt2 & 3) << 6 | (o.d2r & 31));
        v[5] = static_cast<uint8_t>((o.d1l & 15) << 4 | (o.rr & 15));
    }
    return r;
}

std::unique_ptr<Tune> Tune::load(const std::vector<uint8_t>& file, const std::string& name, std::string& error)
{
    auto source = loadVgmSong(file, name, error);
    if (!source)
        return nullptr;
    auto tune = std::unique_ptr<Tune>(new Tune());
    auto data = std::make_shared<Data>();
    data->source = source;
    data->log = splitLog(*source);
    data->import = importVgm(file, name);
    data->clockShift = source->header.ym2151Clock ? 12.0 * std::log2(source->header.ym2151Clock / 3579545.0) : 0.0;
    const SongLog& log = data->log;

    // The register state each note met at its key on, from the log in order.
    std::map<uint32_t, size_t> keyOnOf, triggerOf;
    for (size_t n = 0; n < log.notes.size(); ++n)
        for (const LogWrite& w : log.notes[n].writes)
            if (w.offset == 0 && isKeyOn(w.address, w.value))
                keyOnOf[w.order] = n;
    for (size_t h = 0; h < log.hits.size(); ++h)
        for (const LogWrite& w : log.hits[h].writes)
            if (w.offset == 0 && isTrigger(w.address, w.value))
                triggerOf[w.order] = h;

    data->fmState.resize(log.notes.size());
    std::array<uint8_t, 256> ym {};
    for (size_t i = 0; i < source->ymLogged; ++i)
    {
        const VgmWrite& w = source->ym[i];
        if (auto it = keyOnOf.find(static_cast<uint32_t>(i)); it != keyOnOf.end())
            for (int r = 0; r < kFmRegs; ++r)
                data->fmState[it->second].regs[static_cast<size_t>(r)] = ym[fmRegister(log.notes[it->second].channel, r)];
        if (w.address != 0x08 && w.address < 256)
            ym[w.address] = w.value;
    }
    data->pcmState.resize(log.hits.size());
    std::array<uint8_t, 256> pcm {};
    for (size_t i = 0; i < source->pcmLogged; ++i)
    {
        const VgmWrite& w = source->pcm[i];
        if (auto it = triggerOf.find(static_cast<uint32_t>(i)); it != triggerOf.end())
            for (int r = 0; r < kPcmRegs; ++r)
                data->pcmState[it->second].regs[static_cast<size_t>(r)] = pcm[pcmRegister(log.hits[it->second].channel, r)];
        pcm[w.address & 0xFF] = w.value;
    }

    // Tracks: one per FM patch and one per drum sound, most used first. The
    // importer met the notes and hits in the same order as the log.
    std::map<int, size_t> fmTrack, drumTrack;
    for (size_t p = 0; p < data->import.patches.size(); ++p)
    {
        TuneTrack t;
        t.kind = TuneTrack::Kind::Fm;
        t.instrument = static_cast<int>(p);
        t.name = "FM " + std::to_string(p + 1);
        fmTrack[static_cast<int>(p)] = tune->tracks.size();
        tune->tracks.push_back(t);
    }
    for (size_t s = 0; s < data->import.samples.size(); ++s)
    {
        TuneTrack t;
        t.kind = TuneTrack::Kind::Drum;
        t.instrument = static_cast<int>(s);
        t.name = "Drum " + std::to_string(s + 1);
        drumTrack[static_cast<int>(s)] = tune->tracks.size();
        tune->tracks.push_back(t);
    }
    const size_t notes = std::min(log.notes.size(), data->import.notes.size());
    for (size_t n = 0; n < notes; ++n)
    {
        const LogNote& ln = log.notes[n];
        tune->tracks[fmTrack[data->import.notes[n].patch]].notes.push_back(
            { static_cast<int>(n), static_cast<int>(n), ln.start, std::max<int64_t>(1, ln.end - ln.start), 0 });
    }
    const size_t hits = std::min(log.hits.size(), data->import.hits.size());
    data->hitLength.resize(log.hits.size(), 1);
    for (size_t h = 0; h < hits; ++h)
    {
        const LogHit& lh = log.hits[h];
        const VgmHit& vh = data->import.hits[h];
        const int64_t length = std::max<int64_t>(1, static_cast<int64_t>(vh.end) - lh.start);
        data->hitLength[h] = length;
        tune->tracks[drumTrack[vh.sample]].notes.push_back(
            { static_cast<int>(log.notes.size() + h), static_cast<int>(h), lh.start, length, 0 });
    }
    tune->tracks.erase(std::remove_if(tune->tracks.begin(), tune->tracks.end(),
                                      [](const TuneTrack& t) { return t.notes.empty(); }),
                       tune->tracks.end());
    tune->grid.beatLength = source->beatLength;
    tune->grid.firstBeat = source->firstBeat;
    tune->data_ = data;
    return tune;
}

std::unique_ptr<Tune> Tune::empty(double tempo)
{
    auto source = std::make_shared<VgmSong>();
    source->name = "Untitled";
    source->header.ym2151Clock = 3579545;
    source->fmRate = source->header.ym2151Clock / 64.0;
    source->beatLength = 60.0 * kVgmRate / tempo;
    source->length = static_cast<uint32_t>(source->beatLength * 4 * 8); // eight bars of 4/4 to write in
    source->beatCount = static_cast<int>(std::ceil((source->length + 2.0 * kVgmRate) / source->beatLength));
    appendSongEnd(*source);
    auto data = std::make_shared<Data>();
    data->source = source;
    auto tune = std::unique_ptr<Tune>(new Tune());
    tune->grid.beatLength = source->beatLength;
    tune->data_ = data;
    return tune;
}

const FmPatch& Tune::defaultPatch()
{
    return factoryPatches()[2]; // Beach Piano
}

const FmPatch& Tune::basePatch(const TuneTrack& track) const
{
    const auto& patches = data_->import.patches;
    return track.instrument >= 0 && static_cast<size_t>(track.instrument) < patches.size()
               ? patches[static_cast<size_t>(track.instrument)]
               : defaultPatch();
}

int64_t Tune::length() const
{
    // A written note past the rip's end makes the song longer, with a second
    // for its release. With no rip, the room to write in follows the tempo
    // and the meter.
    int64_t end = ripped() ? static_cast<int64_t>(data_->source->length) : std::llround(grid.barLength() * 8);
    for (const TuneTrack& t : tracks)
        for (const TuneNote& n : t.notes)
            if (n.written())
                end = std::max<int64_t>(end, n.start + n.length + static_cast<int64_t>(kVgmRate));
    return end;
}

void Tune::setTempo(double bpm)
{
    const double before = grid.beatLength;
    grid.beatLength = 60.0 * kVgmRate / std::clamp(bpm, SongGrid::kMinTempo, SongGrid::kMaxTempo);
    if (ripped())
        return;
    const double scale = grid.beatLength / before;
    for (TuneTrack& t : tracks)
        for (TuneNote& n : t.notes)
        {
            n.start = std::max<int64_t>(0, std::llround(grid.firstBeat + (static_cast<double>(n.start) - grid.firstBeat) * scale));
            n.length = std::max<int64_t>(1, std::llround(static_cast<double>(n.length) * scale));
        }
}

int64_t Tune::originalStart(const TuneTrack& track, const TuneNote& note) const
{
    if (note.written())
        return note.start;
    return track.kind == TuneTrack::Kind::Fm ? data_->log.notes[static_cast<size_t>(note.source)].start
                                             : data_->log.hits[static_cast<size_t>(note.source)].start;
}

int64_t Tune::originalLength(const TuneTrack& track, const TuneNote& note) const
{
    if (note.written())
        return note.length;
    if (track.kind == TuneTrack::Kind::Drum)
        return data_->hitLength[static_cast<size_t>(note.source)];
    const LogNote& n = data_->log.notes[static_cast<size_t>(note.source)];
    return std::max<int64_t>(1, n.end - n.start);
}

int64_t Tune::firstWrite(const TuneTrack& track, const TuneNote& note, int channel) const
{
    int64_t first = 0;
    // A note moved in time or to another channel has no set-up (see build).
    if (track.kind == TuneTrack::Kind::Fm && !note.written() && note.start == originalStart(track, note)
        && channel == logChannel(track, note))
        for (const LogWrite& w : data_->log.notes[static_cast<size_t>(note.source)].writes)
            first = std::min(first, w.offset);
    return note.start + first;
}

uint8_t Tune::voiceSides(const TuneTrack& track, int channel) const
{
    std::array<int, 4> count {};
    for (const TuneNote& n : track.notes)
        if (track.kind == TuneTrack::Kind::Fm && originalChannel(track, n) == channel)
            ++count[n.written() ? 3 : static_cast<size_t>(data_->fmState[static_cast<size_t>(n.source)].regs[0] >> 6)];
    const auto most = std::max_element(count.begin(), count.end());
    return *most == 0 ? 0 : static_cast<uint8_t>((most - count.begin()) << 6);
}

int Tune::noteLevel(const TuneTrack& track, const TuneNote& note, int op) const
{
    if (track.kind != TuneTrack::Kind::Fm)
        return 0;
    if (note.written())
        return basePatch(track).op[static_cast<size_t>(op)].tl;
    // A note's registers run by slot: M1, M2, C1, C2, each six from the TL's place.
    static constexpr int kBlock[4] = { 0, 2, 1, 3 };
    return data_->fmState[static_cast<size_t>(note.source)].regs[static_cast<size_t>(4 + kBlock[op] * 6 + 1)] & 0x7F;
}

int Tune::logChannel(const TuneTrack& track, const TuneNote& note) const
{
    // A written drum hit with no channel of its own asks for the last,
    // which games use least.
    if (note.written())
        return note.voice >= 0 || track.kind == TuneTrack::Kind::Fm ? note.voice : kPcmChannels - 1;
    return track.kind == TuneTrack::Kind::Fm ? data_->log.notes[static_cast<size_t>(note.source)].channel
                                             : data_->log.hits[static_cast<size_t>(note.source)].channel;
}

int Tune::originalChannel(const TuneTrack& track, const TuneNote& note) const
{
    if (track.kind == TuneTrack::Kind::Fm && note.voice >= 0)
        return note.voice;
    return logChannel(track, note);
}

void Tune::moveToVoice(const TuneTrack& track, TuneNote& note, int channel) const
{
    if (track.kind != TuneTrack::Kind::Fm)
        return;
    note.voice = std::clamp(channel, 0, kFmChannels - 1);
    if (!note.written() && note.voice == logChannel(track, note))
        note.voice = -1;
}

TuneNote Tune::writtenCopy(const TuneTrack& track, const TuneNote& note, int channel) const
{
    // The copy sounds where the note does: its nearest key, and the rest,
    // a ripped note's key fraction among it, as fine tune.
    TuneNote copy = note;
    copy.source = -1;
    const double exact = exactPitch(track, note);
    copy.transpose = static_cast<int>(std::lround(exact)) - 60;
    copy.fine = std::clamp(static_cast<int>(std::lround((exact - 60.0 - copy.transpose) * 64.0)), -TuneNote::kMaxFine, TuneNote::kMaxFine);
    copy.voice = std::clamp(channel, 0, kFmChannels - 1);
    return copy;
}

int Tune::pitch(const TuneTrack& track, const TuneNote& note) const
{
    if (track.kind == TuneTrack::Kind::Drum || note.written())
        return 60 + note.transpose + static_cast<int>(std::lround(note.fine / 64.0));
    // Key code and fraction are registers 1 and 2 of the list. The game's key
    // rounds as it always has, so a transposition moves it by whole keys.
    const auto& regs = data_->fmState[static_cast<size_t>(note.source)].regs;
    const double key = 69.0 + keyCodePitch(regs[1], regs[2]) - 56.0 + data_->clockShift;
    if (note.fine == 0)
        return static_cast<int>(std::lround(key)) + note.transpose;
    return static_cast<int>(std::lround(key + note.fine / 64.0)) + note.transpose;
}

double Tune::exactPitch(const TuneTrack& track, const TuneNote& note) const
{
    const double fine = note.fine / 64.0;
    if (track.kind == TuneTrack::Kind::Drum || note.written())
        return 60.0 + note.transpose + fine;
    const auto& regs = data_->fmState[static_cast<size_t>(note.source)].regs;
    return 69.0 + keyCodePitch(regs[1], regs[2]) - 56.0 + data_->clockShift + note.transpose + fine;
}

NoteExpression Tune::expression(const TuneTrack& track, const TuneNote& note) const
{
    NoteExpression out;
    if (note.written())
        return out;
    const bool fm = track.kind == TuneTrack::Kind::Fm;
    const auto& writes = fm ? data_->log.notes[static_cast<size_t>(note.source)].writes
                            : data_->log.hits[static_cast<size_t>(note.source)].writes;
    const int64_t originalLen = originalLength(track, note);
    const int64_t stretch = note.length - originalLen;

    // The writes after the key on or trigger; the ones before it set up the
    // state the note starts from.
    size_t first = writes.size();
    for (size_t i = 0; i < writes.size(); ++i)
        if (writes[i].offset > 0 || (writes[i].offset == 0 && (fm ? isKeyOn(writes[i].address, writes[i].value) : isTrigger(writes[i].address, writes[i].value))))
        {
            first = writes[i].offset > 0 ? i : i + 1;
            break;
        }

    // The curves as whole chip values first, so a change is exact: a point
    // at an offset that already has one replaces it, and one that comes back
    // to the value before it goes.
    using Curve = std::vector<std::pair<int64_t, int>>;
    Curve pitch, level;
    auto set = [](Curve& curve, int64_t offset, int value, int base) {
        if (!curve.empty() && curve.back().first == offset)
            curve.pop_back();
        if (value != (curve.empty() ? base : curve.back().second))
            curve.emplace_back(offset, value);
    };
    auto offsetOf = [&](const LogWrite& w) { return w.offset >= originalLen && w.offset > 0 ? w.offset + stretch : w.offset; };

    if (fm)
    {
        // Pitch in 64ths of a semitone, level as the loudest carrier's
        // total level in 0.75 dB steps; the clock moves every pitch alike.
        const auto& regs = data_->fmState[static_cast<size_t>(note.source)].regs;
        int connection = regs[0] & 7;
        uint8_t kc = transposeKeyCode(regs[1], note.transpose), kf = regs[2];
        std::array<int, 4> tl {};
        for (size_t k = 0; k < 4; ++k)
            tl[k] = regs[5 + 6 * k] & 0x7F;
        auto steps = [&] {
            const int code = kc & 15;
            return ((kc >> 4 & 7) * 12 + code - code / 4) * 64 + (kf >> 2);
        };
        auto loudest = [&] {
            int least = 127;
            for (size_t k = 0; k < 4; ++k)
                if ((kCarriers[connection] >> k) & 1)
                    least = std::min(least, tl[k]);
            return least;
        };
        const int pitch0 = steps(), level0 = loudest();
        for (size_t i = first; i < writes.size(); ++i)
        {
            const LogWrite& w = writes[i];
            const int64_t offset = offsetOf(w);
            if (offset >= note.length)
                continue;
            if (w.address >= 0x20 && w.address < 0x28)
                connection = w.value & 7;
            else if (w.address >= 0x28 && w.address < 0x30)
                kc = transposeKeyCode(w.value, note.transpose);
            else if (w.address >= 0x30 && w.address < 0x38)
                kf = w.value;
            else if (w.address >= 0x60 && w.address < 0x80)
                tl[static_cast<size_t>((w.address - 0x60) >> 3)] = w.value & 0x7F;
            else
                continue;
            set(pitch, offset, steps(), pitch0);
            set(level, offset, loudest(), level0);
        }
        for (const auto& [offset, value] : pitch)
            out.pitch.push_back({ offset, static_cast<float>(value - pitch0) / 64.0f });
        for (const auto& [offset, value] : level)
            out.level.push_back({ offset, static_cast<float>(level0 - value) * 0.75f });
        return out;
    }

    // A drum hit: its step and its louder volume, as the build writes them
    // with the track's sample edit over the game's values.
    const PcmVoice voice = hitVoice(track, note);
    const auto& state = data_->pcmState[static_cast<size_t>(note.source)].regs;
    const SampleEdit edit = rippedEdit(track, note);
    int left = voice.regs[0] & 0x7F, right = voice.regs[1] & 0x7F, step = voice.regs[5];
    const int volume0 = std::max(left, right), step0 = step;
    for (size_t i = first; i < writes.size(); ++i)
    {
        const LogWrite& w = writes[i];
        const int64_t offset = offsetOf(w);
        if (offset >= note.length)
            continue;
        const int reg = w.address & 0x87;
        if (reg == 0x02)
            left = (edit.left ? scaleTo(w.value, state[0], *edit.left, 127) : w.value) & 0x7F;
        else if (reg == 0x03)
            right = (edit.right ? scaleTo(w.value, state[1], *edit.right, 127) : w.value) & 0x7F;
        else if (reg == 0x07)
            step = transposeStep(edit.step ? scaleTo(w.value, state[5], *edit.step, 255) : w.value, note.transpose);
        else
            continue;
        set(pitch, offset, step, step0);
        set(level, offset, std::max(left, right), volume0);
    }
    // A hit that starts silent or at no step has nothing to be relative to.
    if (step0 > 0)
        for (const auto& [offset, value] : pitch)
            out.pitch.push_back({ offset, static_cast<float>(12.0 * std::log2(std::max(value, 1) / static_cast<double>(step0))) });
    if (volume0 > 0)
        for (const auto& [offset, value] : level)
            out.level.push_back({ offset, value == 0 ? TuneTrack::kSilent
                                                     : std::max(TuneTrack::kSilent, static_cast<float>(20.0 * std::log10(value / static_cast<double>(volume0)))) });
    return out;
}

const PadSample* Tune::sample(const TuneTrack& track) const
{
    const auto& all = samples();
    if (track.kind != TuneTrack::Kind::Drum || track.instrument < 0 || static_cast<size_t>(track.instrument) >= all.size())
        return nullptr;
    return &all[static_cast<size_t>(track.instrument)];
}

const std::vector<LoadedSample>& Tune::loadedSamples() const
{
    static const std::vector<LoadedSample> kNone;
    return samples_ ? samples_->loaded : kNone;
}

bool Tune::sampleLoaded(int index) const
{
    const auto& loaded = loadedSamples();
    return std::any_of(loaded.begin(), loaded.end(), [index](const LoadedSample& l) { return l.index == index; });
}

int Tune::loadSample(int index, const PadSample& sample)
{
    std::vector<LoadedSample> loaded = loadedSamples();
    const auto it = std::find_if(loaded.begin(), loaded.end(), [index](const LoadedSample& l) { return l.index == index; });
    if (index < 0 || static_cast<size_t>(index) >= samples().size())
    {
        index = static_cast<int>(samples().size());
        loaded.push_back({ index, sample });
    }
    else if (it != loaded.end())
        it->sample = sample;
    else
        loaded.push_back({ index, sample });
    return setLoadedSamples(loaded) ? index : -1;
}

bool Tune::setLoadedSamples(const std::vector<LoadedSample>& loaded)
{
    if (loaded.empty())
    {
        samples_.reset();
        return true;
    }
    // From the rip's samples and ROM each time, so the same samples loaded
    // in the same order always land in the same places. Every page the rip's
    // song reads stays as it is, its samples' and any other, such as a loop
    // before a sample's start or a bank read through the ROM's mirror. A rip
    // that reads through the mirror keeps the ROM within its power of two,
    // as a larger one would change what the mirror reads.
    auto set = std::make_shared<SampleSet>();
    set->samples = data_->import.samples;
    set->romSize = data_->source->rom.size();
    const RomReads reads = romReads(*data_->source);
    RomRanges used = samplePages(set->samples);
    used.insert(used.end(), reads.ranges.begin(), reads.ranges.end());
    const size_t maxSize = reads.mirrored ? reads.span : kMaxRomSize;
    for (const LoadedSample& l : loaded)
    {
        if (l.index < 0 || static_cast<size_t>(l.index) > set->samples.size())
            return false;
        PadSample placed = l.sample;
        if (!placeSample(set->romSize, data_->source->header.segaPcmInterface, used, placed, maxSize))
            return false;
        if (static_cast<size_t>(l.index) == set->samples.size())
            set->samples.push_back(std::move(placed));
        else
            set->samples[static_cast<size_t>(l.index)] = std::move(placed);
        set->loaded.push_back(l);
    }
    samples_ = std::move(set);
    return true;
}

std::vector<uint8_t> Tune::rom() const
{
    std::vector<uint8_t> out = data_->source->rom;
    if (!samples_)
        return out;
    out.resize(std::max(out.size(), samples_->romSize), 0x80);
    for (const LoadedSample& l : samples_->loaded)
        writeSample(out, samples_->samples[static_cast<size_t>(l.index)]);
    return out;
}

uint8_t Tune::romByte(size_t at) const
{
    if (samples_)
        for (const LoadedSample& l : samples_->loaded)
        {
            const PadSample& s = samples_->samples[static_cast<size_t>(l.index)];
            const size_t from = s.romOffset + s.address, to = (from + s.data.size() + 0xFF) & ~size_t { 0xFF };
            if (at >= from && at < to)
                return at - from < s.data.size() ? s.data[at - from] : uint8_t { 0x80 };
        }
    const auto& source = data_->source->rom;
    return at < source.size() ? source[at] : uint8_t { 0x80 };
}

const PadSample* Tune::replacement(const TuneTrack& track) const
{
    if (!samples_ || track.kind != TuneTrack::Kind::Drum || track.instrument < 0
        || static_cast<size_t>(track.instrument) >= data_->import.samples.size() || !sampleLoaded(track.instrument))
        return nullptr;
    return &samples_->samples[static_cast<size_t>(track.instrument)];
}

SampleEdit Tune::rippedEdit(const TuneTrack& track, const TuneNote& note) const
{
    const PadSample* pad = sample(track);
    if (pad == nullptr)
        return track.sample;
    SampleEdit e = track.sample.fitted(*pad);
    if (note.written() || replacement(track) == nullptr)
        return e;
    if (!e.start)
        e.start = sampleFirst(*pad);
    if (!e.endPage)
        e.endPage = sampleLastPage(*pad);
    if (!e.loop)
        e.loop = pad->loop;
    if (!e.step)
    {
        const uint8_t game = data_->pcmState[static_cast<size_t>(note.source)].regs[5];
        e.step = std::max<uint8_t>(1, scaleTo(game, data_->import.samples[static_cast<size_t>(track.instrument)].step, pad->step, 255));
    }
    return e;
}

uint8_t Tune::withBank(const TuneTrack& track, uint8_t flags) const
{
    const PadSample* pad = replacement(track);
    if (pad == nullptr)
        return flags;
    const uint32_t mask = SegaPcm::banking(data_->source->header.segaPcmInterface).mask;
    return static_cast<uint8_t>((static_cast<uint32_t>(flags) & ~mask) | (static_cast<uint32_t>(pad->bank) & mask));
}

PcmVoice Tune::sampleVoice(const TuneTrack& track, int transpose, int fine) const
{
    PcmVoice v;
    const PadSample* pad = sample(track);
    if (pad == nullptr)
        return v;
    const SampleEdit e = track.sample.fitted(*pad);
    const uint16_t start = e.start ? *e.start : sampleFirst(*pad);
    const uint8_t endPage = e.endPage ? *e.endPage : sampleLastPage(*pad);
    const bool loops = e.loop ? *e.loop : pad->loop;
    // The sample's own loop point stays between an edited start and end.
    const auto loopStart = static_cast<uint16_t>(
        e.loopStart ? *e.loopStart : std::clamp<int64_t>(int64_t { sampleFirst(*pad) } + pad->loopStart, start, ((endPage + 1) << 8) - 1));
    v.regs[0] = e.left ? *e.left : pad->left;
    v.regs[1] = e.right ? *e.right : pad->right;
    v.regs[2] = static_cast<uint8_t>(loopStart);
    v.regs[3] = static_cast<uint8_t>(loopStart >> 8);
    v.regs[4] = endPage;
    v.regs[5] = transposeStep(e.step ? *e.step : pad->step, transpose, fine);
    v.regs[6] = static_cast<uint8_t>(start);
    v.regs[7] = static_cast<uint8_t>(start >> 8);
    v.flags = static_cast<uint8_t>((pad->bank & 0xFC) | (loops ? 0x00 : 0x02));
    return v;
}

PcmVoice Tune::hitVoice(const TuneTrack& track, const TuneNote& note, bool expressed) const
{
    const int fine = expressed ? note.fine : 0;
    PcmVoice v = note.written() || track.kind != TuneTrack::Kind::Drum ? sampleVoice(track, note.transpose, fine) : rippedVoice(track, note, fine);
    if (expressed)
    {
        const auto volumes = noteVolumes(v.regs[0], v.regs[1], note);
        v.regs[0] = volumes[0];
        v.regs[1] = volumes[1];
    }
    return v;
}

PcmVoice Tune::rippedVoice(const TuneTrack& track, const TuneNote& note, int fine) const
{
    PcmVoice v;
    const auto& regs = data_->pcmState[static_cast<size_t>(note.source)].regs;
    std::copy(regs.begin(), regs.end(), v.regs.begin());
    const PadSample* pad = sample(track);
    const SampleEdit e = rippedEdit(track, note);
    if (e.left)
        v.regs[0] = *e.left;
    if (e.right)
        v.regs[1] = *e.right;
    if (e.endPage)
        v.regs[4] = *e.endPage;
    if (e.step)
        v.regs[5] = *e.step;
    v.regs[5] = transposeStep(v.regs[5], note.transpose, fine);
    if (e.start)
        v.regs[6] = static_cast<uint8_t>(*e.start), v.regs[7] = static_cast<uint8_t>(*e.start >> 8);
    // The chip stops only on the page after the end page, so a start moved
    // past this hit's own end raises the end to the start's page.
    const int start = v.regs[6] | v.regs[7] << 8;
    if (e.start || e.endPage)
        v.regs[4] = std::max(v.regs[4], static_cast<uint8_t>(start >> 8));
    // The flags come from the hit's trigger in the log.
    for (const LogWrite& w : data_->log.hits[static_cast<size_t>(note.source)].writes)
        if (w.offset == 0 && isTrigger(w.address, w.value))
            v.flags = withBank(track, w.value);
    const bool gameLoops = !(v.flags & 0x02);
    if (e.loop)
        v.flags = static_cast<uint8_t>((v.flags & ~0x02) | (*e.loop ? 0x00 : 0x02));
    // The loop address: the edit's; for a one-shot made to loop, the
    // sample's own, as the Sample window shows it; else the game's. It stays
    // between the hit's start and end.
    if (setsLoop(e))
    {
        int64_t loopStart = v.regs[2] | v.regs[3] << 8;
        if (e.loopStart)
            loopStart = *e.loopStart;
        else if (!gameLoops && e.loop.value_or(false) && pad != nullptr)
            loopStart = int64_t { sampleFirst(*pad) } + pad->loopStart;
        loopStart = std::clamp<int64_t>(loopStart, start, ((v.regs[4] + 1) << 8) - 1);
        v.regs[2] = static_cast<uint8_t>(loopStart);
        v.regs[3] = static_cast<uint8_t>(loopStart >> 8);
    }
    return v;
}

int64_t Tune::sampleLength(const TuneTrack& track, int transpose, int fine) const
{
    const PcmVoice v = sampleVoice(track, transpose, fine);
    if (!(v.flags & 0x02))
        return 0;
    // The chip plays from the start address until its address reaches the
    // page after the end page, step / 256 bytes a chip sample.
    const int from = v.regs[6] | v.regs[7] << 8;
    const int to = (v.regs[4] + 1) << 8;
    const double bytes = std::max(1, to - from);
    const double seconds = bytes * 256.0 / std::max<uint8_t>(v.regs[5], 1) / data_->source->pcmRate;
    return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(seconds * kVgmRate)));
}

int Tune::newNoteId() const
{
    int id = 0;
    for (const TuneTrack& t : tracks)
        for (const TuneNote& n : t.notes)
            id = std::max(id, n.id + 1);
    return id;
}

// A muted track or voice is silent, soloed or not. Otherwise, while any
// track or voice is soloed, only the soloed ones play; a soloed track plays
// all its voices but the muted ones.
bool Tune::audible(const TuneTrack& track) const
{
    if (track.mute)
        return false;
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const TuneTrack& t) { return t.solo || t.soloedVoices != 0; });
    return !anySolo || track.solo || track.soloedVoices != 0;
}

bool Tune::audible(const TuneTrack& track, const TuneNote& note) const
{
    if (!audible(track))
        return false;
    if (track.kind != TuneTrack::Kind::Fm)
        return true;
    const int voice = originalChannel(track, note);
    if ((track.mutedVoices >> voice) & 1)
        return false;
    // A track heard for its soloed voices plays only those.
    return track.solo || track.soloedVoices == 0 || ((track.soloedVoices >> voice) & 1);
}

std::vector<int> Tune::voices(const TuneTrack& track) const
{
    if (track.kind != TuneTrack::Kind::Fm)
        return {};
    std::array<double, 8> sum {};
    std::array<int, 8> count {};
    for (const TuneNote& n : track.notes)
    {
        const auto c = static_cast<size_t>(originalChannel(track, n));
        sum[c] += pitch(track, n) - n.transpose;
        ++count[c];
    }
    std::vector<int> result;
    for (int c = 0; c < 8; ++c)
        if (count[static_cast<size_t>(c)] > 0 || ((track.addedVoices >> c) & 1))
            result.push_back(c);
    // A voice with no notes yet goes last.
    auto average = [&](int c) { return count[static_cast<size_t>(c)] > 0 ? sum[static_cast<size_t>(c)] / count[static_cast<size_t>(c)] : -1.0; };
    std::stable_sort(result.begin(), result.end(), [&](int a, int b) { return average(a) > average(b); });
    return result;
}

int Tune::freeVoice(const TuneTrack& track, int64_t start, int64_t end) const
{
    std::vector<int> all = voices(track);
    if (all.empty())
        return -1;
    std::sort(all.begin(), all.end());
    for (const int v : all)
        if (std::none_of(track.notes.begin(), track.notes.end(), [&](const TuneNote& n) {
                return originalChannel(track, n) == v && n.start < end && start < n.start + n.length;
            }))
            return v;
    return all.front();
}

std::vector<Tune::Ref> Tune::allocate(bool fm, BuildReport& rep) const
{
    std::vector<Ref> refs, written;
    for (size_t t = 0; t < tracks.size(); ++t)
        if ((tracks[t].kind == TuneTrack::Kind::Fm) == fm)
            for (size_t n = 0; n < tracks[t].notes.size(); ++n)
                (fm || !tracks[t].notes[n].written() ? refs : written).push_back({ t, n });
    auto byStart = [&](const Ref& a, const Ref& b) { return tracks[a.track].notes[a.note].start < tracks[b.track].notes[b.note].start; };
    std::stable_sort(refs.begin(), refs.end(), byStart);
    std::stable_sort(written.begin(), written.end(), byStart);
    const int channels = fm ? kFmChannels : kPcmChannels;
    std::vector<int64_t> busyUntil(static_cast<size_t>(channels), std::numeric_limits<int64_t>::min());
    // Drums: the time each channel is taken, for the written hits to fit around.
    std::vector<std::vector<std::pair<int64_t, int64_t>>> taken(fm ? 0 : static_cast<size_t>(channels));
    for (const Ref& ref : refs)
    {
        const TuneNote& note = tracks[ref.track].notes[ref.note];
        const int original = originalChannel(tracks[ref.track], note);
        int ch = original;
        if (busyUntil[static_cast<size_t>(ch)] > note.start)
        {
            ch = -1;
            for (int c = 0; c < channels; ++c)
                if (busyUntil[static_cast<size_t>(c)] <= note.start
                    && (ch < 0 || busyUntil[static_cast<size_t>(c)] < busyUntil[static_cast<size_t>(ch)]))
                    ch = c;
        }
        rep.channels[ref.track][ref.note] = ch;
        if (ch < 0)
        {
            ++rep.conflicts;
            ch = original;
        }
        busyUntil[static_cast<size_t>(ch)] = std::max(busyUntil[static_cast<size_t>(ch)], note.start + note.length);
        if (!fm)
        {
            // A ripped hit's writes run on past a one-shot's end, up to the
            // next set-up on its channel, and a stop, a volume or a step
            // there would cut or change a written hit. The channel is taken
            // from the hit's set-up to its last write, moved as the build
            // moves them.
            const TuneTrack& track = tracks[ref.track];
            const bool moved = note.start != originalStart(track, note) || ch != logChannel(track, note);
            const int64_t originalLen = originalLength(track, note), stretch = note.length - originalLen;
            int64_t from = note.start, to = note.start + note.length;
            for (const LogWrite& w : data_->log.hits[static_cast<size_t>(note.source)].writes)
            {
                if (moved && w.offset < 0)
                    continue;
                const int64_t at = note.start + w.offset + (w.offset >= originalLen && w.offset > 0 ? stretch : 0);
                from = std::min(from, at);
                to = std::max(to, at + 1);
            }
            taken[static_cast<size_t>(ch)].emplace_back(from, to);
        }
    }

    // A written drum hit takes the channel it asks for when that is free all
    // through it, else the free channel idle longest, else it shares the one
    // it asks for.
    for (const Ref& ref : written)
    {
        const TuneNote& note = tracks[ref.track].notes[ref.note];
        const int64_t from = note.start, to = note.start + note.length;
        auto idleSince = [&](int c) {
            int64_t since = std::numeric_limits<int64_t>::min();
            for (const auto& [a, b] : taken[static_cast<size_t>(c)])
            {
                if (a < to && b > from)
                    return std::optional<int64_t>();
                if (b <= from)
                    since = std::max(since, b);
            }
            return std::optional<int64_t>(since);
        };
        const int original = originalChannel(tracks[ref.track], note);
        int ch = -1;
        if (idleSince(original))
            ch = original;
        else
        {
            std::optional<int64_t> best;
            for (int c = 0; c < channels; ++c)
                if (const auto since = idleSince(c); since && (!best || *since < *best))
                    best = since, ch = c;
        }
        rep.channels[ref.track][ref.note] = ch;
        if (ch < 0)
        {
            ++rep.conflicts;
            ch = original;
        }
        taken[static_cast<size_t>(ch)].emplace_back(from, to);
        refs.push_back(ref);
    }
    return refs;
}

BuildReport Tune::channels() const
{
    BuildReport rep;
    rep.channels.assign(tracks.size(), {});
    for (size_t t = 0; t < tracks.size(); ++t)
        rep.channels[t].assign(tracks[t].notes.size(), -1);
    allocate(true, rep);
    allocate(false, rep);
    return rep;
}

size_t Tune::moveConflicts(size_t& stuck)
{
    const BuildReport rep = channels();
    // A note with no free channel shares one wherever it asks: every
    // channel is busy when it starts, so no move gives it one of its own.
    stuck = rep.conflicts;
    std::vector<std::vector<int>> before;
    for (const TuneTrack& t : tracks)
        before.push_back(voices(t));
    // Each note asks for the channel it was given, so the build gives every
    // note the same channel as before.
    size_t moved = 0;
    for (size_t t = 0; t < tracks.size(); ++t)
        if (tracks[t].kind == TuneTrack::Kind::Fm)
            for (size_t n = 0; n < tracks[t].notes.size(); ++n)
            {
                TuneNote& note = tracks[t].notes[n];
                const int ch = rep.channels[t][n];
                if (ch >= 0 && ch != originalChannel(tracks[t], note))
                {
                    moveToVoice(tracks[t], note, ch);
                    ++moved;
                }
            }
    // The voices the notes left stay with their tracks, as rows to put notes on.
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        const std::vector<int> now = voices(tracks[t]);
        for (const int v : before[t])
            if (std::find(now.begin(), now.end(), v) == now.end())
                tracks[t].addedVoices = static_cast<uint8_t>(tracks[t].addedVoices | 1 << v);
    }
    return moved;
}

std::shared_ptr<VgmSong> Tune::build(BuildReport* report, bool mix) const
{
    BuildReport local;
    BuildReport& rep = report ? *report : local;
    rep.channels.assign(tracks.size(), {});
    rep.conflicts = 0;
    for (size_t t = 0; t < tracks.size(); ++t)
        rep.channels[t].assign(tracks[t].notes.size(), -1);

    std::vector<Out> ym, pcm;
    std::vector<Request> requests;
    for (const LogWrite& w : data_->log.ymSong)
        ym.push_back({ w.offset, uint64_t { w.order } * 2 + 1, w.address, w.value });
    for (const LogWrite& w : data_->log.pcmSong)
        pcm.push_back({ w.offset, uint64_t { w.order } * 2 + 1, w.address, w.value });

    for (const bool fm : { true, false })
    {
        // Notes of this chip in order of start, each on the channel it was given.
        for (const Ref& ref : allocate(fm, rep))
        {
            const TuneTrack& track = tracks[ref.track];
            const TuneNote& note = track.notes[ref.note];
            const int original = originalChannel(track, note);
            int ch = rep.channels[ref.track][ref.note];
            if (ch < 0)
                ch = original;

            const bool muted = mix && (!audible(track, note) || track.volume <= TuneTrack::kSilent);
            const float volume = mix ? std::min(track.volume, TuneTrack::kMaxVolume) : 0.0f;
            const int steps = static_cast<int>(std::lround(-volume / 0.75f));
            const float gain = std::pow(10.0f, volume / 20.0f);
            // FM: the note's velocity lowers its carriers' total level on top
            // of the track's volume. The carriers are those of the connection
            // the player sounds: the track's edited one when its patch changes
            // the game's, else the one the game writes.
            const int carrierSteps = steps - std::clamp(note.velocity, TuneNote::kMinVelocity, 0);
            const std::optional<int> editedConnection = fm && track.patch && track.patch->alg != basePatch(track).alg
                                                            ? std::optional<int>(track.patch->alg & 7)
                                                            : std::nullopt;
            auto carriersOf = [&editedConnection](int written) { return kCarriers[editedConnection.value_or(written) & 7]; };
            const int fine = std::clamp(note.fine, -TuneNote::kMaxFine, TuneNote::kMaxFine);
            // A note moved to another voice is moved too: its set-up was
            // written for the game's channel.
            const bool moved = note.start != originalStart(track, note) || ch != logChannel(track, note);
            const int64_t originalLen = originalLength(track, note);
            const int64_t stretch = note.length - originalLen;
            static const std::vector<LogWrite> kNone;
            const auto& writes = note.written() ? kNone
                               : fm             ? data_->log.notes[static_cast<size_t>(note.source)].writes
                                    : data_->log.hits[static_cast<size_t>(note.source)].writes;

            // Drums: the track's sample edit, and the game's registers at
            // the trigger, which the hit's later writes are moved against.
            const PadSample* pad = fm ? nullptr : sample(track);
            const SampleEdit edit = pad != nullptr ? rippedEdit(track, note) : SampleEdit {};
            static const std::array<uint8_t, kPcmRegs> kNoState {};
            const auto& state = fm || note.written() ? kNoState : data_->pcmState[static_cast<size_t>(note.source)].regs;
            PcmVoice voice;       // the registers the hit starts with
            bool oneShot = false; // the game's trigger plays the sample once

            std::array<uint8_t, 4> gameTl {}; // FM: the operators' total levels as the game has them now, by slot block
            Request request;
            request.channel = ch;
            request.regs.fill(0);
            if (fm)
            {
                if (note.written())
                    request.regs = writtenRegs(basePatch(track), exactPitch(track, note), data_->clockShift);
                else
                {
                    request.regs = data_->fmState[static_cast<size_t>(note.source)].regs;
                    std::tie(request.regs[1], request.regs[2]) = shiftKey(request.regs[1], request.regs[2], note.transpose, fine);
                }
                if (note.pan)
                    request.regs[0] = static_cast<uint8_t>((request.regs[0] & 0x3F) | TuneNote::sides(*note.pan));
                if (muted)
                    request.regs[0] &= 0x3F; // no output, left or right
                for (size_t op = 0; op < 4; ++op)
                    gameTl[op] = request.regs[5 + 6 * op];
                if (carrierSteps != 0)
                    for (size_t op = 0; op < 4; ++op)
                        if ((carriersOf(request.regs[0]) >> op) & 1)
                            request.regs[5 + 6 * op] = attenuate(request.regs[5 + 6 * op], carrierSteps);
            }
            else
            {
                // The registers the hit starts with: the game's, or a written
                // hit's sample, with the track's edit and the note's
                // expression over them.
                voice = hitVoice(track, note, true);
                std::copy(voice.regs.begin(), voice.regs.end(), request.regs.begin());
                if (muted)
                    request.regs[0] = request.regs[1] = 0;
                else if (steps != 0)
                    request.regs[0] = scaleVolume(request.regs[0], gain), request.regs[1] = scaleVolume(request.regs[1], gain);
                request.forceAddress = moved || note.written() || edit.start.has_value();
            }
            const int requestIndex = static_cast<int>(requests.size());
            requests.push_back(request);

            bool ended = false;
            int connection = fm ? request.regs[0] & 7 : 0;
            // A ripped note's pitch and drum volumes as the game has them
            // now, and as written to the chip. Fine tune can carry a key
            // fraction into the key code, and a pan shares a hit's two volumes,
            // so a write to one can change the other: it is written after.
            uint8_t gameKc = 0, gameKf = 0;
            if (fm && !note.written())
                gameKc = data_->fmState[static_cast<size_t>(note.source)].regs[1], gameKf = data_->fmState[static_cast<size_t>(note.source)].regs[2];
            uint8_t chipKc = request.regs[1], chipKf = request.regs[2];
            const PcmVoice base = fm ? PcmVoice {} : hitVoice(track, note);
            std::array<uint8_t, 2> gameVolume { base.regs[0], base.regs[1] }, chipVolume { voice.regs[0], voice.regs[1] };
            const bool expressVolumes = !fm && (note.velocity != 0 || note.pan);
            // A drum volume after the note's expression, as the mix has it.
            auto mixVolume = [&](uint8_t value) {
                if (muted)
                    return uint8_t { 0 };
                return steps != 0 ? scaleVolume(value, gain) : value;
            };
            for (const LogWrite& w : writes)
            {
                // A moved note's set-up is replaced by the corrections.
                if (moved && w.offset < 0)
                    continue;
                int64_t offset = w.offset;
                uint16_t address = w.address;
                uint8_t value = w.value;
                // The key off and what follows it move with the note's end.
                if (offset >= originalLen && offset > 0)
                    offset += stretch;
                if (fm)
                {
                    if (address == 0x08)
                        value = static_cast<uint8_t>((value & 0xF8) | ch);
                    else
                        address = static_cast<uint16_t>((address & 0xF8) | ch);
                    std::optional<Out> carry; // the other half of the pitch, when fine tune carries into it
                    if (address >= 0x28 && address < 0x38)
                    {
                        (address < 0x30 ? gameKc : gameKf) = value;
                        const auto [kc, kf] = shiftKey(gameKc, gameKf, note.transpose, fine);
                        value = address < 0x30 ? kc : kf;
                        if (fine != 0 && (address < 0x30 ? kf != chipKf : kc != chipKc))
                            carry = Out { note.start + offset, uint64_t { w.order } * 2 + 1,
                                          static_cast<uint16_t>(address < 0x30 ? 0x30 + ch : 0x28 + ch), address < 0x30 ? kf : kc };
                        if (address < 0x30 || carry)
                            chipKc = kc;
                        if (address >= 0x30 || carry)
                            chipKf = kf;
                    }
                    // A new connection can make other operators the carriers:
                    // those whose part changes are levelled again after it.
                    uint8_t relevel = 0;
                    if (address >= 0x20 && address < 0x28)
                    {
                        relevel = static_cast<uint8_t>(carriersOf(connection) ^ carriersOf(value));
                        connection = value & 7;
                    }
                    if (address >= 0x60 && address < 0x80)
                        gameTl[static_cast<size_t>((address - 0x60) >> 3)] = value;
                    if (note.pan && address >= 0x20 && address < 0x28)
                        value = static_cast<uint8_t>((value & 0x3F) | TuneNote::sides(*note.pan));
                    if (muted && address >= 0x20 && address < 0x28)
                        value &= 0x3F;
                    // The volume and the note's velocity lower the carriers'
                    // total level; a fade keeps its moves around it.
                    if (carrierSteps != 0 && address >= 0x60 && address < 0x80 && ((carriersOf(connection) >> ((address - 0x60) >> 3)) & 1))
                        value = attenuate(value, carrierSteps);
                    if (carrierSteps != 0 && relevel != 0)
                    {
                        ym.push_back({ note.start + offset, uint64_t { w.order } * 2 + 1, address, value, -1 });
                        for (size_t op = 0; op < 4; ++op)
                            if ((relevel >> op) & 1)
                                ym.push_back({ note.start + offset, uint64_t { w.order } * 2 + 1, static_cast<uint16_t>(0x60 + 8 * static_cast<int>(op) + ch),
                                               ((carriersOf(connection) >> op) & 1) ? attenuate(gameTl[op], carrierSteps) : gameTl[op], -1 });
                        continue;
                    }
                    ended = ended || isKeyOff(address, value);
                    if (carry)
                    {
                        ym.push_back({ note.start + offset, uint64_t { w.order } * 2 + 1, address, value, -1 });
                        ym.push_back(*carry);
                        continue;
                    }
                }
                else
                {
                    address = static_cast<uint16_t>((address & 0x87) | (ch << 3));
                    const int reg = address & 0x87;
                    // The edit's fields take the place of the game's writes to
                    // them; a fade or a bend keeps its shape around the edited value.
                    if (reg == 0x07)
                        value = transposeStep(edit.step ? scaleTo(value, state[5], *edit.step, 255) : value, note.transpose, fine);
                    else if (reg == 0x02 && edit.left)
                        value = scaleTo(value, state[0], *edit.left, 127);
                    else if (reg == 0x03 && edit.right)
                        value = scaleTo(value, state[1], *edit.right, 127);
                    else if ((reg == 0x04 || reg == 0x05) && setsLoop(edit))
                        value = voice.regs[reg == 0x04 ? 2 : 3];
                    else if (reg == 0x06 && (edit.endPage || edit.start))
                        value = voice.regs[4];
                    else if ((reg == 0x84 || reg == 0x85) && edit.start)
                        value = static_cast<uint8_t>(reg == 0x84 ? *edit.start : *edit.start >> 8);
                    else if (reg == 0x86 && !(value & 1))
                    {
                        value = withBank(track, value);
                        if (w.offset == 0)
                            oneShot = (value & 0x02) != 0;
                        if (edit.loop)
                            value = static_cast<uint8_t>((value & ~0x02) | (*edit.loop ? 0x00 : 0x02));
                    }
                    if ((reg == 0x02 || reg == 0x03) && expressVolumes)
                    {
                        // The note's velocity and pan over the volumes as the
                        // game and the edit have them now; a pan's share moves
                        // the other side too.
                        const size_t side = reg == 0x02 ? 0 : 1, other = 1 - side;
                        gameVolume[side] = value;
                        const auto volumes = noteVolumes(gameVolume[0], gameVolume[1], note);
                        value = volumes[side];
                        chipVolume[side] = value;
                        if (volumes[other] != chipVolume[other])
                        {
                            chipVolume[other] = volumes[other];
                            pcm.push_back({ note.start + offset, uint64_t { w.order } * 2 + 1, address, mixVolume(value), -1 });
                            pcm.push_back({ note.start + offset, uint64_t { w.order } * 2 + 1,
                                            static_cast<uint16_t>(address ^ 0x01), mixVolume(volumes[other]), -1 });
                            continue;
                        }
                    }
                    if (reg == 0x02 || reg == 0x03)
                        value = mixVolume(value);
                }
                const bool start = w.offset == 0 && (fm ? isKeyOn(address, value) : isTrigger(address, value));
                (fm ? ym : pcm).push_back({ note.start + offset, uint64_t { w.order } * 2 + 1, address, value,
                                            start ? requestIndex : -1 });
            }

            // A written note: every operator keyed on at its start (the
            // player leaves out those switched off), and off at its end, before
            // any other write then.
            if (fm && note.written())
            {
                ym.push_back({ note.start, 1, 0x08, static_cast<uint8_t>(0x78 | ch), requestIndex });
                ym.push_back({ note.start + note.length, 0, 0x08, static_cast<uint8_t>(ch) });
            }

            // A written drum hit: its trigger, and a cut at its end when its
            // sample loops or would play on past it. A ripped one-shot made to
            // loop stops at its end too. A cut goes before any other write
            // then, so a hit starting there on the channel plays.
            if (!fm)
            {
                const auto cut = static_cast<uint16_t>(0x2000 | (ch * 8 + 0x86));
                if (note.written())
                {
                    pcm.push_back({ note.start, 1, static_cast<uint16_t>(ch * 8 + 0x86), voice.flags, requestIndex });
                    const int64_t natural = sampleLength(track, note.transpose, fine);
                    if (natural == 0 || note.length < natural)
                        pcm.push_back({ note.start + note.length, 0, cut, 0 });
                }
                else if (oneShot && edit.loop && *edit.loop && stretch >= 0)
                    pcm.push_back({ note.start + note.length, 0, cut, 0 });
            }

            // A shortened note without a key off of its own, or a drum cut
            // before its end, stops where it now ends.
            if (stretch < 0)
            {
                const uint64_t key = std::numeric_limits<uint64_t>::max() / 2;
                if (fm && !ended)
                    ym.push_back({ note.start + note.length, key, 0x08, static_cast<uint8_t>(ch) });
                if (!fm)
                    pcm.push_back({ note.start + note.length, key, static_cast<uint16_t>(0x2000 | (ch * 8 + 0x86)), 0 });
            }
        }
    }

    // Replay the writes in order and correct, before each key on or
    // trigger, every register of its channel that differs from what the note
    // had in the game. An unedited tune needs no corrections.
    auto earlier = [](const Out& a, const Out& b) { return a.time != b.time ? a.time < b.time : a.key < b.key; };
    std::stable_sort(ym.begin(), ym.end(), earlier);
    std::stable_sort(pcm.begin(), pcm.end(), earlier);

    auto song = std::make_shared<VgmSong>();
    const VgmSong& src = *data_->source;
    song->name = src.name;
    song->header = src.header;
    song->fmRate = src.fmRate;
    song->pcmRate = src.pcmRate;
    song->length = static_cast<uint32_t>(length());
    song->rom = rom();
    song->fmChannelsUsed = src.fmChannelsUsed;
    song->pcmUsed = src.pcmUsed;
    song->beatLength = grid.beatLength;
    song->firstBeat = grid.firstBeat;
    song->beatCount = std::max(1, static_cast<int>(std::ceil((song->length + 2.0 * kVgmRate - grid.firstBeat) / grid.beatLength)));

    auto time = [](int64_t t) { return static_cast<uint32_t>(std::max<int64_t>(0, t)); };
    std::array<uint8_t, 256> regs {};
    for (const Out& o : ym)
    {
        if (o.request >= 0)
        {
            const Request& r = requests[static_cast<size_t>(o.request)];
            for (int i = 0; i < kFmRegs; ++i)
            {
                const uint16_t address = fmRegister(r.channel, i);
                if (regs[address] != r.regs[static_cast<size_t>(i)])
                {
                    regs[address] = r.regs[static_cast<size_t>(i)];
                    song->ym.push_back({ time(o.time), address, regs[address] });
                }
            }
        }
        if (o.address != 0x08 && o.address < 256)
            regs[o.address] = o.value;
        song->ym.push_back({ time(o.time), o.address, o.value });
    }
    regs.fill(0);
    for (const Out& o : pcm)
    {
        if (o.request >= 0)
        {
            const Request& r = requests[static_cast<size_t>(o.request)];
            for (int i = 0; i < kPcmRegs; ++i)
            {
                const uint16_t address = pcmRegister(r.channel, i);
                // The chip moves a playing sample's address itself, so a
                // moved hit writes its start address whatever the copy says.
                const bool force = r.forceAddress && i >= kStartAddress;
                if (force || regs[address] != r.regs[static_cast<size_t>(i)])
                {
                    regs[address] = r.regs[static_cast<size_t>(i)];
                    song->pcm.push_back({ time(o.time), address, regs[address] });
                }
            }
        }
        if (!(o.address & 0x2000))
            regs[o.address & 0xFF] = o.value;
        song->pcm.push_back({ time(o.time), o.address, o.value });
    }
    appendSongEnd(*song);
    return song;
}

} // namespace opm86
