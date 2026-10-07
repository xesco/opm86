#include "Tune.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace opm86
{

namespace
{

constexpr int kFmChannels = 8;
constexpr int kPcmChannels = SegaPcm::kChannels;

// A note's channel registers: output and connection, key code, key
// fraction, LFO sensitivity, then six registers for each of its four
// operators (slots ch, ch + 8, ch + 16, ch + 24).
uint16_t fmRegister(int channel, int index)
{
    static constexpr uint16_t kChannelRegs[4] = { 0x20, 0x28, 0x30, 0x38 };
    static constexpr uint16_t kOperatorRegs[6] = { 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0 };
    if (index < 4)
        return static_cast<uint16_t>(kChannelRegs[index] + channel);
    const int op = (index - 4) / 6, reg = (index - 4) % 6;
    return static_cast<uint16_t>(kOperatorRegs[reg] + channel + 8 * op);
}
constexpr int kFmRegs = 28;

// A hit's channel registers: volumes, loop address, end page, step, start address.
uint16_t pcmRegister(int channel, int index)
{
    static constexpr uint16_t kRegs[8] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x84, 0x85 };
    return static_cast<uint16_t>(kRegs[index] + channel * 8);
}
constexpr int kPcmRegs = 8;
constexpr int kStartAddress = 6; // index of 0x84 in the list above

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

uint8_t transposeKeyCode(uint8_t kc, int semitones)
{
    if (semitones == 0)
        return kc; // the game's own value, even an unused code
    static constexpr uint8_t kCode[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
    const int code = kc & 15;
    const int n = std::clamp((kc >> 4 & 7) * 12 + code - code / 4 + semitones, 0, 95);
    return static_cast<uint8_t>((n / 12) << 4 | kCode[n % 12]);
}

uint8_t transposeStep(uint8_t step, int semitones)
{
    if (semitones == 0)
        return step;
    return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(step * std::pow(2.0, semitones / 12.0))), 1, 255));
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

} // namespace

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
    tune->data_ = data;
    tune->splitParts();
    return tune;
}

void Tune::splitParts()
{
    // An FM track is one patch on one chip voice, so every line can be muted,
    // soloed and set apart: a voice plays one note at a time. The tracks go in
    // voice order, V1 first, each numbered by its patch on the voice, V1 1,
    // V1 2, most used first, even when the voice plays one patch. Drum tracks
    // follow, as they were.
    std::vector<TuneTrack> fm, drums;
    for (TuneTrack& track : tracks)
    {
        if (track.kind != TuneTrack::Kind::Fm)
        {
            drums.push_back(std::move(track));
            continue;
        }
        std::array<TuneTrack, 8> byVoice;
        for (const TuneNote& n : track.notes)
            byVoice[static_cast<size_t>(originalChannel(track, n))].notes.push_back(n);
        for (size_t v = 0; v < byVoice.size(); ++v)
            if (!byVoice[v].notes.empty())
            {
                byVoice[v].kind = track.kind;
                byVoice[v].instrument = track.instrument;
                fm.push_back(std::move(byVoice[v]));
            }
    }
    auto voiceOf = [&](const TuneTrack& t) { return originalChannel(t, t.notes.front()); };
    std::stable_sort(fm.begin(), fm.end(), [&](const TuneTrack& a, const TuneTrack& b) {
        return voiceOf(a) != voiceOf(b) ? voiceOf(a) < voiceOf(b) : a.notes.size() > b.notes.size();
    });
    std::array<int, 8> seen {};
    for (TuneTrack& t : fm)
    {
        const auto v = static_cast<size_t>(voiceOf(t));
        t.name = "V" + std::to_string(v + 1) + " " + std::to_string(++seen[v]);
    }
    tracks = std::move(fm);
    for (TuneTrack& t : drums)
        tracks.push_back(std::move(t));
}

int64_t Tune::originalStart(const TuneTrack& track, const TuneNote& note) const
{
    return track.kind == TuneTrack::Kind::Fm ? data_->log.notes[static_cast<size_t>(note.source)].start
                                             : data_->log.hits[static_cast<size_t>(note.source)].start;
}

int64_t Tune::originalLength(const TuneTrack& track, const TuneNote& note) const
{
    if (track.kind == TuneTrack::Kind::Drum)
        return data_->hitLength[static_cast<size_t>(note.source)];
    const LogNote& n = data_->log.notes[static_cast<size_t>(note.source)];
    return std::max<int64_t>(1, n.end - n.start);
}

int Tune::originalChannel(const TuneTrack& track, const TuneNote& note) const
{
    return track.kind == TuneTrack::Kind::Fm ? data_->log.notes[static_cast<size_t>(note.source)].channel
                                             : data_->log.hits[static_cast<size_t>(note.source)].channel;
}

int Tune::pitch(const TuneTrack& track, const TuneNote& note) const
{
    if (track.kind == TuneTrack::Kind::Drum)
        return 60 + note.transpose;
    // Key code and fraction are registers 1 and 2 of the list.
    const auto& regs = data_->fmState[static_cast<size_t>(note.source)].regs;
    const uint8_t kc = regs[1], kf = regs[2];
    return static_cast<int>(std::lround(69.0 + keyCodePitch(kc, kf) - 56.0 + data_->clockShift)) + note.transpose;
}

int Tune::newNoteId() const
{
    int id = 0;
    for (const TuneTrack& t : tracks)
        for (const TuneNote& n : t.notes)
            id = std::max(id, n.id + 1);
    return id;
}

bool Tune::audible(const TuneTrack& track) const
{
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const TuneTrack& t) { return t.solo; });
    return !track.mute && (!anySolo || track.solo); // a mute wins over a solo
}

