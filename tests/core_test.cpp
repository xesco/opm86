// Checks of the chip emulation and the VGM importer that need no DAW.

#include "Engine.h"
#include "OpmBank.h"
#include "PreviewVoice.h"
#include "SegaPcm.h"
#include "SongLog.h"
#include "SongExport.h"
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
#include <filesystem>
#include <functional>
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
    chip.setRom(rom.data());
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
}

void testDrumPlayback()
{
    Engine engine;
    engine.prepare(48000.0);
    std::vector<float> left(48000), right(48000);
    const uint8_t kick[3] = { 0x99, 36, 127 };
    engine.handleMidi(kick, 3);
    engine.render(left.data(), right.data(), 48000);

    float early = 0, late = 0;
    for (int i = 0; i < 4800; ++i)
        early = std::max(early, std::abs(left[static_cast<size_t>(i)]));
    for (int i = 43200; i < 48000; ++i)
        late = std::max(late, std::abs(left[static_cast<size_t>(i)]));
    check(early > 0.05f, "the default kick sounds on MIDI channel 10");
    check(late < 0.001f, "the kick has finished after one second");
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

    // Transpose the lead's first note up two semitones.
    TuneTrack& lead = tune->tracks[0];
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

// The writes to one register of a song at one time, in order.
std::vector<uint8_t> writesAt(const std::vector<VgmWrite>& writes, uint32_t time, uint16_t address)
{
    std::vector<uint8_t> out;
    for (const VgmWrite& w : writes)
        if (w.time == time && w.address == address)
            out.push_back(w.value);
    return out;
}

// A drum hit's channel and its registers as the chip has them at its
// trigger, from a built song.
struct HitStart
{
    int channel = -1;
    std::array<uint8_t, 256> regs {};
};

HitStart triggerAt(const VgmSong& song, uint32_t time)
{
    HitStart t;
    std::array<uint8_t, 256> regs {};
    for (const VgmWrite& w : song.pcm)
    {
        if (w.address & 0x2000)
            continue;
        if ((w.address & 0x87) == 0x86 && !(w.value & 1) && w.time == time)
        {
            t.channel = (w.address >> 3) & 15;
            t.regs = regs;
            return t;
        }
        regs[w.address & 0xFF] = w.value;
    }
    return t;
}

// A note's velocity, fine tune and pan: on the carriers' total level, the
// key code and fraction and the output sides of an FM note, and on a drum
// hit's volumes and step. The song (see testNoteExpression) has one FM note
// on connection 4, its carriers C1 at 8 and C2 at 20 and its modulators at
// 30, whose driver fades C1 to 16 at 1000 and to 40 at 2000, sets M1 to 4
// at 3000 and every operator to a carrier at 4000; and a drum hit at volumes
// 100 and 50 and step 0x40, then volume 50 on the left and step 0x80 at 500.
void testExpressionEdits(const Tune& faded)
{
    size_t fmIndex = 0, drumIndex = 0;
    for (size_t t = 0; t < faded.tracks.size(); ++t)
        (faded.tracks[t].kind == TuneTrack::Kind::Fm ? fmIndex : drumIndex) = t;

    // Velocity -6 dB, 8 steps: the carriers' levels go 8 up at the key on and
    // at every later write to a carrier, so the fade keeps its moves; the
    // modulators keep theirs, and the expression its curves.
    Tune quiet = faded;
    TuneNote& note = quiet.tracks[fmIndex].notes[0];
    note.velocity = -8;
    check(std::abs(note.velocityDb() + 6.0f) < 1e-6f, "a velocity of -8 steps is -6 dB");
    auto built = quiet.build();
    auto ons = keyOns(*built);
    check(ons.size() == 1, "the quieter note keys on once");
    if (ons.size() != 1)
        return;
    const KeyOn on = ons[0]; // a copy: ons is built again below
    const auto ch = static_cast<size_t>(on.channel);
    check(on.regs[0x60 + ch] == 30 && on.regs[0x68 + ch] == 30, "velocity leaves the modulators' total level as it was");
    check(on.regs[0x70 + ch] == 16 && on.regs[0x78 + ch] == 28, "velocity -6 dB raises each carrier's total level by 8 steps at the key on");
    const auto c1 = static_cast<uint16_t>(0x70 + ch), m1 = static_cast<uint16_t>(0x60 + ch);
    check(writesAt(built->ym, on.time + 1000, c1) == std::vector<uint8_t> { 24 } && writesAt(built->ym, on.time + 2000, c1) == std::vector<uint8_t> { 48 },
          "a fade after the key on fades from the new velocity, 8 steps lower all along");
    check(writesAt(built->ym, on.time + 3000, m1) == std::vector<uint8_t> { 4 }, "a modulator's later write is not moved by velocity");
    const auto m2 = static_cast<uint16_t>(0x68 + ch);
    check(writesAt(built->ym, on.time + 4000, m1) == std::vector<uint8_t> { 12 } && writesAt(built->ym, on.time + 4000, m2) == std::vector<uint8_t> { 38 }
              && writesAt(built->ym, on.time + 4000, c1).empty(),
          "a new connection that makes the modulators carriers lowers them by the velocity too, and leaves the carriers as they were");

    // A patch edit to connection 7 makes every operator a carrier from the
    // key on, so velocity lowers them all, and the driver's later write to
    // connection 7 changes none of them.
    TuneTrack& fmTrack = quiet.tracks[fmIndex];
    fmTrack.patch = quiet.basePatch(fmTrack);
    fmTrack.patch->alg = 7;
    built = quiet.build();
    ons = keyOns(*built);
    check(!ons.empty() && ons[0].regs[0x60 + ch] == 38 && ons[0].regs[0x68 + ch] == 38 && ons[0].regs[0x70 + ch] == 16 && ons[0].regs[0x78 + ch] == 28,
          "velocity lowers the carriers of the track's edited connection");
    check(writesAt(built->ym, on.time + 3000, m1) == std::vector<uint8_t> { 12 } && writesAt(built->ym, on.time + 4000, m1).empty(),
          "under an edited connection, a later write to the game's connection levels no operator again");
    fmTrack.patch.reset();
    const NoteExpression asPlayed = faded.expression(faded.tracks[fmIndex], faded.tracks[fmIndex].notes[0]);
    const NoteExpression asQuiet = quiet.expression(quiet.tracks[fmIndex], note);
    bool sameCurve = asPlayed.level.size() == asQuiet.level.size();
    for (size_t i = 0; sameCurve && i < asPlayed.level.size(); ++i)
        sameCurve = asPlayed.level[i].offset == asQuiet.level[i].offset && std::abs(asPlayed.level[i].value - asQuiet.level[i].value) < 1e-6f;
    check(sameCurve, "velocity leaves the note's level curve relative, as it was");
    note.velocity = TuneNote::kMinVelocity;
    ons = keyOns(*quiet.build());
    check(!ons.empty() && ons[0].regs[0x70 + ch] == 72 && ons[0].regs[0x78 + ch] == 84, "the lowest velocity, -48 dB, is 64 steps on the carriers");
    note.velocity = 0;

    // Pan to the left: the key on and the driver's later write to the
    // connection keep only the left side.
    note.pan = -TuneNote::kPanRange;
    built = quiet.build();
    ons = keyOns(*built);
    const auto connection = writesAt(built->ym, on.time + 4000, static_cast<uint16_t>(0x20 + ch));
    check(!ons.empty() && (ons[0].regs[0x20 + ch] & 0xC0) == 0x40 && connection.size() == 1 && (connection[0] & 0xC0) == 0x40
              && (connection[0] & 7) == 7,
          "an FM note panned left keys on out of the left side, and keeps it through the driver's writes");
    note.pan = TuneNote::kPanRange;
    ons = keyOns(*quiet.build());
    check(!ons.empty() && (ons[0].regs[0x20 + ch] & 0xC0) == 0x80, "an FM note panned right keys on out of the right side");
    note.pan.reset();

    // A voice's output sides as edited give way to a note's own pan, in the
    // song as baked with the edits, as the player applies them.
    quiet.tracks[fmIndex].voiceSides[ch] = 0x80;
    auto bakedOut = [&quiet] {
        BuildReport report;
        const auto song = quiet.build(&report);
        const auto edits = patchEditsOf(quiet, report);
        return bakeSong(*song, nullptr, edits.get());
    };
    note.pan = -TuneNote::kPanRange;
    auto baked = bakedOut();
    ons = keyOns(*baked);
    const auto bakedConnection = writesAt(baked->ym, on.time + 4000, static_cast<uint16_t>(0x20 + ch));
    check(!ons.empty() && (ons[0].regs[0x20 + ch] & 0xC0) == 0x40 && !bakedConnection.empty() && (bakedConnection.back() & 0xC0) == 0x40,
          "a note's own pan wins over its voice's output sides as edited");
    note.pan.reset();
    baked = bakedOut();
    ons = keyOns(*baked);
    check(!ons.empty() && (ons[0].regs[0x20 + ch] & 0xC0) == 0x80, "a note with no pan of its own plays out of its voice's sides as edited");
    quiet.tracks[fmIndex].voiceSides.fill(0);

    // A drum hit: velocity -6 dB halves both volumes, at the trigger and
    // after; fine tune moves the step by its share of a semitone, at the
    // trigger and after, so the octave jump stays an octave.
    TuneNote& hit = quiet.tracks[drumIndex].notes[0];
    const auto trigger = static_cast<uint32_t>(hit.start);
    hit.velocity = -8;
    hit.fine = 32;
    built = quiet.build();
    HitStart t = triggerAt(*built, trigger);
    check(t.channel >= 0, "the expressed drum hit triggers");
    if (t.channel < 0)
        return;
    auto base = static_cast<uint16_t>(t.channel * 8);
    check(t.regs[base + 2] == 50 && t.regs[base + 3] == 25, "velocity -6 dB scales a drum hit's volumes by half at the trigger");
    check(t.regs[base + 7] == 66, "fine tune of half a semitone moves the step from 0x40 to 66");
    check(writesAt(built->pcm, trigger + 500, static_cast<uint16_t>(base + 2)) == std::vector<uint8_t> { 25 },
          "a drum hit's later volume is scaled by its velocity too");
    check(writesAt(built->pcm, trigger + 500, static_cast<uint16_t>(base + 7)) == std::vector<uint8_t> { 132 },
          "a drum hit's later step keeps its fine tune: 0x80 to 132");
    const PcmVoice voice = quiet.hitVoice(quiet.tracks[drumIndex], hit, true);
    check(voice.regs[0] == 50 && voice.regs[1] == 25 && voice.regs[5] == 66, "hitVoice with the expression starts as the build triggers");
    check(quiet.pitch(quiet.tracks[drumIndex], hit) == 61 && std::abs(quiet.exactPitch(quiet.tracks[drumIndex], hit) - 60.5) < 1e-9,
          "a drum hit's pitch rounds its fine tune, and the exact pitch keeps it");

    // Pan as a balance: three quarters of the two volumes' sum to the right,
    // and the left's later write moves the right too.
    hit.velocity = 0;
    hit.fine = 0;
    hit.pan = TuneNote::kPanRange / 2;
    built = quiet.build();
    t = triggerAt(*built, trigger);
    base = static_cast<uint16_t>(std::max(0, t.channel) * 8);
    check(t.channel >= 0 && t.regs[base + 2] == 38 && t.regs[base + 3] == 113, "a drum hit's balance shares its volumes' sum, 150, a quarter left");
    check(writesAt(built->pcm, trigger + 500, static_cast<uint16_t>(base + 2)) == std::vector<uint8_t> { 25 }
              && writesAt(built->pcm, trigger + 500, static_cast<uint16_t>(base + 3)) == std::vector<uint8_t> { 75 },
          "a later volume write shares the new sum, moving the other side too");
    hit.pan = -TuneNote::kPanRange;
    t = triggerAt(*quiet.build(), trigger);
    base = static_cast<uint16_t>(std::max(0, t.channel) * 8);
    check(t.channel >= 0 && t.regs[base + 2] == 127 && t.regs[base + 3] == 0, "a drum hit balanced fully left plays its sum on the left, up to 127");
    hit.pan.reset();

    // The MIDI arrangement carries them: the FM note's levels 8 steps lower
    // and its output on the left, and the drum hit's fine tune.
    const Arrangement plain = arrangementFromTune(faded, "Fade");
    note.velocity = -8;
    note.pan = -TuneNote::kPanRange;
    hit.fine = 32;
    const Arrangement expressed = arrangementFromTune(quiet, "Fade");
    auto notesOf = [](const Arrangement& a, ArrTrack::Kind kind) -> const ArrNote* {
        for (const ArrTrack& tr : a.tracks)
            if (tr.kind == kind && !tr.notes.empty())
                return &tr.notes[0];
        return nullptr;
    };
    const ArrNote *was = notesOf(plain, ArrTrack::Kind::Fm), *now = notesOf(expressed, ArrTrack::Kind::Fm);
    bool lower = was != nullptr && now != nullptr && was->changes.size() == now->changes.size();
    for (size_t i = 0; lower && i < was->changes.size(); ++i)
        lower = now->changes[i].attenuation == std::min(127, was->changes[i].attenuation + 8) && now->changes[i].pan == FmPan::Left;
    check(lower, "the MIDI arrangement has the FM note's velocity on every level and its pan on every change");
    const ArrNote* drumNote = notesOf(expressed, ArrTrack::Kind::Drum);
    check(drumNote != nullptr && std::abs(drumNote->fine - 0.5) < 1e-9, "the MIDI arrangement has the drum hit's fine tune");

    // A drum hit's fine tune counts in the MIDI bend range: 30 semitones up
    // and half of one more need a range of 31.
    Arrangement wide;
    wide.samples.push_back(PadSample {});
    ArrTrack drums;
    drums.kind = ArrTrack::Kind::Drum;
    ArrNote high;
    high.pitch = 90;
    high.fine = 0.5;
    high.hit = { HitChange { 0, 100, 100, 0x80 } };
    drums.notes.push_back(high);
    wide.tracks.push_back(drums);
    check(arrangementMidi(wide, false).bendRange == 31, "a drum hit's fine tune widens the MIDI bend range");
}

// Fine tune on an FM note: through the key fraction, carrying into the key
// code across its unused codes, and kept by the driver's slide.
void testFineTune()
{
    check(shiftKey(0x4A, 0, 0, -1) == std::pair<uint8_t, uint8_t> { 0x49, 0xFC }, "a 64th flat of A4 is G#4 and 63/64");
    check(shiftKey(0x42, 60 << 2, 0, 10) == std::pair<uint8_t, uint8_t> { 0x44, 6 << 2 },
          "fine tune carries from key code 2 to 4, past the unused code 3");
    check(shiftKey(0x4E, 63 << 2, 0, 1) == std::pair<uint8_t, uint8_t> { 0x50, 0 }, "fine tune carries from the last code of an octave into the next");
    check(shiftKey(0x4A, 0x81, 2, 0) == std::pair<uint8_t, uint8_t> { 0x4D, 0x81 }, "with no fine tune, the key fraction stays as the game wrote it");

    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    if (!tune)
        return;
    TuneTrack& lead = tune->tracks[0];
    TuneNote& first = lead.notes[0];
    const double exact = tune->exactPitch(lead, first);
    first.fine = -1;
    auto ons = keyOns(*tune->build());
    check(!ons.empty() && ons[0].regs[0x28 + static_cast<size_t>(ons[0].channel)] == 0x49 && ons[0].regs[0x30 + static_cast<size_t>(ons[0].channel)] == 0xFC,
          "an FM note a 64th flat keys on a key code lower, its fraction at 63/64");
    check(std::abs(tune->exactPitch(lead, first) - (exact - 1.0 / 64.0)) < 1e-9 && tune->pitch(lead, first) == static_cast<int>(std::lround(exact)),
          "the exact pitch has the fine tune; the pitch rounds it to the key");
    first.fine = 0;

    // The slide: key on at A4, half a semitone up at 4410, then B4 at 8820.
    // With 40/64 on top, the half semitone carries into the next key code,
    // and B4 takes the fraction again.
    TuneNote& slide = lead.notes[3];
    slide.fine = 40;
    const auto built = tune->build();
    const auto start = static_cast<uint32_t>(slide.start);
    ons = keyOns(*built);
    const auto on = std::find_if(ons.begin(), ons.end(), [&](const KeyOn& k) { return k.time == start; });
    check(on != ons.end(), "the fine tuned slide keys on");
    if (on == ons.end())
        return;
    const auto ch = static_cast<uint16_t>(on->channel);
    check(on->regs[0x28 + ch] == 0x4A && on->regs[0x30 + ch] == 40 << 2, "the slide keys on at A4 and 40/64");
    check(writesAt(built->ym, start + 4410, static_cast<uint16_t>(0x30 + ch)) == std::vector<uint8_t> { 8 << 2 }
              && writesAt(built->ym, start + 4410, static_cast<uint16_t>(0x28 + ch)) == std::vector<uint8_t> { 0x4C },
          "the driver's half semitone plus 40/64 carries into the key code: A#4 and 8/64");
    const auto kc = writesAt(built->ym, start + 8820, static_cast<uint16_t>(0x28 + ch));
    const auto kf = writesAt(built->ym, start + 8820, static_cast<uint16_t>(0x30 + ch));
    // The driver writes the key code before the fraction, so at that sample
    // the chip passes through the half semitone above B4, as in the game.
    check(!kc.empty() && kc.back() == 0x4D && !kf.empty() && kf.back() == 40 << 2, "the slide's B4 keeps the fine tune: B4 and 40/64");
}

void testNoteExpression()
{
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    check(tune != nullptr, "the song for the expression checks loads");
    if (!tune)
        return;
    TuneTrack& lead = tune->tracks[0];
    check(tune->expression(lead, lead.notes[0]).empty(), "a note with no writes after its key on has no expression");
    check(tune->expression(lead, tune->writtenCopy(lead, lead.notes[3], 0)).empty(), "a written note has no expression");

    // The lead's fourth note slides half a semitone, then two.
    TuneNote& slide = lead.notes[3];
    auto isSlide = [&](const NoteExpression& e) {
        return e.level.empty() && e.pitch.size() == 2 && e.pitch[0].offset == 4410 && near(e.pitch[0].value, 0.5f)
               && e.pitch[1].offset == 8820 && near(e.pitch[1].value, 2.0f);
    };
    check(isSlide(tune->expression(lead, slide)), "key code and fraction writes after the key on give the pitch curve");
    slide.transpose = 5;
    check(isSlide(tune->expression(lead, slide)), "a transposed note keeps its pitch curve relative to its key on");
    slide.transpose = 0;
    // A resized note moves only the writes past its original length, so a
    // longer note keeps its curve and a shorter one loses what falls past its end.
    const int64_t length = slide.length;
    slide.length = length + 10000;
    check(isSlide(tune->expression(lead, slide)), "a lengthened note keeps the curve of its writes before its key off");
    slide.length = 6000;
    const NoteExpression cut = tune->expression(lead, slide);
    check(cut.pitch.size() == 1 && cut.pitch[0].offset == 4410 && near(cut.pitch[0].value, 0.5f),
          "a shortened note's curve ends where it now ends");
    slide.length = length;

    // A note on connection 4, carriers C1 at level 8 and C2 at 20, whose
    // driver fades C1, then raises the modulator M1, then switches every
    // operator to a carrier; and a drum hit that fades and goes up an octave.
    test::VgmBuilder v;
    std::vector<uint8_t> rom(0x200);
    for (size_t i = 0; i < rom.size(); ++i)
        rom[i] = static_cast<uint8_t>(i & 0xFF);
    v.rom(rom, 0x10000);
    v.ym(0x20, 0xC0 | 4);
    const uint8_t levels[4] = { 30, 30, 8, 20 }; // slots M1, M2, C1, C2
    for (uint8_t k = 0; k < 4; ++k)
    {
        v.ym(static_cast<uint8_t>(0x60 + 8 * k), levels[k]);
        v.ym(static_cast<uint8_t>(0x80 + 8 * k), 31);
    }
    v.ym(0x28, 0x4A);
    v.ym(0x30, 0);
    v.wait(100);
    v.ym(0x08, 0x78);
    const uint16_t base = 3 * 8;
    v.pcm(base + 0x86, 0x13);
    v.pcm(base + 0x02, 100);
    v.pcm(base + 0x03, 50);
    v.pcm(base + 0x06, 0x01);
    v.pcm(base + 0x07, 0x40);
    v.pcm(base + 0x84, 0x00);
    v.pcm(base + 0x85, 0x00);
    v.pcm(base + 0x86, 0x12);
    v.wait(500);
    v.pcm(base + 0x02, 50);
    v.pcm(base + 0x07, 0x80);
    v.wait(500);
    v.ym(0x70, 16); // C1 to 16: C1 is still the loudest, 6 dB down
    v.wait(1000);
    v.ym(0x70, 40); // C1 to 40: C2 at 20 is now the loudest, 9 dB down
    v.wait(1000);
    v.ym(0x60, 4); // M1 is a modulator on connection 4
    v.wait(1000);
    v.ym(0x20, 0xC0 | 7); // every operator a carrier: M1 at 4, 3 dB up
    v.wait(2000);
    v.ym(0x08, 0x00);
    v.wait(1000);
    auto faded = Tune::load(v.finish(), "Fade", error);
    check(faded != nullptr, "the song with a fade loads");
    if (!faded)
        return;
    const TuneTrack* fm = nullptr;
    const TuneTrack* drum = nullptr;
    for (const TuneTrack& t : faded->tracks)
        (t.kind == TuneTrack::Kind::Fm ? fm : drum) = &t;
    check(fm != nullptr && drum != nullptr && fm->notes.size() == 1 && drum->notes.size() == 1, "the fade song has one note and one hit");
    if (fm == nullptr || drum == nullptr || fm->notes.empty() || drum->notes.empty())
        return;
    const NoteExpression level = faded->expression(*fm, fm->notes[0]);
    check(level.pitch.empty() && level.level.size() == 3 && level.level[0].offset == 1000 && near(level.level[0].value, -6.0f)
              && level.level[1].offset == 2000 && near(level.level[1].value, -9.0f) && level.level[2].offset == 4000
              && near(level.level[2].value, 3.0f),
          "the loudest carrier's level gives the level curve, with the connection at each moment");
    const NoteExpression hit = faded->expression(*drum, drum->notes[0]);
    check(hit.pitch.size() == 1 && hit.pitch[0].offset == 500 && near(hit.pitch[0].value, 12.0f) && hit.level.size() == 1
              && hit.level[0].offset == 500 && near(hit.level[0].value, static_cast<float>(20.0 * std::log10(50.0 / 100.0))),
          "a drum hit's step and louder volume give its pitch and level curves");

    // The note's own expression on the fade song's note and hit.
    testExpressionEdits(*faded);
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

// Notes moved to other voices: a ripped note on another chip channel, a
// ripped note copied to another track as a written one, and conflicts moved
// to free voices.
void testNoteVoices()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    if (!tune)
        return;
    const VgmSong& original = tune->source();
    const auto before = keyOns(original);
    TuneTrack& lead = tune->tracks[0];
    TuneNote& note = lead.notes[1];
    const int game = tune->originalChannel(lead, note);
    tune->moveToVoice(lead, note, game);
    check(note.voice == -1 && sameWrites(tune->build()->ym, original.ym),
          "a ripped note moved to the game's channel has no voice of its own, and the tune builds the original writes");

    // On a channel the song never uses, the note keys on there with the
    // registers it had in the game, and the game's channel stays silent then.
    const int to = 5;
    tune->moveToVoice(lead, note, to);
    check(note.voice == to && tune->originalChannel(lead, note) == to, "a ripped note moved to another voice plays on it");
    const auto voices = tune->voices(lead);
    check(std::find(voices.begin(), voices.end(), to) != voices.end(), "the voice a ripped note moved to is one of its track's");
    BuildReport report;
    const auto after = keyOns(*tune->build(&report));
    check(report.channels[0][1] == to, "the build gives a moved ripped note its new channel");
    const auto game0 = std::find_if(before.begin(), before.end(), [&](const KeyOn& k) { return static_cast<int64_t>(k.time) == note.start && k.channel == game; });
    const auto moved = std::find_if(after.begin(), after.end(), [&](const KeyOn& k) { return static_cast<int64_t>(k.time) == note.start && k.channel == to; });
    const bool left = std::none_of(after.begin(), after.end(), [&](const KeyOn& k) { return static_cast<int64_t>(k.time) == note.start && k.channel == game; });
    check(game0 != before.end() && moved != after.end() && sameChannelState(*moved, *game0) && left,
          "a ripped note on another voice keys on there with its own registers");
    // And alone, with the other tracks muted, it sounds as it does on the game's channel.
    auto alone = [&](int channel) {
        auto t = Tune::load(test::makeSong(), "Song", error);
        TuneTrack& part = t->tracks[0];
        const TuneNote kept = part.notes[1];
        part.notes = { kept };
        t->moveToVoice(part, part.notes[0], channel);
        for (size_t k = 1; k < t->tracks.size(); ++k)
            t->tracks[k].mute = true;
        SongPlayer player;
        player.prepare(48000.0);
        const auto song = t->build();
        player.setSong(song, makeStartSnapshot(*song));
        player.play(0.0);
        return renderSong(player, 144000);
    };
    const auto a = alone(to), b = alone(game);
    const float peak = std::abs(*std::max_element(a.begin(), a.end(), [](float x, float y) { return std::abs(x) < std::abs(y); }));
    check(peak > 0.01f && differenceDb(a.data(), b.data(), a.size()) < -100.0, "a ripped note on another voice sounds as it did on the game's");

    // Copied to another track as a written note: its start, length and pitch.
    TuneTrack& bass = tune->tracks[1];
    const TuneNote copy = tune->writtenCopy(lead, lead.notes[2], 3);
    check(copy.written() && copy.id == lead.notes[2].id && copy.start == lead.notes[2].start && copy.length == lead.notes[2].length
              && copy.voice == 3 && tune->pitch(bass, copy) == tune->pitch(lead, lead.notes[2]),
          "a ripped note written on another track keeps its start, length and pitch");

    // Nine notes at once on eight channels: the ninth conflicts. Each on a
    // voice of its own, no channel is free for it.
    auto stack = [](bool shareVoice) {
        auto t = Tune::empty();
        TuneTrack track;
        track.instrument = -1;
        for (int k = 0; k < 9; ++k)
            track.notes.push_back({ k, -1, k * 10, 20000, 0, shareVoice ? 0 : k % 8 });
        t->tracks.push_back(track);
        return t;
    };
    auto spread = stack(false);
    size_t stuck = 0;
    check(spread->channels().conflicts == 1 && spread->moveConflicts(stuck) == 0 && stuck == 1,
          "a conflict with every channel busy cannot move");
    // All on one voice, the build gives notes 1 to 7 channels 1 to 7, and
    // they move there; the ninth still has no free channel.
    auto shared = stack(true);
    const auto sharedBefore = shared->build();
    const size_t movedCount = shared->moveConflicts(stuck);
    bool onTheirChannels = true;
    for (size_t k = 1; k < 8; ++k)
        onTheirChannels = onTheirChannels && shared->originalChannel(shared->tracks[0], shared->tracks[0].notes[k]) == static_cast<int>(k);
    check(movedCount == 7 && stuck == 1 && onTheirChannels && shared->channels().conflicts == 1
              && shared->originalChannel(shared->tracks[0], shared->tracks[0].notes[8]) == 0,
          "notes the build moves go to the channels they play on, and a conflict stays");
    check(sameWrites(shared->build()->ym, sharedBefore->ym), "moving notes to the channels they play on keeps the build");
    // Two notes at once on one voice: the second moves to the channel it
    // plays on, and a second run finds nothing to move.
    auto pair = Tune::empty();
    TuneTrack two;
    two.instrument = -1;
    two.notes = { { 0, -1, 0, 20000, 0, 0 }, { 1, -1, 10, 20000, 0, 0 } };
    pair->tracks.push_back(two);
    const BuildReport given = pair->channels();
    check(pair->moveConflicts(stuck) == 1 && stuck == 0 && given.channels[0][1] == 1
              && pair->originalChannel(pair->tracks[0], pair->tracks[0].notes[1]) == 1 && pair->moveConflicts(stuck) == 0,
          "a note on a busy voice moves to the free one it plays on");
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

// An edited instrument changes its own notes, wherever they play, and no others.
void testPatchEdits()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    BuildReport report;
    const auto song = tune->build(&report, false);
    const auto snapshots = makeStartSnapshot(*song);
    auto render = [&](std::shared_ptr<const PatchEdits> edits, std::shared_ptr<const ChannelMix> mix) {
        SongPlayer player;
        player.prepare(48000.0);
        player.setSong(song, snapshots);
        player.setMix(mix);
        player.setPatchEdits(edits);
        player.play(0.0);
        return renderSong(player, 144000);
    };
    std::vector<int> fm;
    for (size_t t = 0; t < tune->tracks.size(); ++t)
        if (tune->tracks[t].kind == TuneTrack::Kind::Fm)
            fm.push_back(static_cast<int>(t));
    const auto plain = render(nullptr, nullptr);

    auto& first = tune->tracks[static_cast<size_t>(fm[0])];
    first.patch = tune->patches()[static_cast<size_t>(first.instrument)];
    const auto same = render(patchEditsOf(*tune, report), nullptr);
    check(differenceDb(same.data(), plain.data(), plain.size()) < -200.0, "an instrument edited to its own values sounds as before");

    // Every carrier switched off: the instrument, alone, is silent.
    {
        Tune solo = *tune;
        auto& patch = *solo.tracks[static_cast<size_t>(fm[0])].patch;
        for (size_t i = 0; i < 4; ++i)
            if ((kCarrierMask[patch.alg] >> i) & 1)
                patch.op[i].on = false;
        solo.tracks[static_cast<size_t>(fm[0])].solo = true;
        double sum = 0;
        for (const float v : render(patchEditsOf(solo, report), mixOf(solo, report)))
            sum += static_cast<double>(v) * v;
        check(sum < 1e-12, "an instrument with its carriers switched off is silent");
    }

    // The same carriers muted to audition, on the instrument as imported:
    // silent too, and the tune keeps no trace of it.
    {
        Tune solo = *tune;
        solo.tracks[static_cast<size_t>(fm[0])].patch.reset();
        solo.tracks[static_cast<size_t>(fm[0])].solo = true;
        const uint8_t carriers = kCarrierMask[solo.basePatch(solo.tracks[static_cast<size_t>(fm[0])]).alg];
        const auto muted = patchEditsOf(solo, report, fm[0], carriers);
        check(muted != nullptr && muted->mutedTrack == fm[0] && patchEditsOf(solo, report) == nullptr,
              "audition mutes need no edit, and the edits without them have none");
        double sum = 0;
        for (const float v : render(muted, mixOf(solo, report)))
            sum += static_cast<double>(v) * v;
        check(sum < 1e-12, "an instrument with its carriers muted to audition is silent");
        PatchEdits edits;
        edits.tracks.resize(1);
        edits.mutedTrack = 0;
        edits.muted = 0x1;
        const std::array<int, 8> owner { 0, 0, 0, 0, 0, 0, 0, 0 };
        check(editedWrite(&edits, owner, 0x08, 0x7A) == 0x72, "an operator muted to audition is left out of the key on");
        FmPatch off;
        off.op[3].on = false;
        edits.tracks[0] = PatchEdits::Edit { off, FmPatch {}, {} };
        check(editedWrite(&edits, owner, 0x08, 0x7A) == 0x32, "audition mutes and switched-off operators both stay off");
    }

    first.patch->fb = static_cast<uint8_t>((first.patch->fb + 4) % 8);
    first.patch->op[0].mul = static_cast<uint8_t>((first.patch->op[0].mul + 3) % 16);
    const auto edits = patchEditsOf(*tune, report);
    check(differenceDb(render(edits, nullptr).data(), plain.data(), plain.size()) > -40.0, "an edited instrument sounds different");
    if (fm.size() > 1)
    {
        // The other instruments alone, with and without the edit.
        Tune others = *tune;
        others.tracks[static_cast<size_t>(fm[0])].mute = true;
        const auto mix = mixOf(others, report);
        const auto without = render(nullptr, mix), with = render(edits, mix);
        check(differenceDb(with.data(), without.data(), without.size()) < -100.0, "an edit leaves the other instruments as they were");
    }
}

// VOPM's .opm banks: a patch through the text and back, and a bank with two
// voices, comments and a voice with fields missing.
void testOpmBank()
{
    FmPatch p;
    p.name = "Round Trip";
    p.alg = 4, p.fb = 6, p.pms = 3, p.ams = 2, p.lfoFreq = 200, p.lfoWave = 2, p.pmd = 100, p.amd = 17;
    for (size_t i = 0; i < 4; ++i)
    {
        FmOperator& o = p.op[i];
        const auto n = static_cast<uint8_t>(i);
        o.tl = static_cast<uint8_t>(20 + 30 * n), o.ar = static_cast<uint8_t>(31 - n), o.d1r = static_cast<uint8_t>(5 + n), o.d2r = static_cast<uint8_t>(2 * n);
        o.rr = static_cast<uint8_t>(15 - n), o.d1l = static_cast<uint8_t>(3 + n), o.mul = static_cast<uint8_t>(n * 4), o.ks = n, o.dt2 = n;
        o.dt1 = static_cast<int8_t>(static_cast<int>(i) * 2 - 3);
        o.amEn = i % 2 == 1;
    }
    p.op[2].on = false;
    const auto back = parseOpm(writeOpm(p));
    const FmPatch read = back.empty() ? FmPatch {} : back[0].patch;
    check(back.size() == 1 && back[0].number == 0 && back[0].lfo && !back[0].noise, "a .opm written holds one voice with its LFO and no noise");
    check(read == p, "a patch comes back from .opm text as it went in");

    const std::string bank = "//MiOPMdrv sound bank Paramer Ver2002.04.22\n"
                             "// A comment line\n"
                             "\n"
                             "@:0 Brass Lead // a comment after the name\n"
                             "LFO: 10 20 30 1 0\n"
                             "CH: 64 7 2 1 5 88 0\n"
                             "M1: 31 10 5 6 2 30 1 2 6 1 128\n"
                             "C1: 25 9 4 7 3 0 0 1 0 0 0\n"
                             "M2: 20 8 3 8 4 40 2 3 1 2 0\n"
                             "C2: 15 7 2 9 5 5 3 4 0 3 0\n"
                             "\n"
                             "@:12 Short\r\n"
                             "CH: 64 3 5\r\n"
                             "M1: 31 0 0 4\r\n";
    const auto voices = parseOpm(bank);
    check(voices.size() == 2, "a bank with comments gives its two voices");
    if (voices.size() == 2)
    {
        const FmPatch& a = voices[0].patch;
        check(voices[0].patch.name == "Brass Lead" && a.lfoFreq == 10 && a.amd == 20 && a.pmd == 30 && a.lfoWave == 1,
              "a .opm voice's name and LFO are read");
        check(a.fb == 7 && a.alg == 2 && a.ams == 1 && a.pms == 5 && a.op[0].on && a.op[1].on && !a.op[2].on && a.op[3].on,
              "a .opm voice's channel settings and SLOT are read");
        check(a.op[0].ar == 31 && a.op[0].d1r == 10 && a.op[0].d2r == 5 && a.op[0].rr == 6 && a.op[0].d1l == 2 && a.op[0].tl == 30 && a.op[0].ks == 1
                  && a.op[0].mul == 2 && a.op[0].dt1 == -2 && a.op[0].dt2 == 1 && a.op[0].amEn && a.op[3].tl == 5 && a.op[3].dt2 == 3,
              "a .opm voice's operators are read in order, with DT1 4-7 as detune down");
        const FmPatch& b = voices[1].patch;
        check(voices[1].number == 12 && b.name == "Short" && !voices[1].lfo && b.fb == 3 && b.alg == 5 && b.op[0].rr == 4
                  && b.op[0].tl == FmOperator {}.tl && b.op[1] == FmOperator {},
              "a .opm voice with fields missing keeps the defaults");
    }
}

// How far a tone's pitch wanders: the spread of its periods, from one rising
// zero crossing to the next, against their mean.
double pitchSpread(const std::vector<float>& s, size_t from)
{
    std::vector<double> crossings;
    for (size_t i = from + 1; i < s.size(); ++i)
        if (s[i - 1] < 0.0f && s[i] >= 0.0f)
            crossings.push_back(static_cast<double>(i - 1) + static_cast<double>(s[i - 1] / (s[i - 1] - s[i])));
    if (crossings.size() < 3)
        return 0.0;
    double low = 1e9, high = 0.0, sum = 0.0;
    for (size_t i = 1; i < crossings.size(); ++i)
    {
        const double period = crossings[i] - crossings[i - 1];
        low = std::min(low, period), high = std::max(high, period), sum += period;
    }
    return (high - low) / (sum / static_cast<double>(crossings.size() - 1));
}

// The chip's LFO and noise as edited: applied by the player over the song's
// own writes, field by field, with no new build.
void testChipEdits()
{
    // A written song: A4 for two seconds on a sine with the most vibrato
    // sensitivity, whose depth the chip leaves at 0 until it is set.
    FmPatch sine;
    sine.alg = 7;
    sine.op[3].tl = 0;
    sine.pms = 7;
    auto tune = Tune::empty();
    TuneTrack t;
    t.instrument = -1;
    t.addedVoices = 0x01;
    t.patch = sine;
    t.notes.push_back({ 0, -1, 0, 88200, 9, 0 });
    tune->tracks.push_back(t);
    auto render = [](std::shared_ptr<const VgmSong> song, std::shared_ptr<const PatchEdits> edits) {
        SongPlayer player;
        player.prepare(48000.0);
        player.setSong(song, makeStartSnapshot(*song));
        player.setPatchEdits(edits);
        player.play(0.0);
        return renderSong(player, 48000);
    };
    auto play = [&render](const Tune& tn) {
        BuildReport report;
        const auto song = tn.build(&report, false);
        return render(song, patchEditsOf(tn, report));
    };
    Tune still = *tune;
    still.tracks[0].patch->pms = 0;
    const auto plain = play(*tune);
    check(differenceDb(plain.data(), play(still).data(), plain.size()) < -200.0 && pitchSpread(plain, 4800) < 0.01,
          "a written song with no chip edit has no vibrato, whatever the sensitivity");
    Tune vibrato = *tune;
    vibrato.chip.lfoRate = 0xC8;
    vibrato.chip.pmd = 127;
    check(pitchSpread(play(vibrato), 4800) > 0.03, "a written song gets vibrato with the chip's rate and depth set");
    check(sameWrites(vibrato.build()->ym, tune->build()->ym) && sameWrites(vibrato.build()->pcm, tune->build()->pcm),
          "a chip edit builds the same song: it needs no rebuild");

    // A rip-style song: the game sets the LFO's rate, then the vibrato depth
    // and the tremolo depth, both in register 0x19.
    BuildReport report;
    const auto ripped = tune->build(&report, false);
    ripped->ym.insert(ripped->ym.begin(), { { 0, 0x18, 0xC8 }, { 0, 0x19, 0x80 | 100 }, { 0, 0x19, 0x00 } });
    const auto edits = patchEditsOf(*tune, report);
    check(pitchSpread(render(ripped, edits), 4800) > 0.03, "a song that writes the LFO plays its vibrato");
    const ChipState game = chipAt(*ripped, *makeSnapshots(*ripped), 1000.0);
    check(game.lfoRate == 0xC8 && game.pmd == 100 && game.amd == 0, "the song's chip settings are read at a time, both depths apart");

    Tune flat = *tune;
    flat.chip.pmd = 0;
    const auto overridden = patchEditsOf(flat, report);
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(ripped, makeStartSnapshot(*ripped));
    player.setPatchEdits(overridden);
    player.play(0.0);
    const auto before = renderSong(player, 24000);
    const ChipState heard = player.gameChip();
    check(pitchSpread(before, 4800) < 0.01, "a chip edit replaces the song's own writes");
    check(heard.lfoRate == 0xC8 && heard.pmd == 100, "the player reports the song's chip settings under an edit");
    // Unset again while playing: the song's vibrato depth comes back, not
    // the tremolo depth it wrote last to the same register.
    player.setPatchEdits(edits);
    const auto after = renderSong(player, 24000);
    check(pitchSpread(after, 2400) > 0.03, "a chip edit unset gives back the song's own value at once");
}

// A song written in the tracker: an empty tune, a track added to it and notes
// written on two of its voices.
void testWrittenNotes()
{
    auto tune = Tune::empty();
    check(tune->tracks.empty() && !tune->ripped() && std::abs(tune->source().tempo() - 120.0) < 1e-9, "an empty tune has no tracks, at 120 beats a minute");
    TuneTrack t;
    t.instrument = -1;
    t.addedVoices = 0x03;
    t.notes.push_back({ 0, -1, 44100, 22050, 9, 0 });  // A4 on voice 0, at one second
    t.notes.push_back({ 1, -1, 44100, 22050, 12, 1 }); // C5 on voice 1, with it
    tune->tracks.push_back(t);
    check(tune->voices(tune->tracks[0]).size() == 2 && tune->pitch(tune->tracks[0], tune->tracks[0].notes[1]) == 72, "written notes have their voices and keys");

    const auto song = tune->build();
    const auto ons = keyOns(*song);
    const FmPatch& p = Tune::defaultPatch();
    check(ons.size() == 2 && ons[0].time == 44100 && ons[0].regs[0x28] == 0x4A && ons[1].regs[0x29] == 0x4E,
          "a written note keys on at its time and key (A4 at 0x4A, C5 at 0x4E)");
    check((ons[0].regs[0x20] & 7) == p.alg && ons[0].regs[0x60 + 24] == p.op[3].tl && (ons[0].regs[0x20] & 0xC0) == 0xC0,
          "a written note plays the track's instrument, out of both sides");
    bool off = false;
    for (const VgmWrite& w : song->ym)
        off = off || (w.time == 66150 && w.address == 0x08 && w.value == 0);
    check(off, "a written note keys off at its end");

    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(song, makeStartSnapshot(*song));
    player.play(0.0);
    std::vector<float> l(72000), r(72000);
    player.render(l.data(), r.data(), 72000);
    float before = 0, during = 0;
    for (size_t i = 0; i < l.size(); ++i)
        (i < 47000 ? before : during) = std::max(i < 47000 ? before : during, std::abs(l[i]));
    check(before < 1e-6f && during > 0.01f, "written notes are silent before their start and sound from it");
}

void testFreeVoice()
{
    auto tune = Tune::empty();
    TuneTrack t;
    t.instrument = -1;
    t.addedVoices = 0x06;                             // V2 and V3
    t.notes.push_back({ 0, -1, 1000, 1000, 0, 2 });   // on V3, from 1000 to 2000
    tune->tracks.push_back(t);
    const TuneTrack& fm = tune->tracks[0];
    check(tune->freeVoice(fm, 1500, 2500) == 1, "a new note goes on the lowest voice free over its span");
    check(tune->freeVoice(fm, 2000, 3000) == 1 && tune->freeVoice(fm, 0, 1000) == 1, "a note that ends or starts at the span's edge leaves its voice free");

    tune->tracks[0].notes.push_back({ 1, -1, 500, 1000, 0, 1 }); // on V2, from 500 to 1500
    check(tune->freeVoice(tune->tracks[0], 1600, 1700) == 1, "a voice whose note has ended is free");
    check(tune->freeVoice(tune->tracks[0], 1200, 1300) == 1, "with every voice busy, a new note goes on the track's lowest voice");
    tune->tracks[0].notes.erase(tune->tracks[0].notes.begin());
    check(tune->freeVoice(tune->tracks[0], 1200, 1300) == 2, "the next voice up when the lowest is busy");

    TuneTrack drum;
    drum.kind = TuneTrack::Kind::Drum;
    TuneTrack empty;
    check(tune->freeVoice(drum, 0, 100) == -1 && tune->freeVoice(empty, 0, 100) == -1, "a drum track or a track with no voices has no free voice");
}

void testPreviewVoice()
{
    // A written note on voice 0 at time 0: middle C on the default instrument.
    auto tune = Tune::empty();
    TuneTrack t;
    t.instrument = -1;
    t.addedVoices = 0x01;
    t.notes.push_back({ 0, -1, 0, 44100, 0, 0 });
    tune->tracks.push_back(t);
    const auto song = tune->build();
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(song, makeStartSnapshot(*song));
    player.play(0.0);
    constexpr size_t kQuarter = 12000;
    std::vector<float> songL(kQuarter), songR(kQuarter), l(kQuarter), r(kQuarter);
    player.render(songL.data(), songR.data(), kQuarter);

    PreviewVoice preview;
    preview.prepare(48000.0);
    preview.setChip(song->fmRate, tune->clockShift());
    preview.setPatch(Tune::defaultPatch());
    preview.noteOn(60);
    preview.render(l.data(), r.data(), kQuarter);
    float peak = 0;
    for (const float v : l)
        peak = std::max(peak, std::abs(v));
    check(peak > 0.01f && preview.sounding() && preview.pitch() == 60, "a previewed note sounds after its key on");
    check(differenceDb(l.data(), songL.data(), kQuarter) < -100.0 && differenceDb(r.data(), songR.data(), kQuarter) < -100.0,
          "a previewed note sounds as the same note written in the song");

    // Let go: the release plays out to silence, and the chip stops adding anything.
    preview.noteOff(60);
    float tail = 0;
    for (int block = 0; block < 400; ++block)
    {
        std::fill(l.begin(), l.begin() + 480, 0.0f);
        std::fill(r.begin(), r.begin() + 480, 0.0f);
        preview.render(l.data(), r.data(), 480);
        if (block >= 390)
            for (size_t i = 0; i < 480; ++i)
                tail = std::max(tail, std::abs(l[i]));
    }
    check(tail < 1e-6f && !preview.sounding() && preview.pitch() < 0, "a previewed note decays to silence after its key off and release");

    // Its carriers muted to audition: a key held goes silent, and comes back
    // when the mute ends, with the patch unchanged.
    {
        PreviewVoice muted;
        muted.prepare(48000.0);
        muted.setChip(song->fmRate, tune->clockShift());
        muted.setPatch(Tune::defaultPatch());
        muted.setMuted(kCarrierMask[Tune::defaultPatch().alg]);
        muted.noteOn(60);
        std::vector<float> a(kQuarter), b(kQuarter);
        muted.render(a.data(), b.data(), kQuarter);
        float loud = 0;
        for (const float v : a)
            loud = std::max(loud, std::abs(v));
        muted.setMuted(0);
        std::fill(a.begin(), a.end(), 0.0f);
        muted.render(a.data(), b.data(), kQuarter);
        float back = 0;
        for (const float v : a)
            back = std::max(back, std::abs(v));
        check(loud < 1e-6f && back > 0.01f, "a preview's carriers muted to audition are silent until the mute ends");
    }

    // The patch's LFO is set on the chip, so a vibrato is heard.
    auto vibrato = [](const FmPatch& p) {
        PreviewVoice v;
        v.prepare(48000.0);
        v.setChip(3579545.0 / 64.0, 0.0);
        v.setPatch(p);
        v.noteOn(69);
        std::vector<float> a(48000), b(48000);
        v.render(a.data(), b.data(), 48000);
        return a;
    };
    FmPatch strings = factoryPatches()[4];
    FmPatch still = strings;
    still.pms = 0;
    const auto with = vibrato(strings), without = vibrato(still);
    check(strings.pmd > 0 && strings.pms > 0 && differenceDb(with.data(), without.data(), with.size()) > -20.0,
          "a previewed patch's vibrato sounds");

    // A chord: each key on a channel of its own.
    preview.noteOn(60);
    preview.noteOn(64);
    preview.noteOn(67);
    preview.render(l.data(), r.data(), 480);
    check(preview.pitch() == 60, "a chord's keys sound together, the lowest reported");
    preview.noteOff(60);
    preview.render(l.data(), r.data(), 480);
    check(preview.pitch() == 64, "a chord's key let go leaves the others down");
    preview.releaseAll();

    // A fifth key takes the channel of the key down longest.
    {
        PreviewVoice v;
        v.prepare(48000.0);
        v.setChip(song->fmRate, tune->clockShift());
        v.setPatch(Tune::defaultPatch());
        for (const int key : { 60, 62, 64, 65, 67 })
            v.noteOn(key);
        v.render(l.data(), r.data(), 480);
        check(v.pitch() == 62, "a fifth key steals the channel of the oldest key");
    }

    // A note of a set length lets go by itself; a key held does not.
    {
        auto after = [&](double seconds) {
            PreviewVoice v;
            v.prepare(48000.0);
            v.setChip(song->fmRate, tune->clockShift());
            v.setPatch(Tune::defaultPatch());
            v.noteOn(60, seconds);
            for (int block = 0; block < 400; ++block)
                v.render(l.data(), r.data(), 480);
            return v.sounding();
        };
        check(!after(0.05) && after(0.0), "a previewed note of a set length lets go after it, and plays its release");
    }

    // An instrument edited in the tracker: an operator switched off and
    // some fields changed, as the song player applies them.
    FmPatch edited = Tune::defaultPatch();
    edited.fb = static_cast<uint8_t>((edited.fb + 3) % 8);
    edited.op[0].mul = static_cast<uint8_t>((edited.op[0].mul + 2) % 16);
    edited.op[2].tl = static_cast<uint8_t>(std::min(127, edited.op[2].tl + 12));
    edited.op[1].on = false;
    {
        Tune changed = *tune;
        changed.tracks[0].patch = edited;
        BuildReport report;
        const auto built = changed.build(&report, false);
        SongPlayer p;
        p.prepare(48000.0);
        p.setSong(built, makeStartSnapshot(*built));
        p.setPatchEdits(patchEditsOf(changed, report));
        p.play(0.0);
        std::fill(songL.begin(), songL.end(), 0.0f);
        std::fill(songR.begin(), songR.end(), 0.0f);
        p.render(songL.data(), songR.data(), kQuarter);

        PreviewVoice v;
        v.prepare(48000.0);
        v.setChip(built->fmRate, changed.clockShift());
        v.setPatch(edited);
        v.noteOn(60);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        v.render(l.data(), r.data(), kQuarter);
        float loud = 0;
        for (const float x : l)
            loud = std::max(loud, std::abs(x));
        check(loud > 0.01f && differenceDb(l.data(), songL.data(), kQuarter) < -100.0 && differenceDb(r.data(), songR.data(), kQuarter) < -100.0,
              "a previewed edited instrument, an operator off, sounds as the song plays it");
    }

    // A held key takes a new patch at once.
    {
        auto held = [&](bool change) {
            PreviewVoice v;
            v.prepare(48000.0);
            v.setChip(song->fmRate, tune->clockShift());
            v.setPatch(Tune::defaultPatch());
            v.noteOn(60);
            std::vector<float> a(4800), b(4800);
            v.render(a.data(), b.data(), 4800);
            if (change)
                v.setPatch(edited);
            std::fill(a.begin(), a.end(), 0.0f);
            v.render(a.data(), b.data(), 4800);
            return a;
        };
        const auto changed = held(true), same = held(false);
        check(differenceDb(changed.data(), same.data(), same.size()) > -40.0, "a held key changes its sound with the patch");
    }

    // Stopped for the song: the sound ends within milliseconds, and a
    // patch set after it changes nothing.
    {
        PreviewVoice v;
        v.prepare(48000.0);
        v.setChip(song->fmRate, tune->clockShift());
        v.setPatch(Tune::defaultPatch());
        v.noteOn(60);
        v.render(l.data(), r.data(), 4800);
        v.stop();
        v.setPatch(edited);
        float end = 0;
        for (int block = 0; block < 10; ++block)
        {
            std::fill(l.begin(), l.begin() + 480, 0.0f);
            v.render(l.data(), r.data(), 480);
            if (block >= 5)
                for (size_t i = 0; i < 480; ++i)
                    end = std::max(end, std::abs(l[i]));
        }
        check(end < 1e-4f && !v.sounding(), "a stopped preview is silent within 50 milliseconds");
    }
}

// The song's grid and meter, a tempo change, and a loop in the player.
void testSongStructure()
{
    std::string error;
    auto rip = Tune::load(test::makeSong(), "Song", error);
    const VgmSong& source = rip->source();
    check(std::abs(rip->beatLength() - source.beatLength) < 1e-9 && std::abs(rip->firstBeat() - source.firstBeat) < 1e-9
              && rip->beatsPerBar() == 4 && !rip->loop.on,
          "a rip's grid starts as the grid fitted to its notes, in 4/4, with no loop");
    auto empty = Tune::empty();
    check(std::abs(empty->tempo() - 120.0) < 1e-9 && std::abs(empty->firstBeat()) < 1e-9 && empty->beatsPerBar() == 4
              && std::abs(empty->barLength() - 2.0 * kVgmRate) < 1e-9,
          "a song with no rip starts at 120 beats a minute in 4/4");

    // A rip keeps the game's timing: only the grid changes.
    std::vector<int64_t> starts;
    for (const TuneTrack& t : rip->tracks)
        for (const TuneNote& n : t.notes)
            starts.push_back(n.start);
    rip->setTempo(90.0);
    bool kept = true;
    size_t k = 0;
    for (const TuneTrack& t : rip->tracks)
        for (const TuneNote& n : t.notes)
            kept = kept && n.start == starts[k++];
    const auto built = rip->build();
    check(kept && std::abs(rip->tempo() - 90.0) < 1e-9 && std::abs(rip->firstBeat() - source.firstBeat) < 1e-9
              && std::abs(built->beatLength - rip->beatLength()) < 1e-9 && sameWrites(built->ym, source.ym),
          "a rip's new tempo moves its grid, not its notes, and the build carries the grid");

    // A written song keeps its notes on their beats, counted from the first beat.
    auto song = Tune::empty();
    song->grid.firstBeat = 1000.0;
    const double beat = song->beatLength();
    TuneTrack t;
    t.instrument = -1;
    t.addedVoices = 0x01;
    t.notes.push_back({ 0, -1, std::llround(1000.0 + 2.0 * beat), std::llround(beat), 0, 0 });
    t.notes.push_back({ 1, -1, std::llround(1000.0 + 5.5 * beat), std::llround(beat / 2), 0, 0 });
    song->tracks.push_back(t);
    const int64_t before = song->length();
    song->setTempo(60.0);
    const double after = song->beatLength();
    const auto& notes = song->tracks[0].notes;
    check(std::abs(after - 2.0 * beat) < 1e-9 && notes[0].start == std::llround(1000.0 + 2.0 * after)
              && notes[1].start == std::llround(1000.0 + 5.5 * after) && notes[0].length == std::llround(after)
              && notes[1].length == std::llround(after / 2),
          "a written song's new tempo keeps its notes' starts and lengths in beats");
    check(song->length() > before && song->build()->length == static_cast<uint32_t>(song->length()), "a written song's length follows its tempo");
    auto waltz = Tune::empty();
    waltz->grid.beatsPerBar = 3;
    check(waltz->length() == std::llround(waltz->barLength() * 8), "a song with no rip has eight bars of its meter to write in");

    // A loop: at its end, playback goes on from its start with the next
    // sample, exactly as it plays from there once the crossfade is over.
    const auto played = rip->build();
    const auto snapshots = makeSnapshots(*played);
    const double rate = 48000.0, from = 0.25 * kVgmRate, loopStart = 0.5 * kVgmRate, loopEnd = 1.0 * kVgmRate;
    SongPlayer looped;
    looped.prepare(rate);
    looped.setSong(played, snapshots);
    looped.setLoop(loopStart, loopEnd);
    looped.play(from);
    const size_t total = 50000; // one jump, and less than the loop again after it
    std::vector<float> l(total), r(total);
    looped.render(l.data(), r.data(), static_cast<int>(total));
    const double step = 1.0 * kVgmRate / rate;
    const auto wrap = static_cast<size_t>(std::ceil((loopEnd - from) / step));
    check(std::abs(looped.position() - (loopStart + static_cast<double>(total - wrap) * step)) < 1.0,
          "past the loop's end, the position is back after its start");

    SongPlayer straight;
    straight.prepare(rate);
    straight.setSong(played, snapshots);
    straight.play(loopStart);
    std::vector<float> l2(total - wrap), r2(total - wrap);
    straight.render(l2.data(), r2.data(), static_cast<int>(total - wrap));
    for (size_t i = 0; i < total; ++i)
        l[i] += r[i];
    for (size_t i = 0; i < l2.size(); ++i)
        l2[i] += r2[i];
    const size_t settle = 480; // 10 ms, past the 3 ms crossfade
    check(differenceDb(l.data() + wrap + settle, l2.data() + settle, l2.size() - settle) < -100.0,
          "after the loop's jump, playback is exact from the loop's start");

    looped.setLoop(0.0, 0.0);
    looped.render(l.data(), r.data(), 48000);
    check(looped.position() > loopEnd, "with the loop off, playback plays through its end");

    // A loop shorter than the shortest is made longer, and in the player one
    // no longer than the 3 ms crossfade plays through.
    const LoopRegion tiny { 1000, 1010, true };
    check(tiny.fitted().end == 1000 + LoopRegion::kMinLength && LoopRegion {}.fitted().end == 0,
          "a loop is at least 10 ms long, and one with no length stays as it is");
    looped.setLoop(loopStart, loopStart + 50.0);
    looped.play(loopStart);
    looped.render(l.data(), r.data(), 4800);
    check(looped.position() > loopStart + 50.0, "a loop shorter than the crossfade plays through");
}

// A song played from its start by the plain player, the left and right
// channels summed, as renderSong has them.
std::vector<float> playSong(std::shared_ptr<const VgmSong> song, std::shared_ptr<const ChannelMix> mix,
                            std::shared_ptr<const PatchEdits> edits, size_t samples)
{
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(song, makeStartSnapshot(*song));
    player.setMix(mix);
    player.setPatchEdits(edits);
    player.play(0.0);
    return renderSong(player, samples);
}

// The VGM writer: a song written and read back is the same song.
void testVgmWriter()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    const auto song = tune->build();
    auto logged = [](const std::vector<VgmWrite>& w, size_t count) { return std::vector<VgmWrite>(w.begin(), w.begin() + static_cast<long>(count)); };
    for (const bool gzip : { false, true })
    {
        const auto file = writeVgm(*song, "Song", gzip);
        check(gzip == (file.size() > 2 && file[0] == 0x1F && file[1] == 0x8B), gzip ? "a .vgz is gzipped" : "a .vgm is not gzipped");
        const auto back = loadVgmSong(file, "Song", error);
        check(back != nullptr && error.empty(), gzip ? "a written .vgz reads back without error" : "a written .vgm reads back without error");
        if (!back)
            continue;
        check(sameWrites(logged(back->ym, back->ymLogged), logged(song->ym, song->ymLogged))
                  && sameWrites(logged(back->pcm, back->pcmLogged), logged(song->pcm, song->pcmLogged)),
              "a written song reads back with the same writes at the same times");
        check(back->length == song->length && back->header.ym2151Clock == song->header.ym2151Clock
                  && back->header.segaPcmClock == song->header.segaPcmClock && back->header.segaPcmInterface == song->header.segaPcmInterface
                  && back->rom == song->rom,
              "a written song keeps its length, clocks, PCM interface and sample memory");
        if (!gzip)
        {
            const auto a = playSong(song, nullptr, nullptr, 144000), b = playSong(back, nullptr, nullptr, 144000);
            check(differenceDb(b.data(), a.data(), a.size()) < -100.0, "a written song plays as the song it was written from");
        }
    }
    check(sameWrites(bakedSong(*tune)->ym, tune->build(nullptr, false)->ym) && sameWrites(bakedSong(*tune)->pcm, tune->build(nullptr, false)->pcm),
          "an unedited tune bakes to its build");

    // A shortened drum's stop is written as its channel's flags.
    Tune cut = *tune;
    for (auto& t : cut.tracks)
        if (t.kind == TuneTrack::Kind::Drum)
            for (auto& n : t.notes)
                n.length = std::max<int64_t>(1, n.length / 4);
    const auto cutSong = bakedSong(cut);
    const auto cutBack = loadVgmSong(writeVgm(*cutSong, "Cut", false), "Cut", error);
    check(cutBack != nullptr && differenceDb(playSong(cutBack, nullptr, nullptr, 144000).data(), playSong(cutSong, nullptr, nullptr, 144000).data(), 144000) < -100.0,
          "a song with shortened drums plays the same written and read back");
}

