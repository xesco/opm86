// Checks of the chip emulation and the VGM importer that need no DAW.

#include "SegaPcm.h"
#include "SongLog.h"
#include "SongPlayer.h"
#include "Tune.h"
#include "TuneRebuilder.h"
#include "VgmImport.h"

#include "TestVgm.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <thread>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace opm86;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        ++failures;
}

void testVgmImport()
{
    const VgmImport r = importVgm(test::makeVgm(), "Test");
    check(r.error.empty(), "VGM parses without error");
    check(r.ym2151Clock == 4000000 && r.segaPcmClock == 4000000, "VGM header clocks are read");
    check(r.patches.size() == 2, "two distinct FM patches are found");
    if (!r.patches.empty())
    {
        const FmPatch& p = r.patches[0];
        check(p.alg == 4 && p.fb == 5 && p.op[2].mul == 3, "patch registers are decoded in signal order");
        check(p.op[1].tl == 10 && p.op[3].tl == 10, "repeated key ons keep the loudest carrier level");
    }
    check(r.samples.size() == 1, "one PCM sample is found");
    if (!r.samples.empty())
    {
        const PadSample& s = r.samples[0];
        bool ramp = s.data.size() == 512;
        for (size_t i = 0; ramp && i < s.data.size(); ++i)
            ramp = s.data[i] == (i & 0xFF);
        check(ramp, "the sample is cut from the right bank and length");
        check(s.step == 0x40 && !s.loop, "the sample's step and loop flag are kept");
    }
}

void testSegaPcm()
{
    std::vector<uint8_t> rom(SegaPcm::kRomSize, 0x80);
    rom[0x20000 + 0x100] = 0x90; // bank 2, offset 0x100: +16
    rom[0x20000 + 0x101] = 0x70; // -16

    SegaPcm chip;
    chip.reset();
    chip.setRom(rom.data(), rom.size(), 0);
    chip.write(0x86, 0x23);
    chip.write(0x02, 10);
    chip.write(0x03, 20);
    chip.write(0x06, 0x01);
    chip.write(0x07, 0xFF);
    chip.write(0x84, 0x00);
    chip.write(0x85, 0x01);
    chip.write(0x86, 0x22);

    int32_t l = 0, r = 0;
    chip.generate(l, r);
    check(l == 16 * 10 && r == 16 * 20, "first sample uses bank, address and both volumes");
    chip.generate(l, r);
    const int32_t second = l;
    chip.generate(l, r);
    check(second == 16 * 10 && l == -16 * 10, "a step of 0xFF crosses to the next byte on the third sample");

    // 256 bytes at 255/256 of a byte per sample: the 259th sample finds the end.
    int samples = 3;
    while (!(chip.read(0x86) & 1) && samples < 1000)
        chip.generate(l, r), ++samples;
    check(samples == 259, "a one-shot sample stops at the end of its last page");

    // Boards wire the bank bits differently; the VGM header's interface value says how.
    check(SegaPcm::bankOffset(0x23, 0) == 0x20000 && SegaPcm::bankOffset(0x23, 0x0C) == 0x20000, "Out Run's banks are 64 KB, from bits 4 to 6");
    check(SegaPcm::bankOffset(0x2B, 0xF8000D) == 0x50000, "a wider mask and a shift of 13 reach past 512 KB");
}

} // namespace

bool sameWrites(const std::vector<VgmWrite>& a, const std::vector<VgmWrite>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].time != b[i].time || a[i].address != b[i].address || a[i].value != b[i].value)
            return false;
    return true;
}

void testSongLog()
{
    std::string error;
    const auto song = loadVgmSong(test::makeSong(), "Song", error);
    const SongLog log = splitLog(*song);
    check(log.notes.size() == 12 && log.hits.size() == 12, "the song splits into its 12 notes and 12 drum hits");

    bool keyOns = true;
    for (const LogNote& n : log.notes)
        keyOns = keyOns && std::any_of(n.writes.begin(), n.writes.end(), [](const LogWrite& w) {
                     return w.offset == 0 && w.address == 0x08 && (w.value & 0x78) != 0;
                 });
    bool triggers = true;
    for (const LogHit& h : log.hits)
        triggers = triggers && std::any_of(h.writes.begin(), h.writes.end(), [](const LogWrite& w) {
                       return w.offset == 0 && (w.address & 0x87) == 0x86 && !(w.value & 1);
                   });
    check(keyOns && triggers, "every note owns its key on and every hit its trigger");

    const auto rebuilt = rebuildSong(*song, log);
    check(sameWrites(rebuilt->ym, song->ym) && sameWrites(rebuilt->pcm, song->pcm),
          "rebuilding the split log gives back every write in the original order");
}

