#include "SongExport.h"

#include "TuneRebuilder.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace opm86
{

namespace
{

constexpr double kNever = std::numeric_limits<double>::infinity();

// Which of a channel's operators (bit k for slot ch + 8k: M1, M2, C1, C2)
// are carriers, heard directly, for each connection.
constexpr uint8_t kCarriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xC, 0xE, 0xE, 0xF };

// The 0.75 dB steps of total level that come nearest a gain; a boost is a
// negative count.
int levelSteps(float gain)
{
    return static_cast<int>(std::lround(-20.0 * std::log10(static_cast<double>(gain)) / 0.75));
}

uint32_t writeTime(double t)
{
    return static_cast<uint32_t>(std::clamp(std::ceil(t - 1e-9), 0.0, 4294967295.0));
}

// A chip's channels' gains, as the player follows the mix: 1 before a
// channel's first change.
struct GainCursors
{
    GainCursors(const ChannelMix* mix, size_t first, size_t count) : mix_(mix), first_(first), gain(count, 1.0f), at_(count, 0) {}

    double next() const
    {
        double t = kNever;
        for (size_t c = 0; mix_ && c < gain.size(); ++c)
            if (const auto& v = mix_->changes[first_ + c]; at_[c] < v.size())
                t = std::min(t, v[at_[c]].time);
        return t;
    }
    // Moves a channel past the changes due by a song time; returns whether it passed any.
    bool advance(size_t c, double now)
    {
        if (!mix_)
            return false;
        const auto& v = mix_->changes[first_ + c];
        bool moved = false;
        for (; at_[c] < v.size() && v[at_[c]].time <= now; ++at_[c])
            gain[c] = v[at_[c]].gain, moved = true;
        return moved;
    }

    const ChannelMix* mix_;
    size_t first_;
    std::vector<float> gain;
    std::vector<size_t> at_;
};

void bakeFm(const VgmSong& song, const ChannelMix* mix, const PatchEdits* edits, VgmSong& out)
{
    std::array<uint8_t, 256> songRegs {}; // what the song last wrote, as the player's shadow
    std::array<uint8_t, 8> keys {};
    std::array<uint8_t, 256> edited {}, chip {}; // the values as edited, and as the chip has them after the mix
    std::array<int, 8> owner {};
    owner.fill(-1);
    std::array<size_t, 8> ownerAt {};
    GainCursors gains(mix, 0, 8);
    uint32_t time = 0;

    // A register's value as the mix has it: a silent channel has no output
    // sides, and a carrier's level moves by its channel's gain, by the
    // connection as edited.
    auto mixed = [&](int reg, uint8_t value) {
        const size_t ch = static_cast<size_t>(reg & 7);
        if (reg >= 0x20 && reg < 0x28 && gains.gain[ch] <= 0.0f)
            return static_cast<uint8_t>(value & 0x3F);
        if (reg >= 0x60 && reg < 0x80 && gains.gain[ch] > 0.0f && ((kCarriers[edited[0x20 + ch] & 7] >> ((reg - 0x60) >> 3)) & 1))
            if (const int steps = levelSteps(gains.gain[ch]); steps != 0)
                return static_cast<uint8_t>((value & 0x80) | std::clamp((value & 0x7F) + steps, 0, 127));
        return value;
    };
    // A channel's output and its operators' levels set again where the mix
    // makes them differ from what the chip has.
    auto relevel = [&](size_t ch) {
        for (const size_t reg : { 0x20 + ch, 0x60 + ch, 0x68 + ch, 0x70 + ch, 0x78 + ch })
            if (const uint8_t v = mixed(static_cast<int>(reg), edited[reg]); v != chip[reg])
            {
                chip[reg] = v;
                out.ym.push_back({ time, static_cast<uint16_t>(reg), v });
            }
    };
    auto emit = [&](uint16_t address, uint8_t value) {
        const int reg = address & 0xFF;
        if (reg != 0x08)
            edited[static_cast<size_t>(reg)] = value;
        const uint8_t v = mixed(reg, value);
        chip[static_cast<size_t>(reg)] = v;
        out.ym.push_back({ time, address, v });
        // A new connection can make other operators the carriers.
        if (reg >= 0x20 && reg < 0x28)
            relevel(static_cast<size_t>(reg & 7));
    };
    auto rewrite = [&](size_t ch) {
        for (const RegWrite& w : channelRewrite(edits, owner, songRegs, keys, static_cast<int>(ch)))
            emit(w.reg, w.value);
    };
    // The owners' and the gains' changes due by a song time, each at its own time.
    auto until = [&](double t) {
        for (;;)
        {
            double next = gains.next();
            if (edits)
                for (size_t ch = 0; ch < 8; ++ch)
                    if (ownerAt[ch] < edits->owner[ch].size())
                        next = std::min(next, edits->owner[ch][ownerAt[ch]].first);
            if (std::isinf(next) || next > t)
                break;
            time = std::max(time, writeTime(next));
            for (size_t ch = 0; edits && ch < 8; ++ch)
                if (advanceOwner(*edits, ch, next, ownerAt[ch], owner[ch]))
                    rewrite(ch);
            for (size_t ch = 0; ch < 8; ++ch)
                if (gains.advance(ch, next))
                    relevel(ch);
        }
    };

    // Playback from the start sets every channel and the chip as the edits
    // have them, before the song's first writes.
    if (edits)
    {
        for (size_t ch = 0; ch < 8; ++ch)
            advanceOwner(*edits, ch, 0.0, ownerAt[ch], owner[ch]);
        for (size_t ch = 0; ch < 8; ++ch)
            rewrite(ch);
        if (edits->chip.any())
            for (const RegWrite& w : chipRewrite(edits, songRegs))
                emit(w.reg, w.value);
    }
    // A write the player meets after a later one plays at that one's time.
    for (size_t i = 0; i < song.ym.size(); ++i)
    {
        const VgmWrite& w = song.ym[i];
        until(std::max<double>(time, w.time));
        time = std::max(time, w.time);
        if (i == song.ymLogged)
            out.ymLogged = out.ym.size();
        shadowWrite(songRegs, keys, w);
        emit(w.address, editedWrite(edits, owner, w.address, w.value));
    }
    if (song.ymLogged >= song.ym.size())
        out.ymLogged = out.ym.size();
    until(kNever);
}

void bakePcm(const VgmSong& song, const ChannelMix* mix, VgmSong& out)
{
    std::array<uint8_t, 256> written {}, chip {};
    GainCursors gains(mix, 8, SegaPcm::kChannels);
    uint32_t time = 0;

    // A volume as the mix has it: 0 on a silent channel, else scaled by the
    // channel's gain, up to the chip's 127.
    auto mixed = [&](int reg, uint8_t value) {
        const int field = reg & 0x87;
        if (field != 0x02 && field != 0x03)
            return value;
        const float gain = gains.gain[static_cast<size_t>((reg >> 3) & 15)];
        if (gain <= 0.0f)
            return uint8_t { 0 };
        if (std::abs(gain - 1.0f) <= 1e-6f)
            return value;
        return static_cast<uint8_t>(std::min(127L, std::lround(static_cast<float>(value & 0x7F) * gain)));
    };
    auto relevel = [&](size_t ch) {
        for (const size_t reg : { ch * 8 + 2, ch * 8 + 3 })
            if (const uint8_t v = mixed(static_cast<int>(reg), written[reg]); v != chip[reg])
            {
                chip[reg] = v;
                out.pcm.push_back({ time, static_cast<uint16_t>(reg), v });
            }
    };
    auto until = [&](double t) {
        for (;;)
        {
            const double next = gains.next();
            if (std::isinf(next) || next > t)
                break;
            time = std::max(time, writeTime(next));
            for (size_t ch = 0; ch < SegaPcm::kChannels; ++ch)
                if (gains.advance(ch, next))
                    relevel(ch);
        }
    };
    for (size_t i = 0; i < song.pcm.size(); ++i)
    {
        const VgmWrite& w = song.pcm[i];
        until(std::max<double>(time, w.time));
        time = std::max(time, w.time);
        if (i == song.pcmLogged)
            out.pcmLogged = out.pcm.size();
        if (w.address & 0x3000)
        {
            out.pcm.push_back({ time, w.address, w.value }); // a stop, which the volumes do not touch
            continue;
        }
        const size_t reg = w.address & 0xFF;
        written[reg] = w.value;
        chip[reg] = mixed(static_cast<int>(reg), w.value);
        out.pcm.push_back({ time, w.address, chip[reg] });
    }
    if (song.pcmLogged >= song.pcm.size())
        out.pcmLogged = out.pcm.size();
    until(kNever);
}

void put16(std::vector<uint8_t>& d, uint16_t v)
{
    d.push_back(static_cast<uint8_t>(v));
    d.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& d, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        d.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void set32(std::vector<uint8_t>& d, size_t at, uint32_t v)
{
    for (size_t i = 0; i < 4; ++i)
        d[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// A string in UTF-16, as a GD3 tag holds it, from UTF-8, ending in a zero.
void putUtf16(std::vector<uint8_t>& d, const std::string& text)
{
    for (size_t i = 0; i < text.size();)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        uint32_t code = extra == 0 ? c : c & (0x3Fu >> extra);
        for (int k = 1; k <= extra && i + static_cast<size_t>(k) < text.size(); ++k)
            code = code << 6 | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3Fu);
        i += static_cast<size_t>(extra) + 1;
        if (code >= 0x10000)
        {
            code -= 0x10000;
            put16(d, static_cast<uint16_t>(0xD800 | (code >> 10)));
            put16(d, static_cast<uint16_t>(0xDC00 | (code & 0x3FF)));
        }
        else
            put16(d, static_cast<uint16_t>(code));
    }
    put16(d, 0);
}

std::vector<uint8_t> gzipped(const std::vector<uint8_t>& data)
{
    z_stream z {};
    if (deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    std::vector<uint8_t> out(deflateBound(&z, static_cast<uLong>(data.size())));
    z.next_in = const_cast<Bytef*>(data.data());
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return result == Z_STREAM_END ? out : std::vector<uint8_t> {};
}

} // namespace

std::shared_ptr<VgmSong> bakeSong(const VgmSong& song, const ChannelMix* mix, const PatchEdits* edits)
{
    auto out = std::make_shared<VgmSong>();
    out->name = song.name;
    out->header = song.header;
    out->fmRate = song.fmRate;
    out->pcmRate = song.pcmRate;
    out->length = song.length;
    out->rom = song.rom;
    out->fmChannelsUsed = song.fmChannelsUsed;
    out->pcmUsed = song.pcmUsed;
    out->beatLength = song.beatLength;
    out->firstBeat = song.firstBeat;
    out->beatCount = song.beatCount;
    out->ym.reserve(song.ym.size());
    out->pcm.reserve(song.pcm.size());
    bakeFm(song, mix, edits, *out);
    bakePcm(song, mix, *out);
    return out;
}

std::shared_ptr<VgmSong> bakedSong(const Tune& tune)
{
    BuildReport report;
    const auto song = tune.build(&report, false);
    const auto mix = mixOf(tune, report);
    const auto edits = patchEditsOf(tune, report);
    return bakeSong(*song, mix.get(), edits.get());
}

std::vector<uint8_t> writeVgm(const VgmSong& song, const std::string& title, bool gzip)
{
    std::vector<uint8_t> d(0x80, 0);
    d[0] = 'V', d[1] = 'g', d[2] = 'm', d[3] = ' ';
    set32(d, 0x08, 0x151);
    const size_t ymCount = std::min(song.ymLogged, song.ym.size());
    const size_t pcmCount = std::min(song.pcmLogged, song.pcm.size());
    // The clocks the song plays at: the rip's, or those of its rates.
    const bool romUsed = std::any_of(song.rom.begin(), song.rom.end(), [](uint8_t b) { return b != 0x80; });
    const uint32_t ymClock = song.header.ym2151Clock ? song.header.ym2151Clock
                             : ymCount > 0          ? static_cast<uint32_t>(std::lround(song.fmRate * 64.0))
                                                    : 0;
    const uint32_t pcmClock = song.header.segaPcmClock ? song.header.segaPcmClock
                              : pcmCount > 0 || romUsed ? static_cast<uint32_t>(std::lround(song.pcmRate * 128.0))
                                                        : 0;
    set32(d, 0x30, ymClock);
    set32(d, 0x34, 0x80 - 0x34);
    set32(d, 0x38, pcmClock);
    set32(d, 0x3C, song.header.segaPcmInterface);

    // The sample memory, in blocks over the bytes in use; the rest reads as
    // silence (0x80), as it does in the song.
    for (size_t i = 0; romUsed && i < song.rom.size();)
    {
        if (song.rom[i] == 0x80)
        {
            ++i;
            continue;
        }
        size_t end = i + 1, last = i;
        for (; end < song.rom.size() && end - last <= 64; ++end)
            if (song.rom[end] != 0x80)
                last = end;
        end = last + 1;
        d.insert(d.end(), { 0x67, 0x66, 0x80 });
        put32(d, static_cast<uint32_t>(8 + end - i));
        put32(d, static_cast<uint32_t>(song.rom.size()));
        put32(d, static_cast<uint32_t>(i));
        d.insert(d.end(), song.rom.begin() + static_cast<long>(i), song.rom.begin() + static_cast<long>(end));
        i = end;
    }

    // The writes of both chips in order of time; a write after a later one
    // plays at that one's time, as the player has it.
    uint32_t now = 0;
    auto waitUntil = [&](uint32_t t) {
        for (uint32_t gap = t > now ? t - now : 0; gap > 0;)
        {
            if (gap <= 16)
            {
                d.push_back(static_cast<uint8_t>(0x70 + gap - 1));
                gap = 0;
            }
            else
            {
                const uint32_t n = std::min<uint32_t>(gap, 65535);
                d.push_back(0x61);
                put16(d, static_cast<uint16_t>(n));
                gap -= n;
            }
        }
        now = std::max(now, t);
    };
    // The PCM flags as written, for a stop: the chip itself changes only
    // their bit 0, which a stop sets.
    std::array<uint8_t, 256> flags {};
    for (int ch = 0; ch < SegaPcm::kChannels; ++ch)
        flags[static_cast<size_t>(ch * 8 + 0x86)] = 0x01;
    uint32_t ymTime = 0, pcmTime = 0;
    for (size_t i = 0, j = 0; i < ymCount || j < pcmCount;)
    {
        const uint32_t ty = i < ymCount ? std::max(ymTime, song.ym[i].time) : UINT32_MAX;
        const uint32_t tp = j < pcmCount ? std::max(pcmTime, song.pcm[j].time) : UINT32_MAX;
        if (ty <= tp)
        {
            ymTime = ty;
            waitUntil(ty);
            d.insert(d.end(), { 0x54, static_cast<uint8_t>(song.ym[i].address), song.ym[i].value });
            ++i;
            continue;
        }
        pcmTime = tp;
        waitUntil(tp);
        const VgmWrite& w = song.pcm[j++];
        const size_t reg = w.address & 0xFF;
        uint8_t value = w.value;
        if (w.address & 0x2000)
            value = static_cast<uint8_t>(flags[reg] | 0x01); // any sample stops
        else if (w.address & 0x1000)
        {
            if (flags[reg] & 0x02)
                continue; // a one-shot sample plays out
            value = static_cast<uint8_t>(flags[reg] | 0x01);
        }
        if ((reg & 0x87) == 0x86)
            flags[reg] = value;
        d.insert(d.end(), { 0xC0, static_cast<uint8_t>(reg), 0x00, value });
    }
    waitUntil(song.length);
    d.push_back(0x66);
    set32(d, 0x18, now);

    // The GD3 tag: the title, and every other field empty.
    set32(d, 0x14, static_cast<uint32_t>(d.size() - 0x14));
    std::vector<uint8_t> strings;
    putUtf16(strings, title);
    for (int i = 0; i < 10; ++i)
        put16(strings, 0);
    d.insert(d.end(), { 'G', 'd', '3', ' ' });
    put32(d, 0x100);
    put32(d, static_cast<uint32_t>(strings.size()));
    d.insert(d.end(), strings.begin(), strings.end());
    set32(d, 0x04, static_cast<uint32_t>(d.size() - 4));
    return gzip ? gzipped(d) : d;
}

std::vector<uint8_t> wavHeader(uint32_t rate, int bits, uint32_t frames)
{
    const uint32_t frameBytes = static_cast<uint32_t>(2 * bits / 8);
    const uint32_t data = frames * frameBytes;
    std::vector<uint8_t> h;
    h.insert(h.end(), { 'R', 'I', 'F', 'F' });
    put32(h, 36 + data);
    h.insert(h.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    put32(h, 16);
    put16(h, 1); // PCM
    put16(h, 2); // stereo
    put32(h, rate);
    put32(h, rate * frameBytes);
    put16(h, static_cast<uint16_t>(frameBytes));
    put16(h, static_cast<uint16_t>(bits));
    h.insert(h.end(), { 'd', 'a', 't', 'a' });
    put32(h, data);
    return h;
}

WavWriter::~WavWriter()
{
    close();
}

bool WavWriter::open(const std::string& path, uint32_t rate, int bits)
{
    close();
    rate_ = rate;
    bits_ = bits == 24 ? 24 : 16;
    frames_ = 0;
    failed_ = false;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_)
        return false;
    const auto header = wavHeader(rate_, bits_, 0);
    failed_ = std::fwrite(header.data(), 1, header.size(), file_) != header.size();
    return !failed_;
}

void WavWriter::write(const float* left, const float* right, int count)
{
    if (!file_ || count <= 0)
        return;
    buffer_.clear();
    const double full = bits_ == 24 ? 8388607.0 : 32767.0;
    for (int i = 0; i < count; ++i)
        for (const float x : { left[i], right[i] })
        {
            const auto v = static_cast<int32_t>(std::lround(std::clamp(static_cast<double>(x), -1.0, 1.0) * full));
            buffer_.push_back(static_cast<uint8_t>(v));
            buffer_.push_back(static_cast<uint8_t>(v >> 8));
            if (bits_ == 24)
                buffer_.push_back(static_cast<uint8_t>(v >> 16));
        }
    failed_ = failed_ || std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size();
    frames_ += static_cast<uint32_t>(count);
}

bool WavWriter::close()
{
    if (!file_)
        return !failed_;
    const auto header = wavHeader(rate_, bits_, frames_);
    failed_ = failed_ || std::fseek(file_, 0, SEEK_SET) != 0 || std::fwrite(header.data(), 1, header.size(), file_) != header.size();
    failed_ = std::fclose(file_) != 0 || failed_;
    file_ = nullptr;
    return !failed_;
}

TuneRenderer::TuneRenderer(const Tune& tune, double rate, bool loopOnly)
    : tune_(tune), rate_(rate), loop_(loopOnly && tune.loop.active())
{
}

TuneRenderer::~TuneRenderer() = default;

bool TuneRenderer::prepare(const std::function<bool()>& keepGoing)
{
    BuildReport report;
    auto song = tune_.build(&report, false);
    mix_ = mixOf(tune_, report);
    edits_ = patchEditsOf(tune_, report);
    const double start = loop_ ? static_cast<double>(tune_.loop.start) : 0.0;
    const double end = loop_ ? static_cast<double>(tune_.loop.end) : songEnd(*song);
    // The chips' state up to the start, ten seconds at a time.
    auto snapshots = std::make_shared<SongSnapshots>();
    for (double reached = 0.0;;)
    {
        if (!keepGoing())
            return false;
        reached = std::min(start + kVgmRate, reached + 10.0 * kVgmRate);
        extendSnapshots(*song, *snapshots, reached);
        if (reached >= start + kVgmRate)
            break;
    }
    total_ = static_cast<int64_t>(std::ceil((end - start) * rate_ / kVgmRate));
    songEnd_ = loop_ ? total_ : static_cast<int64_t>(std::ceil((song->length - start) * rate_ / kVgmRate));
    song_ = song;
    player_ = std::make_unique<SongPlayer>();
    player_->prepare(rate_);
    player_->setSong(song_, snapshots);
    player_->setMix(mix_);
    player_->setPatchEdits(edits_);
    started_ = player_->play(start);
    finished_ = !started_;
    return true;
}

int TuneRenderer::render(float* left, float* right, int count)
{
    if (!started_ || finished_)
        return 0;
    const int n = static_cast<int>(std::min<int64_t>(count, total_ - done_));
    if (n <= 0)
    {
        finished_ = true;
        return 0;
    }
    player_->render(left, right, n);
    // Past the song's end, the releases ring on until a quarter second of silence.
    constexpr float kSilent = 1e-5f; // below a 16-bit step, and 0.2 of a 24-bit one
    const int64_t enough = static_cast<int64_t>(0.25 * rate_);
    for (int i = 0; i < n; ++i)
    {
        quiet_ = std::abs(left[i]) < kSilent && std::abs(right[i]) < kSilent ? quiet_ + 1 : 0;
        if (!loop_ && done_ + i >= songEnd_ && quiet_ >= enough)
        {
            finished_ = true;
            done_ += i + 1;
            return i + 1;
        }
    }
    done_ += n;
    return n;
}

double TuneRenderer::progress() const
{
    if (finished_)
        return 1.0;
    return total_ > 0 ? static_cast<double>(done_) / static_cast<double>(total_) : 0.0;
}

Arrangement arrangementFromTune(const Tune& tune, const std::string& title)
{
    Arrangement a;
    a.name = title;
    a.patches = tune.patches();
    a.samples = tune.samples();
    a.beatLength = tune.beatLength();
    a.firstBeat = tune.firstBeat();
    a.beatsPerBar = tune.beatsPerBar();
    a.length = tune.length();
    const VgmImport& rip = tune.imported();
    for (const TuneTrack& track : tune.tracks)
    {
        if (!tune.audible(track))
            continue;
        const bool fm = track.kind == TuneTrack::Kind::Fm;
        ArrTrack t;
        t.kind = fm ? ArrTrack::Kind::Fm : ArrTrack::Kind::Drum;
        t.source = std::max(0, track.instrument);
        t.name = track.name;
        for (const TuneNote& note : track.notes)
        {
            if (!tune.audible(track, note))
                continue;
            const auto source = static_cast<size_t>(std::max(0, note.source));
            ArrNote n;
            n.start = note.start;
            n.length = note.length;
            n.bend.clear();
            n.changes.clear();
            // The note's expression: its velocity lowers every level, its
            // fine tune bends it, and its pan sets the output.
            const double fine = std::clamp(note.fine, -TuneNote::kMaxFine, TuneNote::kMaxFine) / 64.0;
            const int quieter = -std::clamp(note.velocity, TuneNote::kMinVelocity, 0);
            const auto pan = [&note](FmPan was) {
                return !note.pan ? was : *note.pan < 0 ? FmPan::Left : *note.pan > 0 ? FmPan::Right : FmPan::Both;
            };
            if (fm && !note.written() && source < rip.notes.size())
            {
                // The game's slides and level changes, at the note's pitch.
                const VgmNote& vn = rip.notes[source];
                const int key = static_cast<int>(std::lround(vn.pitch.front().second));
                n.pitch = std::clamp(key + note.transpose, 0, 127);
                n.voice = tune.originalChannel(track, note);
                for (const auto& [time, pitch] : vn.pitch)
                    if (const int64_t offset = static_cast<int64_t>(time) - vn.start; offset < n.length || n.bend.empty())
                        n.bend.emplace_back(offset, pitch - key + fine);
                for (const VgmNoteState& s : vn.state)
                    n.changes.push_back({ static_cast<int64_t>(s.time) - vn.start, s.patch, std::min(127, s.attenuation + quieter), pan(s.pan) });
            }
            else if (fm)
            {
                n.pitch = std::clamp(tune.pitch(track, note), 0, 127);
                n.voice = tune.originalChannel(track, note);
                n.bend = { { 0, tune.exactPitch(track, note) - n.pitch } };
                n.changes = { NoteChange { 0, t.source, quieter, pan(FmPan::Both) } };
            }
            else if (!note.written() && source < rip.hits.size())
            {
                // The game's volumes and step, with the track's edit over them
                // as the build has it: later values keep their shape around the
                // edited ones at the trigger.
                const VgmHit& vh = rip.hits[source];
                const PadSample* pad = tune.sample(track);
                const SampleEdit edit = pad != nullptr ? track.sample.fitted(*pad) : track.sample;
                const VgmHitState first = vh.state.empty() ? VgmHitState {} : vh.state.front();
                auto level = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 127)); };
                n.pitch = std::clamp(60 + note.transpose, 0, 127);
                n.fine = fine;
                for (const VgmHitState& s : vh.state)
                    if (const int64_t offset = static_cast<int64_t>(s.time) - vh.start; offset < n.length || n.hit.empty())
                    {
                        const auto volumes = noteVolumes(edit.left ? scaleTo(level(s.left), level(first.left), *edit.left, 127) : level(s.left),
                                                         edit.right ? scaleTo(level(s.right), level(first.right), *edit.right, 127) : level(s.right), note);
                        n.hit.push_back({ offset, volumes[0], volumes[1], edit.step ? scaleTo(s.step, first.step, *edit.step, 255) : s.step });
                    }
            }
            else if (!fm && note.written())
            {
                // A written hit: its sample's volumes and step, as the build triggers it.
                const PcmVoice v = tune.hitVoice(track, note);
                const auto volumes = noteVolumes(v.regs[0], v.regs[1], note);
                n.pitch = std::clamp(60 + note.transpose, 0, 127);
                n.fine = fine;
                n.hit.push_back({ 0, volumes[0], volumes[1], v.regs[5] });
            }
            else
                continue;
            t.notes.push_back(std::move(n));
        }
        if (!t.notes.empty())
        {
            std::stable_sort(t.notes.begin(), t.notes.end(), [](const ArrNote& x, const ArrNote& y) { return x.start < y.start; });
            a.tracks.push_back(std::move(t));
        }
    }
    return a;
}

MidiExport exportMidi(const Tune& tune, const std::string& title)
{
    return exportMidi(arrangementFromTune(tune, title));
}

} // namespace opm86