// A baked song plays as the tracker plays the tune with its edits.
void testBakedSong()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    std::vector<size_t> fm, drums;
    for (size_t t = 0; t < tune->tracks.size(); ++t)
        (tune->tracks[t].kind == TuneTrack::Kind::Fm ? fm : drums).push_back(t);
    auto live = [](const Tune& tn) {
        BuildReport report;
        const auto song = tn.build(&report, false);
        return playSong(song, mixOf(tn, report), patchEditsOf(tn, report), 144000);
    };
    auto baked = [](const Tune& tn) { return playSong(bakedSong(tn), nullptr, nullptr, 144000); };

    // An edited instrument: feedback, a multiple, an operator off and the output on one side; and the chip's LFO.
    Tune edited = *tune;
    auto& track = edited.tracks[fm[0]];
    FmPatch patch = edited.patches()[static_cast<size_t>(track.instrument)];
    patch.fb = static_cast<uint8_t>((patch.fb + 4) % 8);
    patch.op[0].mul = static_cast<uint8_t>((patch.op[0].mul + 3) % 16);
    patch.op[1].tl = static_cast<uint8_t>(std::min(127, patch.op[1].tl + 6));
    patch.op[2].on = false;
    patch.pms = 5;
    track.patch = patch;
    for (const int v : edited.voices(track))
        track.voiceSides[static_cast<size_t>(v)] = 0x40;
    edited.chip.lfoRate = 0xC0;
    edited.chip.pmd = 90;
    const auto plain = live(*tune), editedLive = live(edited);
    check(differenceDb(editedLive.data(), plain.data(), plain.size()) > -40.0, "the edits change the sound");
    check(differenceDb(baked(edited).data(), editedLive.data(), editedLive.size()) < -100.0,
          "a song baked with an edited instrument and chip plays as the player plays the edits");

    // Mute and solo: a silent channel loses its output.
    Tune muted = edited;
    muted.tracks[fm[0]].mute = true;
    if (!drums.empty())
        muted.tracks[drums[0]].mute = true;
    const auto mutedLive = live(muted);
    check(differenceDb(baked(muted).data(), mutedLive.data(), mutedLive.size()) < -100.0, "a baked song keeps the mutes exactly");

    // Volumes: the chips' steps, close to the player's gains.
    Tune quiet = edited;
    quiet.tracks[fm[0]].volume = -9.0f;
    quiet.tracks[fm[0]].voiceVolume[static_cast<size_t>(edited.voices(quiet.tracks[fm[0]]).front())] = -3.0f;
    if (!drums.empty())
        quiet.tracks[drums[0]].volume = -6.0f;
    const auto quietLive = live(quiet);
    check(differenceDb(baked(quiet).data(), quietLive.data(), quietLive.size()) < -40.0,
          "a baked song's track and voice volumes come close to the player's");

    // And read back from a file, the same again.
    const auto file = loadVgmSong(writeVgm(*bakedSong(edited), "Edited", true), "Edited", error);
    check(file != nullptr && differenceDb(playSong(file, nullptr, nullptr, 144000).data(), editedLive.data(), editedLive.size()) < -100.0,
          "an exported VGM plays as the tracker plays the edited tune");
}