// Every key on in a song with its time, channel and the channel's registers.
struct KeyOn
{
    uint32_t time;
    int channel;
    std::array<uint8_t, 256> regs;
};

std::vector<KeyOn> keyOns(const VgmSong& song)
{
    std::vector<KeyOn> out;
    std::array<uint8_t, 256> regs {};
    for (const VgmWrite& w : song.ym)
    {
        if (w.address == 0x08 && (w.value & 0x78) != 0)
            out.push_back({ w.time, w.value & 7, regs });
        else if (w.address != 0x08 && w.address < 256)
            regs[w.address] = w.value;
    }
    return out;
}

// The 28 registers a note uses on its channel, compared between two key ons.
bool sameChannelState(const KeyOn& a, const KeyOn& b)
{
    static constexpr int kRegs[10] = { 0x20, 0x28, 0x30, 0x38, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0 };
    for (int base : kRegs)
        for (int op = 0; op < (base < 0x40 ? 1 : 4); ++op)
            if (a.regs[static_cast<size_t>(base + a.channel + 8 * op)] != b.regs[static_cast<size_t>(base + b.channel + 8 * op)])
                return false;
    return true;
}

void testTune()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    check(tune && tune->tracks.size() == 4, "the tune has two instrument tracks and two drum tracks");
    if (!tune)
        return;
    const VgmSong& original = tune->source();
    check(sameWrites(tune->build()->ym, original.ym) && sameWrites(tune->build()->pcm, original.pcm),
          "an unedited tune builds the original log");
    const auto before = keyOns(original);

    // The lead's fourth note bends half a semitone 4410 samples after its
    // key on, then moves two semitones up from its start 4410 samples later.
    TuneTrack& lead = tune->tracks[0];
    const TuneNote& slide = lead.notes[3];
    const auto bends = tune->bends(lead, slide);
    check(bends.size() == 3 && std::lround(bends[0].second) == tune->pitch(lead, slide)
              && bends[1].first == slide.start + 4410 && std::abs(bends[1].second - bends[0].second - 0.5) < 1e-9
              && bends[2].first == slide.start + 8820 && std::abs(bends[2].second - bends[0].second - 2.0) < 1e-9,
          "a note's bends follow each pitch change while held");
    check(tune->bends(lead, lead.notes[0]).size() == 1, "a note held at one pitch has one bend");

    // The lead's second note played alone: only its channel sounds, from its
    // key on to its end, with no release.
    {
        BuildReport report;
        tune->build(&report);
        const TuneNote& chosen = lead.notes[1];
        const NoteMix alone = noteMixOf(*tune, report, { { 0, chosen.id } });
        const auto ch = static_cast<size_t>(report.channels[0][1]);
        bool othersSilent = true, chosenHeard = false;
        for (size_t c = 0; c < alone.mix->changes.size(); ++c)
            for (const auto& change : alone.mix->changes[c])
            {
                const bool atChosen = c == ch && change.time == static_cast<double>(chosen.start) && change.gain > 0.0f;
                chosenHeard |= atChosen && change.gain == 1.0f;
                othersSilent &= atChosen || change.gain == 0.0f;
            }
        const double end = static_cast<double>(chosen.start + chosen.length);
        const auto& v = alone.mix->changes[ch];
        const bool cut = std::any_of(v.begin(), v.end(), [&](const ChannelMix::Change& c) { return c.time == end && c.gain == 0.0f; });
        check(chosenHeard && othersSilent && cut && alone.from == static_cast<double>(chosen.start) && alone.to == end,
              "a note played alone sounds on its channel only, and stops at its end");
    }

    // Transpose the lead's first note up two semitones.
    const int pitch = tune->pitch(lead, lead.notes[0]);
    lead.notes[0].transpose = 2;
    check(tune->pitch(lead, lead.notes[0]) == pitch + 2, "a transposed note reports its new pitch");
    auto after = keyOns(*tune->build());
    const KeyOn& first = after[0];
    check(first.regs[0x28 + static_cast<size_t>(first.channel)] == 0x4D, "the transposed note keys on two semitones up (A4 to B4)");
    lead.notes[0].transpose = 0;

    // Move the lead's third note 3000 samples later: it keys on there, and
    // every other note keys on with the registers it had in the game.
    lead.notes[2].start += 3000;
    after = keyOns(*tune->build());
    bool moved = false, others = after.size() == before.size();
    for (const KeyOn& k : after)
        moved = moved || k.time == before[4].time + 3000;
    for (size_t i = 0; others && i < before.size(); ++i)
        if (before[i].time != before[4].time)
        {
            // A note may have moved to a free channel: match it by time and registers.
            const auto match = std::find_if(after.begin(), after.end(), [&](const KeyOn& k) {
                return k.time == before[i].time && sameChannelState(k, before[i]);
            });
            others = match != after.end();
            if (!others)
                std::printf("      key on %zu at %u on channel %d differs\n", i, before[i].time, before[i].channel);
        }
    check(moved, "a moved note keys on at its new time");
    check(others, "every other note keys on with the registers it had in the game, on whatever channel");
    lead.notes[2].start -= 3000;

    // Make two lead notes overlap: the second moves to a free channel.
    lead.notes[1].start = lead.notes[0].start + 100;
    BuildReport report;
    tune->build(&report);
    check(report.channels[0][0] != report.channels[0][1] && report.conflicts == 0,
          "a note made to overlap another moves to a free channel");

    // Stack all 12 FM notes at one time: 8 channels play, 4 notes conflict.
    for (TuneTrack& t : tune->tracks)
        if (t.kind == TuneTrack::Kind::Fm)
            for (TuneNote& n : t.notes)
                n.start = 1000;
    tune->build(&report);
    check(report.conflicts == 4, "beyond the 8 FM channels, notes are reported as conflicts");

    // Mute everything: the song is silent.
    auto silent = Tune::load(test::makeSong(), "Song", error);
    for (TuneTrack& t : silent->tracks)
        t.mute = true;
    const auto built = silent->build();
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(built, makeStartSnapshot(*built));
    player.play(0.0);
    std::vector<float> l(144000), r(144000);
    player.render(l.data(), r.data(), 144000);
    float peak = 0;
    for (size_t i = 0; i < l.size(); ++i)
        peak = std::max({ peak, std::abs(l[i]), std::abs(r[i]) });
    check(peak < 1e-6f, "muting every track silences the song");

    // Muting a track's only voice is muting the track; other tracks keep playing.
    auto byTrack = Tune::load(test::makeSong(), "Song", error);
    auto byVoice = Tune::load(test::makeSong(), "Song", error);
    byTrack->tracks[0].mute = true;
    const auto voices = byVoice->voices(byVoice->tracks[0]);
    check(voices.size() == 1, "a one-note-at-a-time part has one voice");
    byVoice->tracks[0].mutedVoices = static_cast<uint16_t>(1 << voices[0]);
    const auto a = byTrack->build(), b = byVoice->build();
    bool same = a->ym.size() == b->ym.size();
    for (size_t i = 0; same && i < a->ym.size(); ++i)
        same = a->ym[i].time == b->ym[i].time && a->ym[i].address == b->ym[i].address && a->ym[i].value == b->ym[i].value;
    check(same, "muting a track's only voice builds the same song as muting the track");
    check(byVoice->audible(byVoice->tracks[1], byVoice->tracks[1].notes[0]), "a voice mute leaves other tracks audible");

    // Shorten a drum: a stop marker ends it.
    auto drums = Tune::load(test::makeSong(), "Song", error);
    TuneTrack& kick = drums->tracks[2];
    kick.notes[0].length /= 2;
    const auto cut = drums->build();
    const bool stopped = std::any_of(cut->pcm.begin(), cut->pcm.end(), [&](const VgmWrite& w) {
        return (w.address & 0x2000) && w.time == kick.notes[0].start + kick.notes[0].length;
    });
    check(stopped, "a shortened drum stops at its new end");
}

