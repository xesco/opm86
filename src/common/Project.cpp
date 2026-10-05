#include "Project.h"

#include "Instruments.h"

#include <cstring>

namespace opm86::project
{

namespace
{

constexpr int kFormat = 1;

juce::var bytes(const uint8_t* data, size_t count)
{
    juce::Array<juce::var> out;
    for (size_t i = 0; i < count; ++i)
        out.add(static_cast<int>(data[i]));
    return out;
}

juce::var floats(const float* data, size_t count)
{
    juce::Array<juce::var> out;
    for (size_t i = 0; i < count; ++i)
        out.add(static_cast<double>(data[i]));
    return out;
}

template <size_t N, typename T>
void readArray(const juce::var& v, std::array<T, N>& out)
{
    if (const auto* a = v.getArray())
        for (size_t i = 0; i < N && i < static_cast<size_t>(a->size()); ++i)
            out[i] = static_cast<T>(static_cast<double>((*a)[static_cast<int>(i)]));
}

// The binary form: a header, then the project's tree gzipped, each value a
// tag and its contents. Numbers are varints, signed ones zigzagged, and each
// track's notes are packed as the change from the note before.
constexpr char kMagic[6] = { 'O', 'P', 'M', '8', '6', 0 };
constexpr int kMaxDepth = 32;
enum Tag : uint8_t { Null, False, True, Int, Double, String, Array, Object, Binary };

void putVarint(juce::OutputStream& out, uint64_t v)
{
    for (; v >= 0x80; v >>= 7)
        out.writeByte(static_cast<char>((v & 0x7F) | 0x80));
    out.writeByte(static_cast<char>(v));
}

bool getVarint(juce::InputStream& in, uint64_t& v)
{
    v = 0;
    for (int shift = 0; shift < 64; shift += 7)
    {
        if (in.isExhausted())
            return false;
        const auto b = static_cast<uint8_t>(in.readByte());
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

uint64_t zigzag(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
int64_t unzigzag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }

void putString(juce::OutputStream& out, const juce::String& s)
{
    const auto utf8 = s.toUTF8();
    const auto size = utf8.sizeInBytes() - 1;
    putVarint(out, size);
    out.write(utf8.getAddress(), size);
}

bool getString(juce::InputStream& in, juce::String& s)
{
    uint64_t size = 0;
    if (!getVarint(in, size) || size > static_cast<uint64_t>(in.getNumBytesRemaining()))
        return false;
    juce::MemoryBlock bytes;
    in.readIntoMemoryBlock(bytes, static_cast<ssize_t>(size));
    s = juce::String::fromUTF8(static_cast<const char*>(bytes.getData()), static_cast<int>(bytes.getSize()));
    return bytes.getSize() == size;
}

void encode(juce::OutputStream& out, const juce::var& v)
{
    if (v.isBool())
        out.writeByte(static_cast<char>(static_cast<bool>(v) ? True : False));
    else if (v.isInt() || v.isInt64())
        out.writeByte(static_cast<char>(Int)), putVarint(out, zigzag(static_cast<juce::int64>(v)));
    else if (v.isDouble())
        out.writeByte(static_cast<char>(Double)), out.writeDouble(static_cast<double>(v));
    else if (v.isString())
        out.writeByte(static_cast<char>(String)), putString(out, v.toString());
    else if (const auto* a = v.getArray())
    {
        out.writeByte(static_cast<char>(Array));
        putVarint(out, static_cast<uint64_t>(a->size()));
        for (const auto& e : *a)
            encode(out, e);
    }
    else if (const auto* o = v.getDynamicObject())
    {
        out.writeByte(static_cast<char>(Object));
        putVarint(out, static_cast<uint64_t>(o->getProperties().size()));
        for (const auto& p : o->getProperties())
        {
            putString(out, p.name.toString());
            encode(out, p.value);
        }
    }
    else if (const auto* b = v.getBinaryData())
    {
        out.writeByte(static_cast<char>(Binary));
        putVarint(out, b->getSize());
        out.write(b->getData(), b->getSize());
    }
    else
        out.writeByte(static_cast<char>(Null));
}

bool decode(juce::InputStream& in, juce::var& v, int depth = 0)
{
    if (depth > kMaxDepth || in.isExhausted())
        return false;
    uint64_t n = 0;
    switch (static_cast<uint8_t>(in.readByte()))
    {
        case Null: v = juce::var(); return true;
        case False: v = false; return true;
        case True: v = true; return true;
        case Int:
            if (!getVarint(in, n))
                return false;
            v = static_cast<juce::int64>(unzigzag(n));
            return true;
        case Double:
            if (in.getNumBytesRemaining() < 8)
                return false;
            v = in.readDouble();
            return true;
        case String:
        {
            juce::String s;
            if (!getString(in, s))
                return false;
            v = s;
            return true;
        }
        case Array:
        {
            // Each element takes a byte at least, so a count beyond what is left is damage.
            if (!getVarint(in, n) || n > static_cast<uint64_t>(in.getNumBytesRemaining()))
                return false;
            juce::Array<juce::var> a;
            for (uint64_t i = 0; i < n; ++i)
            {
                juce::var e;
                if (!decode(in, e, depth + 1))
                    return false;
                a.add(e);
            }
            v = a;
            return true;
        }
        case Object:
        {
            if (!getVarint(in, n) || n > static_cast<uint64_t>(in.getNumBytesRemaining()))
                return false;
            auto* o = new juce::DynamicObject();
            v = juce::var(o);
            for (uint64_t i = 0; i < n; ++i)
            {
                juce::String name;
                juce::var e;
                if (!getString(in, name) || name.isEmpty() || !decode(in, e, depth + 1))
                    return false;
                o->setProperty(name, e);
            }
            return true;
        }
        case Binary:
        {
            if (!getVarint(in, n) || n > static_cast<uint64_t>(in.getNumBytesRemaining()))
                return false;
            juce::MemoryBlock b;
            in.readIntoMemoryBlock(b, static_cast<ssize_t>(n));
            v = b;
            return true;
        }
        default: return false;
    }
}

// A track's notes, each [id, source, start, length, transpose] and the
// optional columns after them (see tree), packed: the field count, then
// each field less the note before's (id, source, start) or as it is (the
// rest), zigzagged.
juce::MemoryBlock packNotes(const juce::Array<juce::var>& notes)
{
    juce::MemoryOutputStream out;
    putVarint(out, static_cast<uint64_t>(notes.size()));
    std::array<juce::int64, 3> before {};
    for (const auto& n : notes)
    {
        const auto* f = n.getArray();
        const int count = f ? f->size() : 0;
        putVarint(out, static_cast<uint64_t>(count));
        for (int i = 0; i < count; ++i)
        {
            const auto value = static_cast<juce::int64>((*f)[i]);
            putVarint(out, zigzag(i < 3 ? value - before[static_cast<size_t>(i)] : value));
            if (i < 3)
                before[static_cast<size_t>(i)] = value;
        }
    }
    return out.getMemoryBlock();
}

bool unpackNotes(const juce::MemoryBlock& block, juce::Array<juce::var>& notes)
{
    juce::MemoryInputStream in(block, false);
    uint64_t size = 0;
    if (!getVarint(in, size) || size > block.getSize())
        return false;
    std::array<juce::int64, 3> before {};
    for (uint64_t k = 0; k < size; ++k)
    {
        uint64_t count = 0, raw = 0;
        if (!getVarint(in, count) || count > 16)
            return false;
        juce::Array<juce::var> note;
        for (uint64_t i = 0; i < count; ++i)
        {
            if (!getVarint(in, raw))
                return false;
            juce::int64 value = unzigzag(raw);
            if (i < 3)
                value = before[static_cast<size_t>(i)] += value;
            note.add(value);
        }
        notes.add(note);
    }
    return true;
}

// The tracks' notes swapped between arrays (JSON) and packed blocks (binary).
void packTracks(const juce::var& root, bool pack)
{
    if (const auto* tracks = root["tracks"].getArray())
        for (const auto& t : *tracks)
            if (auto* o = t.getDynamicObject())
            {
                if (pack && o->getProperty("notes").isArray())
                    o->setProperty("notes", packNotes(*o->getProperty("notes").getArray()));
                else if (!pack && o->getProperty("notes").isBinaryData())
                {
                    juce::Array<juce::var> notes;
                    if (unpackNotes(*o->getProperty("notes").getBinaryData(), notes))
                        o->setProperty("notes", notes);
                }
            }
}

// The project's tree, as JSON holds it.
juce::var tree(const Tune& tune, const juce::File& rip, const juce::File& to)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("format", kFormat);
    // A song written from empty has no rip.
    if (rip == juce::File())
        root->setProperty("rip", juce::var());
    else
    {
        auto* source = new juce::DynamicObject();
        source->setProperty("relative", rip.getRelativePathFrom(to.getParentDirectory()));
        source->setProperty("path", rip.getFullPathName());
        root->setProperty("rip", juce::var(source));
    }

    juce::Array<juce::var> tracks;
    for (const TuneTrack& t : tune.tracks)
    {
        auto* track = new juce::DynamicObject();
        track->setProperty("name", juce::String(t.name));
        track->setProperty("kind", t.kind == TuneTrack::Kind::Fm ? "fm" : "drum");
        track->setProperty("instrument", t.instrument);
        track->setProperty("mute", t.mute);
        track->setProperty("solo", t.solo);
        track->setProperty("volume", static_cast<double>(t.volume));
        track->setProperty("mutedVoices", static_cast<int>(t.mutedVoices));
        track->setProperty("soloedVoices", static_cast<int>(t.soloedVoices));
        track->setProperty("voiceVolume", floats(t.voiceVolume.data(), t.voiceVolume.size()));
        track->setProperty("addedVoices", static_cast<int>(t.addedVoices));
        track->setProperty("voiceSides", bytes(t.voiceSides.data(), t.voiceSides.size()));
        if (t.patch)
            track->setProperty("patch", instruments::toJson(*t.patch, t.name));
        // A drum track's sample edit, only the fields set.
        if (t.sample.any())
        {
            auto* sample = new juce::DynamicObject();
            const SampleEdit& e = t.sample;
            for (const auto& [key, field] : { std::pair { "start", e.start }, std::pair { "loopStart", e.loopStart } })
                if (field)
                    sample->setProperty(key, static_cast<int>(*field));
            for (const auto& [key, field] : { std::pair { "endPage", e.endPage }, std::pair { "step", e.step }, std::pair { "left", e.left },
                                              std::pair { "right", e.right } })
                if (field)
                    sample->setProperty(key, static_cast<int>(*field));
            if (e.loop)
                sample->setProperty("loop", *e.loop);
            track->setProperty("sample", juce::var(sample));
        }
        juce::Array<juce::var> notes;
        for (const TuneNote& n : t.notes)
        {
            // id, source, start, length, transpose, and the voice of a written
            // note or of a ripped one moved off the game's channel: compact, one
            // row a note. A note with expression adds its voice, -1 for a ripped
            // note on the game's channel, its velocity and fine tune, and its pan
            // when it has one.
            juce::Array<juce::var> note { n.id, n.source, static_cast<juce::int64>(n.start), static_cast<juce::int64>(n.length), n.transpose };
            const bool expressed = n.velocity != 0 || n.fine != 0 || n.pan.has_value();
            if (n.written() || n.voice >= 0 || expressed)
                note.add(n.voice);
            if (expressed)
            {
                note.add(n.velocity);
                note.add(n.fine);
            }
            if (n.pan)
                note.add(*n.pan);
            notes.add(note);
        }
        track->setProperty("notes", notes);
        tracks.add(juce::var(track));
    }
    root->setProperty("tracks", tracks);
    // The chip's edits, only the fields set.
    auto* chip = new juce::DynamicObject();
    const ChipEdits& c = tune.chip;
    for (const auto& [key, field] : { std::pair { "lfoRate", c.lfoRate }, std::pair { "lfoWave", c.lfoWave }, std::pair { "pmDepth", c.pmd },
                                      std::pair { "amDepth", c.amd }, std::pair { "noiseFrequency", c.noiseFreq } })
        if (field)
            chip->setProperty(key, static_cast<int>(*field));
    if (c.noise)
        chip->setProperty("noise", *c.noise);
    root->setProperty("chip", juce::var(chip));
    // The beat grid and meter, and the loop.
    auto* grid = new juce::DynamicObject();
    grid->setProperty("beatLength", tune.grid.beatLength);
    grid->setProperty("firstBeat", tune.grid.firstBeat);
    grid->setProperty("beatsPerBar", tune.grid.beatsPerBar);
    root->setProperty("grid", juce::var(grid));
    auto* loop = new juce::DynamicObject();
    loop->setProperty("start", static_cast<juce::int64>(tune.loop.start));
    loop->setProperty("end", static_cast<juce::int64>(tune.loop.end));
    loop->setProperty("on", tune.loop.on);
    root->setProperty("loop", juce::var(loop));
    return juce::var(root);
}

// A VGM rip as a tune, as it is.
std::unique_ptr<Tune> loadRip(const juce::File& file, juce::String& error)
{
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes))
    {
        error = "Cannot read " + file.getFullPathName();
        return nullptr;
    }
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    std::string message;
    auto tune = Tune::load({ begin, begin + bytes.getSize() }, file.getFileNameWithoutExtension().toStdString(), message);
    if (!tune)
        error = message;
    return tune;
}

} // namespace