// The WAV writer and the offline render.
void testWavExport()
{
    const auto path = (std::filesystem::temp_directory_path() / "opm86_core_test.wav").string();
    std::vector<float> l(1000), r(1000);
    for (size_t i = 0; i < l.size(); ++i)
        l[i] = std::sin(static_cast<float>(i) * 0.1f), r[i] = -l[i] * 2.0f; // the right side clips
    for (const int bits : { 16, 24 })
    {
        WavWriter wav;
        check(wav.open(path, 44100, bits), "a WAV file opens for writing");
        wav.write(l.data(), r.data(), 600);
        wav.write(l.data() + 600, r.data() + 600, 400);
        check(wav.close(), "a WAV file closes");
        std::vector<uint8_t> bytes(std::filesystem::file_size(path));
        if (std::FILE* f = std::fopen(path.c_str(), "rb"))
        {
            check(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size(), "a WAV file reads back");
            std::fclose(f);
        }
        auto u32 = [&bytes](size_t at) { return static_cast<uint32_t>(bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | bytes[at + 3] << 24); };
        auto u16 = [&bytes](size_t at) { return static_cast<uint32_t>(bytes[at] | bytes[at + 1] << 8); };
        const size_t frame = static_cast<size_t>(2 * bits / 8);
        check(bytes.size() == 44 + 1000 * frame && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") && u32(4) == bytes.size() - 8
                  && std::equal(bytes.begin() + 8, bytes.begin() + 16, "WAVEfmt ") && u16(20) == 1 && u16(22) == 2 && u32(24) == 44100
                  && u32(28) == 44100 * frame && u16(32) == frame && u16(34) == static_cast<uint32_t>(bits)
                  && std::equal(bytes.begin() + 36, bytes.begin() + 40, "data") && u32(40) == 1000 * frame,
              bits == 16 ? "a 16-bit WAV file has a stereo PCM header and every frame" : "a 24-bit WAV file has a stereo PCM header and every frame");
        // The right side of frame 16, sin(1.6) x -2, clips at full scale.
        const size_t at = 44 + 16 * frame + frame / 2;
        const int32_t right = bits == 16 ? static_cast<int16_t>(u16(at)) : (static_cast<int32_t>(u32(at) << 8) >> 8);
        check(right == (bits == 16 ? -32767 : -8388607), "a sample beyond full scale is clipped");
    }
    std::filesystem::remove(path);

    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    tune->tracks[0].patch = tune->basePatch(tune->tracks[0]);
    tune->tracks[0].patch->fb = 7;
    auto renderAll = [](TuneRenderer& renderer, std::vector<float>& out) {
        if (!renderer.prepare([] { return true; }))
            return false;
        float a[512], b[512];
        while (const int n = renderer.render(a, b, 512))
            for (int i = 0; i < n; ++i)
                out.push_back(a[i] + b[i]);
        return true;
    };
    TuneRenderer whole(*tune, 48000.0, false);
    std::vector<float> all;
    check(renderAll(whole, all) && whole.progress() >= 1.0, "a tune renders to its end");
    const double seconds = static_cast<double>(all.size()) / 48000.0;
    const double length = static_cast<double>(tune->length()) / kVgmRate;
    check(seconds >= length && seconds <= length + 3.0 + 1e-3, "a render runs past the song's end, at most to where the tracker stops");
    BuildReport report;
    const auto song = tune->build(&report, false);
    const auto played = playSong(song, mixOf(*tune, report), patchEditsOf(*tune, report), 144000);
    check(all.size() >= played.size() && differenceDb(all.data(), played.data(), played.size()) < -200.0,
          "a render is what the tracker plays");

    tune->loop = { 22050, 66150, true };
    TuneRenderer loop(*tune, 44100.0, true);
    std::vector<float> once;
    check(renderAll(loop, once) && once.size() == 44100, "a render of the loop plays it once");
    TuneRenderer cancelled(*tune, 44100.0, false);
    check(!cancelled.prepare([] { return false; }), "a render can be stopped before it starts");
}

