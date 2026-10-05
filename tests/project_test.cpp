// Checks of the project file: every note's columns saved and read back, in
// binary and as JSON, and the shorter rows an older project has read with
// the expression as played.

#include "Project.h"

#include <juce_core/juce_core.h>

#include <cstdio>

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

TuneNote makeNote(int id, int source, int voice, int velocity, int fine, std::optional<int> pan)
{
    TuneNote n;
    n.id = id;
    n.source = source;
    n.start = id * 1000;
    n.length = 500 + id;
    n.transpose = id - 3;
    n.voice = voice;
    n.velocity = velocity;
    n.fine = fine;
    n.pan = pan;
    return n;
}

bool sameNotes(const std::vector<TuneNote>& a, const std::vector<TuneNote>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const TuneNote &p = a[i], &q = b[i];
        if (p.id != q.id || p.source != q.source || p.start != q.start || p.length != q.length || p.transpose != q.transpose || p.voice != q.voice
            || p.velocity != q.velocity || p.fine != q.fine || p.pan != q.pan)
            return false;
    }
    return true;
}

// The tracks a project holds, read back from a file; none when it cannot be read.
std::vector<TuneTrack> tracksOf(const juce::File& file)
{
    juce::String error;
    const auto contents = project::read(file, error);
    return contents ? contents->tracks : std::vector<TuneTrack> {};
}

} // namespace

int main()
{
    const juce::File dir = juce::File::createTempFile("opm86-project-test");
    dir.createDirectory();

    // One FM track and one drum track with every kind of row: a written note
    // as played (6 columns), a ripped one as played on the game's channel
    // (5), notes with velocity and fine tune (8) and with a pan (9).
    auto tune = Tune::empty();
    TuneTrack fm;
    fm.kind = TuneTrack::Kind::Fm;
    fm.instrument = -1;
    fm.name = "Lead";
    fm.notes = { makeNote(1, -1, 2, 0, 0, std::nullopt), makeNote(2, 0, -1, 0, 0, std::nullopt), makeNote(3, -1, 0, -8, 12, std::nullopt),
                 makeNote(4, 1, -1, -4, 0, std::nullopt), makeNote(5, -1, 1, 0, 0, -TuneNote::kPanRange),
                 makeNote(6, -1, 0, TuneNote::kMinVelocity, -TuneNote::kMaxFine, 0) };
    TuneTrack drum;
    drum.kind = TuneTrack::Kind::Drum;
    drum.instrument = 0;
    drum.name = "Kick";
    drum.notes = { makeNote(7, -1, -1, 0, 0, std::nullopt), makeNote(8, -1, 3, -2, -5, 20) };
    tune->tracks = { fm, drum };

    juce::String error;
    const juce::File binary = dir.getChildFile("song.opm86"), json = dir.getChildFile("song.json");
    check(project::save(*tune, juce::File(), binary, error) && project::isBinary(binary), "the project saves in binary");
    auto tracks = tracksOf(binary);
    check(tracks.size() == 2 && sameNotes(tracks[0].notes, fm.notes) && sameNotes(tracks[1].notes, drum.notes),
          "every note's columns come back from the packed binary form");

    check(project::exportJson(*tune, juce::File(), json, error) && !project::isBinary(json), "the project exports as JSON");
    tracks = tracksOf(json);
    check(tracks.size() == 2 && sameNotes(tracks[0].notes, fm.notes) && sameNotes(tracks[1].notes, drum.notes),
          "every note's columns come back from JSON");

    // The rows are as short as each note allows.
    const juce::var root = juce::JSON::parse(json);
    const juce::var rows = root["tracks"][0]["notes"];
    bool shapes = rows.isArray() && rows.size() == 6;
    const int columns[] = { 6, 5, 8, 8, 9, 9 };
    for (int i = 0; shapes && i < 6; ++i)
        shapes = rows[i].size() == columns[i];
    check(shapes, "a row has 5 or 6 columns as played, 8 with velocity or fine tune, 9 with a pan");

    // Rows from an older project: 5 and 6 columns, read with the expression
    // as played; a ripped FM note with no voice plays on the game's channel,
    // a written one on V1.
    juce::Array<juce::var> old;
    old.add(juce::Array<juce::var> { 1, 0, 100, 50, 2 });
    old.add(juce::Array<juce::var> { 2, -1, 200, 50, 0 });
    old.add(juce::Array<juce::var> { 3, -1, 300, 50, -1, 4 });
    root["tracks"][0].getDynamicObject()->setProperty("notes", old);
    const juce::File older = dir.getChildFile("older.json");
    older.replaceWithText(juce::JSON::toString(root));
    tracks = tracksOf(older);
    const std::vector<TuneNote> expected { makeNote(0, 0, -1, 0, 0, std::nullopt), makeNote(0, -1, 0, 0, 0, std::nullopt),
                                           makeNote(0, -1, 4, 0, 0, std::nullopt) };
    bool asPlayed = tracks.size() == 2 && tracks[0].notes.size() == 3;
    for (size_t i = 0; asPlayed && i < 3; ++i)
    {
        const TuneNote& n = tracks[0].notes[i];
        asPlayed = n.voice == expected[i].voice && n.velocity == 0 && n.fine == 0 && !n.pan && n.start == static_cast<int64_t>(i + 1) * 100;
    }
    check(asPlayed, "an older project's 5 and 6 column rows read with the expression as played");

    // A sample loaded from audio, with a drum track that plays it: the
    // project keeps its bytes, raw in binary and as base64 in JSON, and
    // opens with the sample in the ROM and the hit playing it.
    auto drums = Tune::empty();
    PadSample sound;
    sound.name = "Clap";
    sound.step = 0x40;
    for (int i = 0; i < 700; ++i)
        sound.data.push_back(static_cast<uint8_t>(0x80 + (i * 37) % 90 - 45));
    TuneTrack clap;
    clap.kind = TuneTrack::Kind::Drum;
    clap.instrument = drums->loadSample(-1, sound);
    clap.name = "Drum 1";
    clap.notes = { makeNote(1, -1, -1, 0, 0, std::nullopt) };
    drums->tracks = { clap };
    const auto built = drums->build();
    const juce::File sampled = dir.getChildFile("sampled.opm86"), sampledJson = dir.getChildFile("sampled.json");
    bool kept = project::save(*drums, juce::File(), sampled, error) && project::exportJson(*drums, juce::File(), sampledJson, error);
    for (const juce::File& f : { sampled, sampledJson })
    {
        juce::File rip;
        const auto opened = project::load(f, rip, error);
        kept = kept && opened && opened->samples().size() == 1 && opened->samples()[0].data == sound.data && opened->samples()[0].step == 0x40
               && opened->samples()[0].name == "Clap" && opened->rom() == drums->rom() && opened->build()->pcm.size() == built->pcm.size();
    }
    const juce::var saved = juce::JSON::parse(sampledJson)["samples"][0]["data"];
    juce::MemoryOutputStream decoded;
    check(kept && saved.isString() && juce::Base64::convertFromBase64(decoded, saved.toString()) && decoded.getDataSize() == sound.data.size(),
          "a loaded sample's bytes come back from a project, binary or JSON, and the song plays it");

    dir.deleteRecursively();
    std::printf(failures == 0 ? "all checks passed\n" : "%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