std::vector<float> renderSong(SongPlayer& player, size_t samples);
std::vector<float> renderSong(SongPlayer& player, size_t samples)
{
    std::vector<float> l(samples), r(samples);
    for (size_t i = 0; i < samples; i += 512)
        player.render(&l[i], &r[i], static_cast<int>(std::min<size_t>(512, samples - i)));
    for (size_t i = 0; i < samples; ++i)
        l[i] += r[i];
    return l;
}

double differenceDb(const float* a, const float* b, size_t n)
{
    double d = 0, p = 0;
    for (size_t i = 0; i < n; ++i)
        d += (a[i] - b[i]) * (a[i] - b[i]), p += b[i] * b[i];
    return 10.0 * std::log10(d / (p + 1e-30) + 1e-30);
}

void testEditPlayback()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    const auto original = tune->build();
    const auto originalSnapshots = makeSnapshots(*original);

    // An edit late in the song: the lead's fifth note, at 2 s, up a fifth.
    Tune edited = *tune;
    edited.tracks[0].notes[4].transpose = 7;
    const auto changed = edited.build();
    const auto full = makeSnapshots(*changed);

    // Snapshots kept from the original up to the change, then extended,
    // match snapshots made from scratch.
    size_t ym = 0, pcm = 0;
    firstDifference(*original, *changed, ym, pcm);
    auto kept = keepSnapshots(*originalSnapshots, ym, pcm);
    const size_t keptFm = kept->fm.size();
    extendSnapshots(*changed, *kept, songEnd(*changed));
    bool same = kept->fm.size() == full->fm.size() && kept->pcm.size() == full->pcm.size() && keptFm > 1;
    for (size_t i = 0; same && i < full->fm.size(); ++i)
        same = kept->fm[i].sample == full->fm[i].sample && kept->fm[i].write == full->fm[i].write
               && kept->fm[i].state == full->fm[i].state;
    check(same, "snapshots kept up to an edit and extended match snapshots made anew");

    // Playing at 1 s, the edit at 2 s comes in: playback carries on seamlessly
    // and sounds exactly like the edited song played from the start.
    const size_t rate = 48000, total = 3 * rate;
    SongPlayer reference;
    reference.prepare(rate);
    reference.setSong(changed, full);
    reference.play(0.0);
    const auto expected = renderSong(reference, total);

    SongPlayer live;
    live.prepare(rate);
    live.setSong(original, originalSnapshots);
    live.play(0.0);
    auto out = renderSong(live, rate);
    live.replaceSong(changed, full, ym, pcm);
    const auto rest = renderSong(live, total - rate);
    out.insert(out.end(), rest.begin(), rest.end());
    check(differenceDb(out.data(), expected.data(), total) < -200.0,
          "an edit ahead of playback takes over seamlessly, bit for bit");

    // Playing at 2.5 s, past the edit: playback jumps to the same place in
    // the edited song and, after the crossfade, plays it exactly.
    SongPlayer late;
    late.prepare(rate);
    late.setSong(original, originalSnapshots);
    late.play(0.0);
    renderSong(late, rate * 5 / 2);
    const double at = late.position();
    late.replaceSong(changed, full, ym, pcm);
    const auto after = renderSong(late, rate / 2);
    SongPlayer fresh;
    fresh.prepare(rate);
    fresh.setSong(changed, full);
    fresh.play(at);
    const auto freshOut = renderSong(fresh, rate / 2);
    const size_t settle = rate / 50;
    check(differenceDb(after.data() + settle, freshOut.data() + settle, rate / 2 - settle) < -100.0,
          "an edit behind playback jumps to the same place in the edited song");

    // The rebuilder delivers the edited song with complete snapshots.
    TuneRebuilder rebuilder;
    rebuilder.request(edited, 0.0);
    std::optional<TuneVersion> version;
    for (int tries = 0; tries < 500 && !(version && version->complete); ++tries)
    {
        if (auto v = rebuilder.take())
            version = v;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(version && version->complete && sameWrites(version->song->ym, changed->ym)
              && version->snapshots->fm.size() == full->fm.size(),
          "the rebuilder delivers the edited song with all its snapshots");
}