// MIDI from a tune: a track per track heard, in the song's meter.
void testTuneMidi()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    tune->grid.beatsPerBar = 3;
    tune->tracks.back().mute = true;
    size_t heard = 0;
    for (const auto& t : tune->tracks)
        heard += tune->audible(t) ? 1 : 0;
    const Arrangement a = arrangementFromTune(*tune, "Song");
    const MidiExport midi = exportMidi(*tune, "Song");
    const std::vector<uint8_t> meter { 0xFF, 0x58, 0x04, 3, 2 };
    check(a.tracks.size() == heard && midi.tracks.size() == heard, "a tune's MIDI has a track for each track heard");
    check(std::search(midi.file.begin(), midi.file.end(), meter.begin(), meter.end()) != midi.file.end(), "a tune's MIDI has its meter");
    const TuneNote& first = tune->tracks[0].notes[0];
    check(a.tracks[0].notes[0].start == first.start && a.tracks[0].notes[0].length == first.length
              && a.tracks[0].notes[0].pitch == std::clamp(tune->pitch(tune->tracks[0], first), 0, 127),
          "a tune's MIDI notes keep their starts, lengths and pitches");
}


// A SegaPCM channel's registers at each trigger of a song, in order.
struct Trigger
{
    uint32_t time = 0;
    int channel = 0;
    uint8_t flags = 0;
    std::array<uint8_t, 8> regs {}; // as pcmRegister lists them
};