bool save(const Tune& tune, const juce::File& rip, const juce::File& to, juce::String& error)
{
    const juce::var root = tree(tune, rip, to);
    packTracks(root, true);
    juce::MemoryOutputStream out;
    out.write(kMagic, sizeof(kMagic));
    out.writeByte(static_cast<char>(kFormat));
    {
        juce::GZIPCompressorOutputStream zip(out, 9);
        encode(zip, root);
    }
    if (!to.replaceWithData(out.getData(), out.getDataSize()))
    {
        error = "Could not write " + to.getFullPathName();
        return false;
    }
    return true;
}

bool exportJson(const Tune& tune, const juce::File& rip, const juce::File& to, juce::String& error)
{
    if (!to.replaceWithText(juce::JSON::toString(tree(tune, rip, to))))
    {
        error = "Could not write " + to.getFullPathName();
        return false;
    }
    return true;
}

bool isBinary(const juce::File& file)
{
    char head[sizeof(kMagic)] {};
    juce::FileInputStream in(file);
    return in.openedOk() && in.read(head, sizeof(head)) == sizeof(head) && std::memcmp(head, kMagic, sizeof(head)) == 0;
}

std::optional<Contents> read(const juce::File& from, juce::String& error)
{
    juce::var root;
    if (isBinary(from))
    {
        juce::MemoryBlock bytes;
        from.loadFileAsData(bytes);
        const auto header = sizeof(kMagic) + 1;
        if (bytes.getSize() > header && static_cast<uint8_t>(bytes[sizeof(kMagic)]) == kFormat)
        {
            juce::MemoryInputStream raw(static_cast<const char*>(bytes.getData()) + header, bytes.getSize() - header, false);
            juce::GZIPDecompressorInputStream zip(raw);
            juce::MemoryBlock unzipped;
            zip.readIntoMemoryBlock(unzipped);
            juce::MemoryInputStream in(unzipped, false);
            if (!decode(in, root) || !in.isExhausted())
                root = juce::var();
            packTracks(root, false);
        }
    }
    else
        root = juce::JSON::parse(from);
    if (!root.isObject() || static_cast<int>(root["format"]) != kFormat || root["tracks"].getArray() == nullptr)
    {
        error = from.getFileName() + " is not an OPM86 project";
        return std::nullopt;
    }
    Contents c;
    // The rip beside the project first, as a folder moved whole keeps it; else where it was.
    const juce::var rip = root["rip"];
    if (rip.isObject())
    {
        const juce::File near = from.getParentDirectory().getChildFile(rip["relative"].toString());
        c.rip = near.existsAsFile() ? near : juce::File(rip["path"].toString());
    }
    if (rip.isObject() && !c.rip.existsAsFile())
    {
        error = "Cannot find its rip, " + rip["path"].toString();
        return std::nullopt;
    }
    for (const juce::var& v : *root["tracks"].getArray())
    {
        TuneTrack t;
        t.name = v["name"].toString().toStdString();
        t.kind = v["kind"].toString() == "drum" ? TuneTrack::Kind::Drum : TuneTrack::Kind::Fm;
        t.instrument = static_cast<int>(v["instrument"]);
        t.mute = static_cast<bool>(v["mute"]);
        t.solo = static_cast<bool>(v["solo"]);
        t.volume = static_cast<float>(static_cast<double>(v["volume"]));
        t.mutedVoices = static_cast<uint16_t>(static_cast<int>(v["mutedVoices"]));
        t.soloedVoices = static_cast<uint16_t>(static_cast<int>(v["soloedVoices"]));
        readArray(v["voiceVolume"], t.voiceVolume);
        t.addedVoices = static_cast<uint8_t>(static_cast<int>(v["addedVoices"]));
        readArray(v["voiceSides"], t.voiceSides);
        if (v.hasProperty("patch"))
            if (const auto patch = instruments::fromJson(v["patch"]))
                t.patch = *patch;
        // Each sample field within its register's range; one missing, as an
        // older project has them all, keeps the game's.
        if (const juce::var sample = v["sample"]; sample.isObject())
        {
            auto field = [&sample](const char* key, int high) {
                return sample.hasProperty(key) ? std::optional<int>(juce::jlimit(0, high, static_cast<int>(sample[key]))) : std::nullopt;
            };
            auto narrow = [](std::optional<int> x, auto type) {
                return x ? std::optional<decltype(type)>(static_cast<decltype(type)>(*x)) : std::nullopt;
            };
            t.sample.start = narrow(field("start", 0xFFFF), uint16_t {});
            t.sample.loopStart = narrow(field("loopStart", 0xFFFF), uint16_t {});
            t.sample.endPage = narrow(field("endPage", 0xFF), uint8_t {});
            t.sample.step = narrow(field("step", 0xFF), uint8_t {});
            t.sample.left = narrow(field("left", 127), uint8_t {});
            t.sample.right = narrow(field("right", 127), uint8_t {});
            if (sample.hasProperty("loop"))
                t.sample.loop = static_cast<bool>(sample["loop"]);
        }
        const bool drum = t.kind == TuneTrack::Kind::Drum;
        if (const auto* notes = v["notes"].getArray())
            for (const juce::var& n : *notes)
                if (const auto* f = n.getArray(); f != nullptr && f->size() >= 5 && f->size() <= 9)
                {
                    // With no voice, as an older project has its ripped notes, a
                    // ripped note plays on the game's channel; a written one on V1.
                    // A voice of -1 is the game's channel for a ripped FM note.
                    // A written drum hit's channel is 0-15, or -1 for any free one.
                    TuneNote note;
                    note.id = static_cast<int>((*f)[0]);
                    note.source = static_cast<int>((*f)[1]);
                    note.start = static_cast<int64_t>((*f)[2]);
                    note.length = static_cast<int64_t>((*f)[3]);
                    note.transpose = static_cast<int>((*f)[4]);
                    const int voice = f->size() >= 6 ? static_cast<int>((*f)[5]) : -1;
                    note.voice = drum                           ? juce::jlimit(-1, 15, voice)
                                 : voice >= 0                   ? juce::jlimit(0, 7, voice)
                                 : note.source < 0 ? 0 : -1;
                    // The expression, as a project before it has none: as played.
                    if (f->size() >= 8)
                    {
                        note.velocity = juce::jlimit(TuneNote::kMinVelocity, 0, static_cast<int>((*f)[6]));
                        note.fine = juce::jlimit(-TuneNote::kMaxFine, TuneNote::kMaxFine, static_cast<int>((*f)[7]));
                    }
                    if (f->size() == 9)
                        note.pan = juce::jlimit(-TuneNote::kPanRange, TuneNote::kPanRange, static_cast<int>((*f)[8]));
                    t.notes.push_back(note);
                }
        c.tracks.push_back(std::move(t));
    }
    // Each chip field within its register's range; one missing stays unset.
    const juce::var chip = root["chip"];
    auto field = [&chip](const char* key, int high) {
        return chip.hasProperty(key) ? std::optional<uint8_t>(static_cast<uint8_t>(juce::jlimit(0, high, static_cast<int>(chip[key])))) : std::nullopt;
    };
    c.chip.lfoRate = field("lfoRate", 255);
    c.chip.lfoWave = field("lfoWave", 3);
    c.chip.pmd = field("pmDepth", 127);
    c.chip.amd = field("amDepth", 127);
    c.chip.noiseFreq = field("noiseFrequency", 31);
    if (chip.hasProperty("noise"))
        c.chip.noise = static_cast<bool>(chip["noise"]);
    // The grid within the tempo's and meter's ranges; with no beat length,
    // as an older project has it, the tune keeps its own.
    const juce::var grid = root["grid"];
    if (grid.hasProperty("beatLength"))
    {
        SongGrid g;
        const double slowest = 60.0 * kVgmRate / SongGrid::kMinTempo, fastest = 60.0 * kVgmRate / SongGrid::kMaxTempo;
        g.beatLength = juce::jlimit(fastest, slowest, static_cast<double>(grid["beatLength"]));
        g.firstBeat = static_cast<double>(grid["firstBeat"]);
        g.beatsPerBar = grid.hasProperty("beatsPerBar") ? juce::jlimit(1, SongGrid::kMaxBeatsPerBar, static_cast<int>(grid["beatsPerBar"])) : 4;
        c.grid = g;
    }
    const juce::var loop = root["loop"];
    c.loop.start = std::max<int64_t>(0, static_cast<juce::int64>(loop["start"]));
    c.loop.end = std::max<int64_t>(0, static_cast<juce::int64>(loop["end"]));
    c.loop = c.loop.fitted();
    c.loop.on = static_cast<bool>(loop["on"]) && c.loop.valid();
    return c;
}