// A track's volume, set in the chips: its level drops by that much, alone.
void testTrackVolume()
{
    auto level = [](int track, float volume) {
        std::string error;
        auto tune = Tune::load(test::makeSong(), "Song", error);
        for (size_t t = 0; t < tune->tracks.size(); ++t)
            tune->tracks[t].solo = static_cast<int>(t) == track;
        tune->tracks[static_cast<size_t>(track)].volume = volume;
        const auto song = tune->build();
        SongPlayer player;
        player.prepare(48000.0);
        player.setSong(song, makeStartSnapshot(*song));
        player.play(0.0);
        const auto out = renderSong(player, 144000);
        double sum = 0;
        for (float v : out)
            sum += static_cast<double>(v) * v;
        return 10.0 * std::log10(sum / static_cast<double>(out.size()) + 1e-30);
    };
    const double fmDrop = level(0, 0.0f) - level(0, -12.0f);
    check(std::abs(fmDrop - 12.0) < 1.0, "an FM track at -12 dB plays 12 dB lower");
    const double pcmDrop = level(2, 0.0f) - level(2, -12.0f);
    check(std::abs(pcmDrop - 12.0) < 1.0, "a drum track at -12 dB plays 12 dB lower");
    check(level(0, TuneTrack::kSilent) < -200.0, "a track at the bottom of its fader is silent");
    const double fmBoost = level(0, 6.0f) - level(0, 0.0f);
    check(std::abs(fmBoost - 6.0) < 1.0, "an FM track at +6 dB plays 6 dB louder");
    // The test drums play at volumes 100 and 80: +1.5 dB fits under 127.
    const double pcmBoost = level(2, 1.5f) - level(2, 0.0f);
    check(std::abs(pcmBoost - 1.5) < 0.3, "a drum track at +1.5 dB plays 1.5 dB louder");
}