std::vector<Trigger> triggers(const VgmSong& song)
{
    std::vector<Trigger> out;
    std::array<uint8_t, 256> ram {};
    for (const VgmWrite& w : song.pcm)
    {
        if (w.address & 0x3000)
            continue;
        ram[w.address & 0xFF] = w.value;
        if ((w.address & 0x87) == 0x86 && !(w.value & 1))
        {
            Trigger t;
            t.time = w.time;
            t.channel = (w.address >> 3) & 15;
            t.flags = w.value;
            for (int i = 0; i < kPcmRegs; ++i)
                t.regs[static_cast<size_t>(i)] = ram[pcmRegister(t.channel, i) & 0xFF];
            out.push_back(t);
        }
    }
    return out;
}

float peakOf(const std::vector<float>& v, size_t from, size_t to)
{
    float peak = 0;
    for (size_t i = from; i < to && i < v.size(); ++i)
        peak = std::max(peak, std::abs(v[i]));
    return peak;
}

// The VGM header's SegaPCM interface register sets the banks for the
// importer and the player alike.
void testPcmBanking()
{
    const SegaPcm::Banking plain = SegaPcm::banking(0), outRun = SegaPcm::banking(0x0070000C);
    check(plain.shift == 12 && plain.mask == 0x70 && outRun.shift == 12 && outRun.mask == 0x70,
          "no interface register decodes as Out Run's 512 KB banks");
    const SegaPcm::Banking small = SegaPcm::banking(0x0000000B), wide = SegaPcm::banking(0x00F8000D);
    check(small.offset(0x13) == 0x8000 && wide.offset(0x83) == 0x100000, "the interface register's shift and mask place a bank");

    // Bank bits 0x10 with a shift of 11: the sample at 0x8000, where the
    // 512 KB mode would read 0x10000. The two hold opposite signs.
    test::VgmBuilder v;
    std::vector<uint8_t> rom(0x10200, 0x80);
    std::fill(rom.begin() + 0x8000, rom.begin() + 0x8200, static_cast<uint8_t>(0xC0));
    std::fill(rom.begin() + 0x10000, rom.begin() + 0x10200, static_cast<uint8_t>(0x40));
    v.rom(rom, 0);
    v.pcm(0x86, 0x13);
    v.pcm(0x02, 127);
    v.pcm(0x03, 127);
    v.pcm(0x06, 0x01);
    v.pcm(0x07, 0x80);
    v.pcm(0x84, 0x00);
    v.pcm(0x85, 0x00);
    v.pcm(0x86, 0x12);
    v.wait(8820);
    auto file = v.finish();
    test::put32(file, 0x3C, 0x0000000B);

    const VgmImport imported = importVgm(file, "Banks");
    check(imported.samples.size() == 1 && !imported.samples[0].data.empty() && imported.samples[0].data[0] == 0xC0,
          "the importer cuts a sample from the bank the header's interface register selects");
    std::string error;
    const auto song = loadVgmSong(file, "Banks", error);
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(song, makeStartSnapshot(*song));
    player.play(0.0);
    // The sample lasts 33 ms; its first 25 ms hold its level, whose sign tells the bank.
    const auto out = renderSong(player, 1200);
    double mean = 0;
    for (const float x : out)
        mean += x / static_cast<double>(out.size());
    check(mean > 0.05, "the player reads the bank the header's interface register selects, as the importer does");
}

