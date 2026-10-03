#include "SongTrack.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{

constexpr int kTicksPerQuarter = 960;
constexpr int kLeadInBeats = 4;

void appendLength(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t bytes[5];
    int n = 0;
    do
    {
        bytes[n++] = value & 0x7F;
        value >>= 7;
    } while (value != 0);
    while (n > 0)
    {
        --n;
        out.push_back(static_cast<uint8_t>(bytes[n] | (n > 0 ? 0x80 : 0)));
    }
}

void appendBig(std::vector<uint8_t>& out, uint32_t value, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void appendTrack(std::vector<uint8_t>& out, const std::vector<uint8_t>& body)
{
    out.insert(out.end(), { 'M', 'T', 'r', 'k' });
    appendBig(out, static_cast<uint32_t>(body.size() + 4), 4);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), { 0x00, 0xFF, 0x2F, 0x00 });
}

void appendName(std::vector<uint8_t>& body, const std::string& name)
{
    body.insert(body.end(), { 0x00, 0xFF, 0x03 });
    appendLength(body, static_cast<uint32_t>(name.size()));
    body.insert(body.end(), name.begin(), name.end());
}

} // namespace

void beatToNote(int beat, uint8_t& note, uint8_t& velocity)
{
    const int value = std::max(0, beat + 1);
    note = static_cast<uint8_t>(value % 128);
    velocity = static_cast<uint8_t>(std::min(127, 1 + value / 128));
}

int beatFromNote(int note, int velocity)
{
    return note + 128 * (std::max(velocity, 1) - 1) - 1;
}

void stemChannels(int stem, uint8_t& fm, uint16_t& pcm)
{
    if (stem >= 1 && stem <= 8)
        fm = static_cast<uint8_t>(1 << (stem - 1)), pcm = 0;
    else if (stem == kStemDrums)
        fm = 0, pcm = 0xFFFF;
    else
        fm = 0xFF, pcm = 0xFFFF;
}

std::string stemName(int stem)
{
    if (stem >= 1 && stem <= 8)
        return "FM " + std::to_string(stem);
    return stem == kStemDrums ? "Drums" : "Whole song";
}

SongTrackFile writeSongTrack(const VgmSong& song, bool stems)
{
    SongTrackFile result;
    if (stems)
    {
        for (int ch = 0; ch < 8; ++ch)
            if ((song.fmChannelsUsed >> ch) & 1)
                result.stems.push_back(ch + 1);
        if (song.pcmUsed)
            result.stems.push_back(kStemDrums);
    }
    if (result.stems.empty())
        result.stems.push_back(0);

    const auto microsPerBeat = static_cast<uint32_t>(std::lround(song.beatLength / kVgmRate * 1e6));
    result.tempo = 60e6 / microsPerBeat;

    std::vector<uint8_t>& out = result.file;
    out = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1 };
    appendBig(out, static_cast<uint32_t>(result.stems.size() + 1), 2);
    appendBig(out, kTicksPerQuarter, 2);

    std::vector<uint8_t> tempo;
    appendName(tempo, song.name);
    tempo.insert(tempo.end(), { 0x00, 0xFF, 0x51, 0x03, static_cast<uint8_t>(microsPerBeat >> 16),
                                static_cast<uint8_t>(microsPerBeat >> 8), static_cast<uint8_t>(microsPerBeat) });
    tempo.insert(tempo.end(), { 0x00, 0xFF, 0x58, 0x04, 4, 2, 24, 8 }); // 4/4
    appendTrack(out, tempo);

    const uint32_t id = songId(song.name);
    for (int stem : result.stems)
    {
        std::vector<uint8_t> body;
        appendName(body, song.name + " - " + stemName(stem));
        body.insert(body.end(), { 0x00, 0xB0, kStemController, static_cast<uint8_t>(stem) });
        for (int part = 0; part < 4; ++part)
            body.insert(body.end(), { 0x00, 0xB0, static_cast<uint8_t>(kSongIdController + part),
                                      static_cast<uint8_t>((id >> (21 - 7 * part)) & 0x7F) });

        // Beat -1 sits in the last beat of the lead-in bar; each note lasts
        // one beat and ends as the next one starts.
        uint32_t last = 0;
        for (int beat = -1; beat < song.beatCount; ++beat)
        {
            const auto tick = static_cast<uint32_t>((beat + kLeadInBeats) * kTicksPerQuarter);
            uint8_t note = 0, velocity = 1;
            beatToNote(beat, note, velocity);
            appendLength(body, tick - last);
            body.insert(body.end(), { 0x90, note, velocity });
            appendLength(body, kTicksPerQuarter);
            body.insert(body.end(), { 0x80, note, 0 });
            last = tick + kTicksPerQuarter;
        }
        appendTrack(out, body);
    }
    return result;
}

void SongTrackPlayer::setStem(int stem)
{
    stem_ = stem;
    uint8_t fm = 0;
    uint16_t pcm = 0;
    stemChannels(stem, fm, pcm);
    player_.setChannels(fm, pcm);
}

void SongTrackPlayer::handleMidi(const uint8_t* data, int size)
{
    if (size < 3)
        return;
    const int status = data[0] & 0xF0;
    const int d1 = data[1] & 0x7F, d2 = data[2] & 0x7F;

    if (status == 0xB0)
    {
        if (d1 == kStemController)
            setStem(d2);
        else if (d1 >= kSongIdController && d1 < kSongIdController + 4)
        {
            const int part = d1 - kSongIdController;
            const int shift = 21 - 7 * part;
            requestedId_ = (requestedId_ & ~(0x7Fu << shift)) | static_cast<uint32_t>(d2) << shift;
            idParts_ |= 1 << part;
        }
        return;
    }

    if (status == 0x90 && d2 > 0)
    {
        ++held_;
        const VgmSong* song = player_.song();
        if (song == nullptr)
            return;
        const double target = song->beatTime(beatFromNote(d1, d2));
        // The next beat of the playing song continues; any other jumps.
        if (player_.playing() && std::abs(player_.position() - target) < song->beatLength * 0.25)
            return;
        player_.play(target);
        return;
    }

    if (status == 0x80 || (status == 0x90 && d2 == 0))
        held_ = std::max(0, held_ - 1);
}

void SongTrackPlayer::endOfEvents()
{
    if (held_ == 0 && player_.playing())
        player_.stop();
}

} // namespace opm86