// Speed changes how fast the song's writes come, not the chips: at half
// speed, two seconds of output cover one second of the song.
void testSpeed()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    const auto song = tune->build();
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(song, makeStartSnapshot(*song));
    player.play(0.0);
    player.setSpeed(0.5);
    renderSong(player, 96000);
    check(std::abs(player.position() - kVgmRate) < 2.0, "at half speed, two seconds play one second of the song");
    player.setSpeed(2.0);
    renderSong(player, 48000);
    check(std::abs(player.position() - 3.0 * kVgmRate) < 2.0, "at double speed, one second plays two");
}

// Mute, solo and volume left to the player sound as they do built into the song.
void testPlayerMix()
{
    for (const int solo : { 0, 2 })
    {
        std::string error;
        auto tune = Tune::load(test::makeSong(), "Song", error);
        tune->tracks[static_cast<size_t>(solo)].solo = true;
        tune->tracks[static_cast<size_t>(solo)].volume = -12.0f;
        auto render = [&](bool mixInBuild) {
            BuildReport report;
            const auto song = tune->build(&report, mixInBuild);
            const auto mix = mixOf(*tune, report);
            SongPlayer player;
            player.prepare(48000.0);
            player.setSong(song, makeStartSnapshot(*song));
            if (!mixInBuild)
                player.setMix(mix);
            player.play(0.0);
            return renderSong(player, 144000);
        };
        const auto built = render(true), mixed = render(false);
        check(differenceDb(mixed.data(), built.data(), built.size()) < -40.0,
              solo == 0 ? "an FM track soloed at -12 dB sounds the same applied by the player"
                        : "a drum track soloed at -12 dB sounds the same applied by the player");
    }
}

int main()
{
    testPlayerMix();
    testSpeed();
    testTrackVolume();
    testEditPlayback();
    testTune();
    testSongLog();
    testVgmImport();
    testSegaPcm();
    std::printf("%s\n", failures == 0 ? "all checks passed" : "some checks failed");
    return failures == 0 ? 0 : 1;
}