bool fits(const Tune& tune, const std::vector<TuneTrack>& tracks, juce::String& error)
{
    for (const TuneTrack& t : tracks)
    {
        const bool fm = t.kind == TuneTrack::Kind::Fm;
        const size_t instruments = fm ? tune.patches().size() : tune.samples().size();
        const size_t sources = fm ? tune.log().notes.size() : tune.log().hits.size();
        // A track added in the tracker has no instrument of the rip's; only an FM one, so far.
        const bool added = fm && t.instrument == -1;
        if (!added && (t.instrument < 0 || static_cast<size_t>(t.instrument) >= instruments))
        {
            error = "Track " + juce::String(t.name) + " has an instrument the rip does not";
            return false;
        }
        for (const TuneNote& n : t.notes)
            if ((n.written() ? n.source != -1 : static_cast<size_t>(n.source) >= sources) || n.length < 1)
            {
                error = "Track " + juce::String(t.name) + " has a note the rip does not";
                return false;
            }
    }
    return true;
}

std::unique_ptr<Tune> load(const juce::File& from, juce::File& rip, juce::String& error)
{
    const auto contents = read(from, error);
    if (!contents)
        return nullptr;
    // The rip as it is, to check the project's tracks against before taking
    // them; a song written from empty has none.
    rip = contents->rip;
    std::unique_ptr<Tune> tune = rip == juce::File() ? Tune::empty() : loadRip(rip, error);
    if (!tune || !fits(*tune, contents->tracks, error))
        return nullptr;
    // An edited instrument over the rip's: the project keeps the patch's own
    // settings, and the song's LFO and output sides stay.
    auto tracks = contents->tracks;
    for (auto& t : tracks)
        if (t.patch)
        {
            const FmPatch& base = tune->basePatch(t);
            FmPatch p = base;
            p.alg = t.patch->alg, p.fb = t.patch->fb, p.pms = t.patch->pms, p.ams = t.patch->ams, p.op = t.patch->op;
            t.patch = p == base ? std::nullopt : std::optional<FmPatch>(p);
        }
    // A sample edit inside its sample's ROM range.
    for (auto& t : tracks)
        if (const PadSample* sample = tune->sample(t))
            t.sample = t.sample.fitted(*sample);
        else
            t.sample = {};
    tune->tracks = std::move(tracks);
    tune->chip = contents->chip;
    if (contents->grid)
        tune->grid = *contents->grid;
    tune->loop = contents->loop;
    return tune;
}

std::unique_ptr<Tune> open(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = "Cannot read " + file.getFullPathName();
        return nullptr;
    }
    if (file.hasFileExtension(kExtension) || file.hasFileExtension(".json") || isBinary(file))
    {
        juce::File rip;
        return load(file, rip, error);
    }
    return loadRip(file, error);
}

} // namespace opm86::project