// A drum track's sample edit: the built registers change for its hits, and
// every other track builds as the game played it.
void testSampleEdit()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    if (!tune || tune->tracks.size() < 4)
        return;
    const auto original = triggers(tune->source());
    TuneTrack& kick = tune->tracks[2];
    TuneTrack& burst = tune->tracks[3];
    const PadSample* pad = tune->sample(kick);
    check(pad != nullptr && pad->address == 0x0000 && sampleLastPage(*pad) == 0x07 && pad->left == 100 && pad->step == 0x80,
          "an imported sample knows its place in the ROM and its usual volume and step");

    SampleEdit e;
    e.start = 0x0100;
    e.endPage = 0x04;
    e.loop = true;
    e.loopStart = 0x0180;
    e.step = 0x40;
    e.left = 50;
    e.right = 90;
    kick.sample = e;
    kick.notes[1].transpose = 12;
    const auto built = tune->build();
    const auto after = triggers(*built);
    check(after.size() == original.size(), "an edited sample's hits trigger as many times");
    bool edited = true, others = true;
    size_t k = 0;
    for (size_t i = 0; i < after.size() && i < original.size(); ++i)
    {
        const Trigger &a = after[i], &o = original[i];
        if (o.channel == 0) // the kick's channel in the game
        {
            const uint8_t step = k == 1 ? 0x80 : 0x40;
            edited = edited && a.regs[0] == 50 && a.regs[1] == 90 && a.regs[2] == 0x80 && a.regs[3] == 0x01 && a.regs[4] == 0x04
                     && a.regs[5] == step && a.regs[6] == 0x00 && a.regs[7] == 0x01 && (a.flags & 0x02) == 0;
            ++k;
        }
        else
            others = others && a.time == o.time && a.channel == o.channel && a.flags == o.flags && a.regs == o.regs;
    }
    check(k == 6 && edited, "a sample edit sets every hit's start, end, loop, pitch at its transposition, and volumes");
    check(others, "a sample edit leaves the other drum track's hits as the game played them");
    check(sameWrites(built->ym, tune->source().ym), "a sample edit leaves the FM writes as they were");
    // A one-shot made to loop stops at each hit's end.
    const bool cut = std::any_of(built->pcm.begin(), built->pcm.end(), [&](const VgmWrite& w) {
        return (w.address & 0x2000) && w.time == kick.notes[0].start + kick.notes[0].length;
    });
    check(cut, "a one-shot sample made to loop is cut at its hit's end");

    // Unset fields keep the game's values: only the step moves.
    burst.sample.step = 0x60;
    kick.sample = {};
    kick.notes[1].transpose = 0;
    const auto onlyStep = triggers(*tune->build());
    bool stepOnly = onlyStep.size() == original.size();
    for (size_t i = 0; stepOnly && i < onlyStep.size(); ++i)
    {
        std::array<uint8_t, 8> expect = original[i].regs;
        if (original[i].channel == 1)
            expect[5] = 0x60;
        stepOnly = onlyStep[i].regs == expect && onlyStep[i].flags == original[i].flags;
    }
    check(stepOnly, "a sample edit's unset fields keep the game's values");
    burst.sample = {};
    check(sameWrites(tune->build()->pcm, tune->source().pcm), "a sample edit taken back builds the original log");

    // An edit out of the sample's range is kept inside it.
    SampleEdit wild;
    wild.start = 0x5000;
    wild.endPage = 0x30;
    wild.loopStart = 0x0010;
    const SampleEdit fitted = wild.fitted(*pad);
    check(fitted.endPage == 0x07 && fitted.start == 0x07FF && fitted.loopStart == 0x07FF, "a sample edit is kept inside the sample's ROM range");

    // A ripped one-shot made to loop loops where the Sample window shows,
    // and a start moved up takes the end and the loop with it.
    kick.sample = {};
    kick.sample.loop = true;
    const PcmVoice looped = tune->hitVoice(kick, kick.notes[0]);
    const int shown = std::clamp<int>(sampleFirst(*pad) + static_cast<int>(pad->loopStart), looped.regs[6] | looped.regs[7] << 8,
                                      ((looped.regs[4] + 1) << 8) - 1);
    check((looped.flags & 0x02) == 0 && (looped.regs[2] | looped.regs[3] << 8) == shown,
          "a ripped one-shot made to loop loops at the sample's own loop point");
    kick.sample.start = 0x0750;
    const PcmVoice trimmed = tune->hitVoice(kick, kick.notes[0]);
    const int loopAt = trimmed.regs[2] | trimmed.regs[3] << 8;
    check(trimmed.regs[4] >= 0x07 && loopAt >= 0x0750 && loopAt < (trimmed.regs[4] + 1) << 8,
          "a ripped hit's end and loop stay at or past an edited start");
    kick.sample = {};
}