bool Tune::audible(const TuneTrack& track, const TuneNote& note) const
{
    if (!audible(track) || silentHit(track, note))
        return false;
    return track.kind != TuneTrack::Kind::Fm || ((track.mutedVoices >> originalChannel(track, note)) & 1) == 0;
}

bool Tune::silentHit(const TuneTrack& track, const TuneNote& note) const
{
    if (track.kind != TuneTrack::Kind::Drum || note.source < 0 || static_cast<size_t>(note.source) >= data_->pcmState.size())
        return false;
    const auto& regs = data_->pcmState[static_cast<size_t>(note.source)].regs;
    return regs[0] == 0 && regs[1] == 0; // left and right volume
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
        if (count[static_cast<size_t>(c)] > 0)
            result.push_back(c);
    std::stable_sort(result.begin(), result.end(), [&](int a, int b) {
        return sum[static_cast<size_t>(a)] / count[static_cast<size_t>(a)] > sum[static_cast<size_t>(b)] / count[static_cast<size_t>(b)];
    });
    return result;
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
        // Notes of this chip in order of start, each kept on its channel when
        // free, else moved to the channel free longest.
        struct Ref
        {
            size_t track, note;
        };
        std::vector<Ref> refs;
        for (size_t t = 0; t < tracks.size(); ++t)
            if ((tracks[t].kind == TuneTrack::Kind::Fm) == fm)
                for (size_t n = 0; n < tracks[t].notes.size(); ++n)
                    refs.push_back({ t, n });
        std::stable_sort(refs.begin(), refs.end(), [&](const Ref& a, const Ref& b) {
            return tracks[a.track].notes[a.note].start < tracks[b.track].notes[b.note].start;
        });
        const int channels = fm ? kFmChannels : kPcmChannels;
        std::vector<int64_t> busyUntil(static_cast<size_t>(channels), std::numeric_limits<int64_t>::min());

        for (const Ref& ref : refs)
        {
            const TuneTrack& track = tracks[ref.track];
            const TuneNote& note = track.notes[ref.note];
            const int original = originalChannel(track, note);
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

            const bool muted = mix && (!audible(track, note) || track.volume <= TuneTrack::kSilent);
            const float volume = mix ? std::min(track.volume, TuneTrack::kMaxVolume) : 0.0f;
            const int steps = static_cast<int>(std::lround(-volume / 0.75f));
            const float gain = std::pow(10.0f, volume / 20.0f);
            const bool moved = note.start != originalStart(track, note) || ch != original;
            const int64_t originalLen = originalLength(track, note);
            const int64_t stretch = note.length - originalLen;
            const auto& writes = fm ? data_->log.notes[static_cast<size_t>(note.source)].writes
                                    : data_->log.hits[static_cast<size_t>(note.source)].writes;

            Request request;
            request.channel = ch;
            request.regs.fill(0);
            if (fm)
            {
                request.regs = data_->fmState[static_cast<size_t>(note.source)].regs;
                request.regs[1] = transposeKeyCode(request.regs[1], note.transpose);
                if (muted)
                    request.regs[0] &= 0x3F; // no output, left or right
                if (steps != 0)
                    for (size_t op = 0; op < 4; ++op)
                        if ((kCarriers[request.regs[0] & 7] >> op) & 1)
                            request.regs[5 + 6 * op] = attenuate(request.regs[5 + 6 * op], steps);
            }
            else
            {
                const auto& regs = data_->pcmState[static_cast<size_t>(note.source)].regs;
                std::copy(regs.begin(), regs.end(), request.regs.begin());
                request.regs[5] = transposeStep(request.regs[5], note.transpose);
                if (muted)
                    request.regs[0] = request.regs[1] = 0;
                else if (steps != 0)
                    request.regs[0] = scaleVolume(request.regs[0], gain), request.regs[1] = scaleVolume(request.regs[1], gain);
                request.forceAddress = moved;
            }
            const int requestIndex = static_cast<int>(requests.size());
            requests.push_back(request);

            bool ended = false;
            int connection = fm ? data_->fmState[static_cast<size_t>(note.source)].regs[0] & 7 : 0;
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
                    if (address >= 0x28 && address < 0x30)
                        value = transposeKeyCode(value, note.transpose);
                    if (address >= 0x20 && address < 0x28)
                        connection = value & 7;
                    if (muted && address >= 0x20 && address < 0x28)
                        value &= 0x3F;
                    // The volume lowers the carriers' total level.
                    if (steps != 0 && address >= 0x60 && address < 0x80 && ((kCarriers[connection] >> ((address - 0x60) >> 3)) & 1))
                        value = attenuate(value, steps);
                    ended = ended || isKeyOff(address, value);
                }
                else
                {
                    address = static_cast<uint16_t>((address & 0x87) | (ch << 3));
                    const int reg = address & 0x87;
                    if (reg == 0x07)
                        value = transposeStep(value, note.transpose);
                    if (muted && (reg == 0x02 || reg == 0x03))
                        value = 0;
                    else if (steps != 0 && (reg == 0x02 || reg == 0x03))
                        value = scaleVolume(value, gain);
                }
                const bool start = w.offset == 0 && (fm ? isKeyOn(address, value) : isTrigger(address, value));
                (fm ? ym : pcm).push_back({ note.start + offset, uint64_t { w.order } * 2 + 1, address, value,
                                            start ? requestIndex : -1 });
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
    song->length = src.length;
    song->rom = src.rom;
    song->fmChannelsUsed = src.fmChannelsUsed;
    song->pcmUsed = src.pcmUsed;
    song->beatLength = src.beatLength;
    song->firstBeat = src.firstBeat;
    song->beatCount = src.beatCount;

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