// Written drum hits: a free channel, the sample's registers, a trigger and a cut.
void testWrittenDrums()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    if (!tune || tune->tracks.size() < 4)
        return;
    const BuildReport before = tune->channels();
    TuneTrack& burst = tune->tracks[3];
    const int64_t at = burst.notes[0].start;
    const int64_t natural = tune->sampleLength(burst, 0);
    check(natural > 0 && tune->sampleLength(burst, 12) < natural, "a one-shot sample's length shortens as it goes up");
    burst.notes.push_back({ tune->newNoteId(), -1, at, natural, 0, -1 });
    burst.notes.push_back({ tune->newNoteId() + 1, -1, at, natural / 4, 12, -1 });
    std::stable_sort(burst.notes.begin(), burst.notes.end(), [](const TuneNote& a, const TuneNote& b) { return a.start < b.start; });
    BuildReport report;
    const auto built = tune->build(&report);
    std::vector<int> written;
    for (size_t n = 0; n < burst.notes.size(); ++n)
        if (burst.notes[n].written())
            written.push_back(report.channels[3][n]);
    check(written.size() == 2 && written[0] == 15 && written[1] == 2 && report.conflicts == 0,
          "written drum hits take a free channel the game does not use, the last first");
    bool ripped = true;
    for (size_t t = 0; t < 3; ++t)
        ripped = ripped && report.channels[t] == before.channels[t];
    check(ripped, "written drum hits move none of the game's notes and hits");

    const auto all = triggers(*built);
    const auto on15 = std::find_if(all.begin(), all.end(), [](const Trigger& t) { return t.channel == 15; });
    const auto on2 = std::find_if(all.begin(), all.end(), [](const Trigger& t) { return t.channel == 2; });
    check(on15 != all.end() && on15->time == at && on15->regs[0] == 80 && on15->regs[1] == 80 && on15->regs[4] == 0x0B && on15->regs[5] == 0x80
              && on15->regs[6] == 0x00 && on15->regs[7] == 0x08 && (on15->flags & 0x02) != 0,
          "a written drum hit triggers with its sample's start, end, step and the game's usual volumes, once");
    check(on2 != all.end() && on2->regs[5] == 0xFF, "a written drum hit an octave up doubles its step, up to the chip's highest");
    const bool cut2 = std::any_of(built->pcm.begin(), built->pcm.end(), [&](const VgmWrite& w) {
        return w.address == (0x2000 | (2 * 8 + 0x86)) && w.time == at + natural / 4;
    });
    const bool cut15 = std::any_of(built->pcm.begin(), built->pcm.end(), [](const VgmWrite& w) { return w.address == (0x2000 | (15 * 8 + 0x86)); });
    check(!cut15 && cut2, "a written one-shot hit as long as its sample plays out; a shorter one is cut at its end");

    // Alone in the song, the hit is silent before its start and sounds from it.
    auto alone = Tune::load(test::makeSong(), "Song", error);
    for (TuneTrack& t : alone->tracks)
        t.notes.clear();
    alone->tracks[3].notes.push_back({ 0, -1, 22050, natural, 0, -1 });
    const auto song = alone->build();
    SongPlayer player;
    player.prepare(48000.0);
    player.setSong(song, makeStartSnapshot(*song));
    player.play(0.0);
    const auto out = renderSong(player, 48000);
    check(peakOf(out, 0, 23000) < 1e-6f && peakOf(out, 24000, 30000) > 0.01f, "a written drum hit is silent before its start and sounds from it");

    // A hit shorter than its sample is cut at its end.
    alone->tracks[3].notes[0].length = natural / 4;
    const auto shorter = alone->build();
    const bool cut = std::any_of(shorter->pcm.begin(), shorter->pcm.end(), [&](const VgmWrite& w) {
        return (w.address & 0x2000) && w.time == 22050 + natural / 4;
    });
    check(cut, "a written drum hit shorter than its sample is cut at its end");

    // The preview plays the sample as the hit in the song.
    alone->tracks[3].notes[0] = { 0, -1, 0, natural, 0, -1 };
    const auto atZero = alone->build();
    SongPlayer p;
    p.prepare(48000.0);
    p.setSong(atZero, makeStartSnapshot(*atZero));
    p.play(0.0);
    constexpr size_t kLength = 9600;
    std::vector<float> songL(kLength), songR(kLength), l(kLength), r(kLength);
    p.render(songL.data(), songR.data(), kLength);
    PreviewVoice preview;
    preview.prepare(48000.0);
    preview.setPcm(alone->sourcePointer());
    preview.pcmOn(alone->sampleVoice(alone->tracks[3], 0));
    preview.render(l.data(), r.data(), 480);
    const bool sounding = preview.pcmSounding();
    preview.render(l.data() + 480, r.data() + 480, kLength - 480);
    check(peakOf(l, 0, kLength) > 0.01f && sounding && !preview.pcmSounding(), "a previewed one-shot sample sounds, then ends by itself");
    check(differenceDb(l.data(), songL.data(), kLength) < -100.0 && differenceDb(r.data(), songR.data(), kLength) < -100.0,
          "a previewed sample sounds as the same hit written in the song");
}

// Switching songs while playing: a new version at the playhead sounds as
// that version played from there, a note it no longer has stops with the
// crossfade, a switch waits for snapshots that do not reach the playhead,
// and a newer version while one waits is told against what plays.
void testSongSwitch()
{
    std::string error;
    auto tune = Tune::load(test::makeSong(), "Song", error);
    if (!tune)
        return;
    // The lead, on chip channel 0, and the middle of its third note.
    size_t lead = 0;
    for (size_t t = 0; t < tune->tracks.size(); ++t)
        if (tune->tracks[t].kind == TuneTrack::Kind::Fm && tune->originalChannel(tune->tracks[t], tune->tracks[t].notes[0]) == 0)
            lead = t;
    const TuneNote& third = tune->tracks[lead].notes[2];
    const double at = static_cast<double>(third.start + third.length / 2);
    const size_t rate = 48000;
    const auto toAt = static_cast<size_t>(at * rate / kVgmRate);
    const size_t settle = rate / 50; // past the 3 ms crossfade and the filters

    const auto a = tune->build();
    const auto aSnapshots = makeSnapshots(*a);
    auto version = [&tune](const std::function<void(Tune&)>& change) {
        Tune t = *tune;
        change(t);
        return std::shared_ptr<const VgmSong>(t.build());
    };
    // A playing from the start up to the playhead.
    auto playing = [&](SongPlayer& player, uint8_t fm = 0xFF, uint16_t pcm = 0xFFFF) {
        player.prepare(rate);
        player.setChannels(fm, pcm);
        player.setSong(a, aSnapshots);
        player.play(0.0);
        renderSong(player, toAt);
    };
    auto fresh = [&](const std::shared_ptr<const VgmSong>& song, double from, size_t length, uint8_t fm = 0xFF, uint16_t pcm = 0xFFFF) {
        SongPlayer player;
        player.prepare(rate);
        player.setChannels(fm, pcm);
        player.setSong(song, makeSnapshots(*song));
        player.play(from);
        return renderSong(player, length);
    };

    // The note under the playhead a fourth up: from the switch on, the new
    // version sounds as it does played from there.
    const auto b = version([lead](Tune& t) { t.tracks[lead].notes[2].transpose = 5; });
    const auto bSnapshots = makeSnapshots(*b);
    size_t ym = 0, pcm = 0;
    firstDifference(*a, *b, ym, pcm);
    SongPlayer live;
    playing(live);
    const double switchAt = live.position();
    live.replaceSong(b, bSnapshots, ym, pcm);
    const auto heard = renderSong(live, rate / 2);
    const auto expected = fresh(b, switchAt, rate / 2);
    check(differenceDb(heard.data() + settle, expected.data() + settle, rate / 2 - settle) < -100.0,
          "a switch mid-note sounds as the new version played from the playhead, after the crossfade");

    // The lead gone: its channel, keyed in the old version, is silent once
    // the crossfade ends, with no note left hanging.
    const auto c = version([lead](Tune& t) { t.tracks[lead].notes.clear(); });
    firstDifference(*a, *c, ym, pcm);
    SongPlayer cut;
    playing(cut, 0x01, 0);
    const auto before = renderSong(cut, rate / 100);
    cut.replaceSong(c, makeSnapshots(*c), ym, pcm);
    const auto after = renderSong(cut, rate / 2);
    check(peakOf(before, 0, before.size()) > 0.01f && peakOf(after, rate / 200, after.size()) < 1e-6f,
          "a switch to a version with no note there leaves no channel keyed");

    // Snapshots that end before the playhead: the old version plays on, and
    // the switch takes place once more snapshots reach it.
    auto early = std::make_shared<SongSnapshots>();
    extendSnapshots(*b, *early, at - 1.5 * kVgmRate);
    firstDifference(*a, *b, ym, pcm);
    SongPlayer waits, reference;
    playing(waits);
    playing(reference);
    const bool switched = waits.replaceSong(b, early, ym, pcm);
    const auto meanwhile = renderSong(waits, rate / 10);
    const auto oldOnly = renderSong(reference, rate / 10);
    const bool waiting = !switched && waits.switching() && differenceDb(meanwhile.data(), oldOnly.data(), rate / 10) < -200.0;
    const double late = waits.position();
    waits.updateSnapshots(bSnapshots);
    const auto then = renderSong(waits, rate / 2);
    const auto lateExpected = fresh(b, late, rate / 2);
    check(waiting && !waits.switching() && differenceDb(then.data() + settle, lateExpected.data() + settle, rate / 2 - settle) < -100.0,
          "a switch waits for snapshots that reach the playhead, the old version playing on meanwhile");

    // While B waits, a newer version D, B with a change far ahead: D shares
    // with B everything up to that change, but not the note under the
    // playhead with what plays, so playback moves to D with the crossfade.
    const auto d = version([lead](Tune& t) {
        t.tracks[lead].notes[2].transpose = 5;
        t.tracks[lead].notes.back().transpose = 7;
    });
    size_t ymBd = 0, pcmBd = 0;
    firstDifference(*b, *d, ymBd, pcmBd);
    SongPlayer coalesced;
    playing(coalesced);
    coalesced.replaceSong(b, early, ym, pcm);
    const double newest = coalesced.position();
    coalesced.replaceSong(d, makeSnapshots(*d), ymBd, pcmBd);
    const auto dHeard = renderSong(coalesced, rate / 2);
    const auto dExpected = fresh(d, newest, rate / 2);
    check(differenceDb(dHeard.data() + settle, dExpected.data() + settle, rate / 2 - settle) < -100.0,
          "a newer version while a switch waits takes its place, against the version that plays");

    // A new tempo's map: the new version plays from the playhead moved.
    SongPlayer moved;
    playing(moved);
    const double from = moved.position();
    moved.replaceSong(b, bSnapshots, ym, pcm, TimeMap::scaled(0.0, 0.5));
    const auto movedHeard = renderSong(moved, rate / 2);
    const auto movedExpected = fresh(b, from * 0.5, rate / 2);
    check(differenceDb(movedHeard.data() + settle, movedExpected.data() + settle, rate / 2 - settle) < -100.0,
          "a switch with a new tempo plays on from the playhead moved to the same beat");

    // A slower tempo whose switch waits, with the loop in the new time: the
    // old version reaches the loop's end in its own time and jumps back to
    // the loop's start in the new one, where the switch takes place.
    auto start = std::make_shared<SongSnapshots>();
    extendSnapshots(*b, *start, 0.6 * kVgmRate);
    SongPlayer looped;
    playing(looped);
    const double here = looped.position();
    const TimeMap slower = TimeMap::scaled(0.0, 2.0);
    looped.setLoop(0.25 * kVgmRate, slower(here + 0.05 * kVgmRate));
    const bool held = !looped.replaceSong(b, start, ym, pcm, slower) && looped.switching();
    renderSong(looped, rate / 10);
    check(held && !looped.switching() && std::abs(looped.position() - 0.3 * kVgmRate) < 0.005 * kVgmRate,
          "a loop set in a new tempo's time ends where the old version, still playing while a switch waits, reaches it");

    // While a switch waits, the old version keeps the mix it had: one for
    // the new version's channels, here every channel muted, waits for it.
    ChannelMix silent;
    for (auto& changes : silent.changes)
        changes.push_back({ 0.0, 0.0f });
    SongPlayer kept, unmixed;
    playing(kept);
    playing(unmixed);
    kept.setMix(std::make_shared<const ChannelMix>(silent));
    kept.replaceSong(b, early, ym, pcm);
    const auto keptHeard = renderSong(kept, rate / 10);
    const auto unmixedHeard = renderSong(unmixed, rate / 10);
    check(kept.switching() && differenceDb(keptHeard.data(), unmixedHeard.data(), rate / 10) < -200.0,
          "while a switch waits, the old version plays with its own mix, not the new version's");
}

// A sample as it plays on its own SegaPCM channel: the left output of each
// chip sample of the song, from its writes, on channel's own.
std::vector<int32_t> pcmOutput(const VgmSong& song, int channel, size_t samples)
{
    SegaPcm chip;
    chip.reset();
    chip.setRom(song.rom.data(), song.rom.size(), song.header.segaPcmInterface);
    std::vector<int32_t> out;
    size_t write = 0;
    for (size_t k = 0; k < samples; ++k)
    {
        const double now = static_cast<double>(k) * kVgmRate / song.pcmRate;
        for (; write < song.pcm.size() && song.pcm[write].time <= now; ++write)
            if (!(song.pcm[write].address & 0x3000))
                chip.write(song.pcm[write].address, song.pcm[write].value);
        int32_t l = 0, r = 0;
        chip.generate(l, r, static_cast<uint16_t>(1u << channel));
        out.push_back(l);
    }
    return out;
}

// Samples loaded from audio: encoded for the chip, placed in free pages of
// the banks, played by written hits and in place of a rip's sample.
void testLoadedSamples()
{
    // A short tone at the chip's rate, made as it is: each byte its value.
    constexpr size_t kFrames = 3000;
    std::vector<float> tone(kFrames);
    for (size_t i = 0; i < kFrames; ++i)
        tone[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
    const float* channels[] = { tone.data() };
    SampleEncoding plain;
    plain.trim = plain.normalise = false;
    const PadSample sound = encodeSample("Tone", channels, 1, kFrames, 15625.0, plain);
    bool bytes = sound.data.size() == kFrames && sound.step == 0x80 && !sound.loop;
    for (size_t i = 0; bytes && i < kFrames; ++i)
        bytes = sound.data[i] == static_cast<uint8_t>(std::lround(tone[i] * 127.0) + 0x80);
    check(bytes, "audio at the chip's rate encodes as 8-bit unsigned bytes, played once");
    SampleEncoding shaped;
    std::vector<float> quiet(kFrames, 0.0f);
    for (size_t i = 1000; i < 2000; ++i)
        quiet[i] = 0.25f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
    const float* stereo[] = { quiet.data(), quiet.data() };
    const PadSample trimmed = encodeSample("Quiet", stereo, 2, kFrames, 31250.0, shaped);
    const auto loudest = std::max_element(trimmed.data.begin(), trimmed.data.end());
    check(trimmed.data.size() < 600 && trimmed.data.size() > 400 && loudest != trimmed.data.end() && *loudest >= 0xFE,
          "a trimmed, normalised sample keeps only its sound, at full scale, at the rate chosen");

    // A song written from empty takes the sample, and a written hit plays its bytes.
    auto written = Tune::empty();
    const int index = written->loadSample(-1, sound);
    const PadSample* placed = index == 0 && written->samples().size() == 1 ? &written->samples()[0] : nullptr;
    check(placed != nullptr && placed->data == sound.data
              && std::equal(sound.data.begin(), sound.data.end(), written->rom().begin() + static_cast<long>(placed->romOffset + placed->address))
              && std::all_of(written->source().rom.begin(), written->source().rom.end(), [](uint8_t v) { return v == 0x80; }),
          "a song written from empty takes a sample into its ROM, the empty one it had left as it was");
    TuneTrack drum;
    drum.kind = TuneTrack::Kind::Drum;
    drum.instrument = index;
    drum.name = "Drum 1";
    const int64_t length = written->sampleLength(drum, 0);
    drum.notes.push_back({ 0, -1, 22050, length, 0, -1 });
    written->tracks.push_back(drum);
    BuildReport report;
    const auto song = written->build(&report);
    const int ch = report.channels[0][0];
    const auto out = pcmOutput(*song, ch, static_cast<size_t>(song->pcmRate * 0.75));
    const auto trigger = static_cast<size_t>(std::ceil(22050.0 * song->pcmRate / kVgmRate));
    bool plays = ch >= 0 && placed != nullptr && out.size() > trigger + 2 * kFrames;
    for (size_t i = 0; plays && i < kFrames; ++i)
        plays = out[trigger + 2 * i] == (sound.data[i] - 0x80) * 127 && out[trigger + 2 * i + 1] == out[trigger + 2 * i];
    check(plays && std::all_of(out.begin(), out.begin() + static_cast<long>(trigger), [](int32_t v) { return v == 0; }),
          "a written hit of a loaded sample plays its bytes, at its step, from its start");

    // A ripped sample replaced: only the hits of that sample change, to the
    // new one's place and bank, played once; the rest of the song stays.
    std::string error;
    auto rip = Tune::load(test::makeSong(), "Song", error);
    if (!rip)
        return;
    const auto original = rip->build();
    int replaced = -1, kept = -1;
    for (const TuneTrack& t : rip->tracks)
        if (t.kind == TuneTrack::Kind::Drum)
            (replaced < 0 ? replaced : kept) = t.instrument;
    const uint16_t oldStart = rip->samples()[static_cast<size_t>(replaced)].address, keptStart = rip->samples()[static_cast<size_t>(kept)].address;
    check(rip->loadSample(replaced, sound) == replaced && rip->sampleLoaded(replaced) && !rip->sampleLoaded(kept), "a rip's sample takes a loaded one's place");
    const PadSample& now = rip->samples()[static_cast<size_t>(replaced)];
    const auto edited = rip->build();
    const auto was = triggers(*original), is = triggers(*edited);
    bool others = was.size() == is.size() && sameWrites(original->ym, edited->ym), moved = true;
    size_t hitsMoved = 0;
    for (size_t i = 0; others && i < was.size(); ++i)
    {
        const uint16_t start = static_cast<uint16_t>(was[i].regs[6] | was[i].regs[7] << 8);
        if (start == keptStart)
            others = was[i].time == is[i].time && was[i].regs == is[i].regs && was[i].flags == is[i].flags;
        else if (start == oldStart)
        {
            ++hitsMoved;
            const uint16_t newStart = static_cast<uint16_t>(is[i].regs[6] | is[i].regs[7] << 8);
            moved = moved && newStart == now.address && is[i].regs[4] == sampleLastPage(now) && (is[i].flags & 0x70) == (now.bank & 0x70)
                    && (is[i].flags & 0x02) != 0 && is[i].time == was[i].time;
        }
    }
    check(others && moved && hitsMoved > 0, "a replaced sample changes only its own hits, to its new place, played once");
    check(std::equal(original->rom.begin(), original->rom.begin() + 0xC00, edited->rom.begin()) && rip->source().rom == original->rom,
          "the rip's ROM keeps its samples, and the rip itself stays as it was");
    rip->setLoadedSamples({});
    const auto back = rip->build();
    check(sameWrites(back->ym, original->ym) && sameWrites(back->pcm, original->pcm) && back->rom == original->rom,
          "with no sample loaded, the song builds as the rip again");

    // The banks: a sample keeps to one bank, ends before its last page and
    // takes no page in use; the ROM grows by whole banks as far as the
    // banking reaches, and no further.
    auto fill = [](uint32_t interfaceRegister, int count, std::vector<PadSample>& out) {
        auto file = test::makeSong();
        test::put32(file, 0x3C, interfaceRegister);
        std::string e;
        auto t = Tune::load(file, "Song", e);
        PadSample big;
        big.data.assign(kMaxSampleBytes, 0x90);
        PadSample small;
        small.data.assign(0x1000, 0x70);
        int placedCount = 0;
        for (int i = 0; i < count; ++i)
            if (t->loadSample(-1, i == 0 ? small : big) >= 0)
                ++placedCount;
        out.assign(t->samples().begin(), t->samples().end());
        return std::make_pair(placedCount, t->rom().size());
    };
    std::vector<PadSample> all;
    const auto [eight, size] = fill(0x0070000C, 9, all);
    bool banks = eight == 8 && size == SegaPcm::kRomSize && all.size() == 10;
    RomRanges ranges;
    for (size_t i = 0; banks && i < all.size(); ++i)
    {
        const PadSample& s = all[i];
        const size_t first = s.address >> 8, last = (s.address + s.data.size() - 1) >> 8;
        banks = s.romOffset == SegaPcm::banking(0x0070000C).offset(s.bank) && last <= 0xFE && first <= last;
        const size_t from = s.romOffset + (s.address & 0xFF00u), to = s.romOffset + ((last + 1) << 8);
        for (const auto& [x, y] : ranges)
            banks = banks && (to <= x || from >= y);
        ranges.emplace_back(from, to);
    }
    check(banks && all[2].romOffset == 0 && all[2].address == 0x0C00 && all[3].romOffset == 0x10000,
          "loaded samples take the free pages of the rip's banks, one bank each, none over another");
    const auto [wider, grown] = fill(0x00F0000C, 9, all);
    const auto [capped, capSize] = fill(0x00F00014, 2, all);
    check(wider == 9 && grown == 0x90000 && capped == 1 && capSize == SegaPcm::kRomSize,
          "the ROM grows by a bank as the banking reaches, and never past the chip's 16 MB");

    // A rip that reads more than its samples: a hit from 0x0200 that loops
    // back to 0x0000, before its start, and plays on to page 5 once its end
    // page moves there. In a bank past the ROM, the chip reads it through
    // the mirror, from bank 0.
    auto reading = [](uint32_t interfaceRegister, uint8_t bank) {
        test::VgmBuilder v;
        v.rom(std::vector<uint8_t>(0x600, 0x90), 0);
        v.pcm(0x86, static_cast<uint8_t>(bank | 0x01));
        v.pcm(0x02, 100);
        v.pcm(0x03, 100);
        v.pcm(0x04, 0x00);
        v.pcm(0x05, 0x00);
        v.pcm(0x06, 0x03);
        v.pcm(0x07, 0x80);
        v.pcm(0x84, 0x00);
        v.pcm(0x85, 0x02);
        v.pcm(0x86, bank);
        v.wait(4410);
        v.pcm(0x06, 0x05);
        v.wait(4410);
        v.pcm(0x86, static_cast<uint8_t>(bank | 0x01));
        v.wait(4410);
        auto file = v.finish();
        test::put32(file, 0x3C, interfaceRegister);
        std::string e;
        return Tune::load(file, "Reads", e);
    };
    PadSample short1;
    short1.data.assign(0x100, 0x70);
    auto loops = reading(0x0070000C, 0x00);
    const int at = loops ? loops->loadSample(-1, short1) : -1;
    const PadSample* away = at >= 0 ? &loops->samples()[static_cast<size_t>(at)] : nullptr;
    check(away != nullptr && (away->romOffset != 0 || away->address >= 0x0600),
          "a loaded sample takes no page the rip reads outside its samples, as a loop before a start or an end page moved");
    auto mirrored = reading(0x00F0000C, 0x80);
    PadSample big;
    big.data.assign(kMaxSampleBytes, 0x90);
    int fitted = 0;
    while (mirrored && fitted < 16 && mirrored->loadSample(-1, big) >= 0)
        ++fitted;
    bool clear = mirrored != nullptr && mirrored->rom().size() == SegaPcm::kRomSize && fitted == 7;
    for (int i = 0; clear && i < 0x600; ++i)
        clear = mirrored->romByte(static_cast<size_t>(i)) == 0x90;
    check(clear, "a rip that reads through the ROM's mirror keeps the ROM within it, and the bank it reads there");
}

int main()
{
    testOpmBank();
    testVgmWriter();
    testBakedSong();
    testWavExport();
    testTuneMidi();
    testSongStructure();
    testChipEdits();
    testPreviewVoice();
    testWrittenNotes();
    testFreeVoice();
    testPatchEdits();
    testPlayerMix();
    testSpeed();
    testTrackVolume();
    testEditPlayback();
    testTune();
    testNoteVoices();
    testNoteExpression();
    testFineTune();
    testSongLog();
    testVgmImport();
    testSegaPcm();
    testDrumPlayback();
    testPcmBanking();
    testSampleEdit();
    testWrittenDrums();
    testSongSwitch();
    testLoadedSamples();
    std::printf("%s\n", failures == 0 ? "all checks passed" : "some checks failed");
    return failures == 0 ? 0 : 1;
}
