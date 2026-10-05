#include "AppModel.h"

#include "Project.h"
#include "SongExport.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{
// Whether two sets of tracks build the same song: they differ at most in
// mute, solo, voice mutes and volume, which the player applies. The chip's
// edits are not in the tracks: the player applies them too. A sample edit
// changes the drums' writes, so it needs a new build, and so does a new
// connection in the patch of a track with velocity: the build lowers the
// carriers of the connection the player sounds.
bool sameBuild(const std::vector<TuneTrack>& a, const std::vector<TuneTrack>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t t = 0; t < a.size(); ++t)
    {
        const TuneTrack &x = a[t], &y = b[t];
        if (x.kind != y.kind || x.instrument != y.instrument || x.name != y.name || x.sample != y.sample || x.notes.size() != y.notes.size())
            return false;
        for (size_t n = 0; n < x.notes.size(); ++n)
        {
            const TuneNote &p = x.notes[n], &q = y.notes[n];
            if (p.id != q.id || p.source != q.source || p.start != q.start || p.length != q.length || p.transpose != q.transpose || p.voice != q.voice
                || p.velocity != q.velocity || p.fine != q.fine || p.pan != q.pan)
                return false;
        }
        auto connection = [](const TuneTrack& track) { return track.patch ? track.patch->alg : -1; };
        if (connection(x) != connection(y) && std::any_of(x.notes.begin(), x.notes.end(), [](const TuneNote& n) { return n.velocity != 0; }))
            return false;
    }
    return true;
}

// Frees the kept objects that only the model still holds. The audio thread
// takes its pointers from the commands, the player or a deck, each with its
// own reference, so one held only here never reaches it again. One a stopped
// deck still holds stays, however old, so the audio thread never frees it.
template <typename T>
void freeUnused(std::deque<std::shared_ptr<const T>>& kept)
{
    kept.erase(std::remove_if(kept.begin(), kept.end(), [](const auto& p) { return p.use_count() == 1; }), kept.end());
}
} // namespace

struct AppModel::TuneState
{
    std::vector<TuneTrack> tracks;
    ChipEdits chip;
    SongGrid grid;
    std::shared_ptr<const SampleSet> samples; // shared, as the tune shares them
};

// One undoable edit: the tune's state before and after it.
class AppModel::TracksChange : public juce::UndoableAction
{
public:
    TracksChange(AppModel& model, TuneState before, TuneState after) : model_(model), before_(std::move(before)), after_(std::move(after)) {}

    bool perform() override
    {
        model_.setTracks(after_);
        return true;
    }

    bool undo() override
    {
        model_.setTracks(before_);
        return true;
    }

    int getSizeInUnits() override
    {
        size_t notes = 0;
        for (const auto& t : before_.tracks)
            notes += t.notes.size();
        // A step that loads samples keeps a set of its own, every sample's
        // bytes in it, counted as so many notes' worth of memory.
        if (after_.samples && after_.samples != before_.samples)
            for (const PadSample& s : after_.samples->samples)
                notes += s.data.size() / sizeof(TuneNote);
        return static_cast<int>(notes / 50 + 1);
    }

private:
    AppModel& model_;
    TuneState before_, after_;
};

// A WAV render on a thread of its own, into a temporary file that takes
// the place of the target when the render is complete.
class AppModel::WavExport : public juce::Thread
{
public:
    WavExport(const Tune& tune, const juce::File& file, const WavOptions& options)
        : juce::Thread("WAV export"), file_(file), options_(options), renderer_(tune, options.rate, options.loopOnly)
    {
    }
    ~WavExport() override { stopThread(-1); }

    const juce::File& file() const { return file_; }
    double progress() const { return progress_.load(); }
    bool finished() const { return finished_.load(); }
    const juce::String& result() const { return result_; } // once finished

    void run() override
    {
        result_ = render();
        finished_.store(true);
    }

private:
    juce::String render()
    {
        if (!renderer_.prepare([this] { return !threadShouldExit(); }))
            return "Export cancelled";
        juce::TemporaryFile temporary(file_);
        WavWriter wav;
        if (!wav.open(temporary.getFile().getFullPathName().toStdString(), static_cast<uint32_t>(options_.rate), options_.bits))
            return "Could not write " + file_.getFullPathName();
        std::vector<float> left(4096), right(4096);
        while (const int n = renderer_.render(left.data(), right.data(), static_cast<int>(left.size())))
        {
            if (threadShouldExit())
                return "Export cancelled";
            wav.write(left.data(), right.data(), n);
            progress_.store(renderer_.progress());
        }
        const auto seconds = static_cast<int>(wav.frames() / options_.rate);
        if (!wav.close() || !temporary.overwriteTargetFileWithTemporary())
            return "Could not write " + file_.getFullPathName();
        return "Exported " + file_.getFileName() + (renderer_.loop() ? ", the loop: " : ": ") + juce::String(seconds / 60) + ":"
               + juce::String(seconds % 60).paddedLeft('0', 2) + ", " + juce::String(options_.bits) + "-bit, "
               + juce::String(options_.rate / 1000.0, 1) + " kHz";
    }

    juce::File file_;
    WavOptions options_;
    TuneRenderer renderer_;
    std::atomic<double> progress_ { 0.0 };
    std::atomic<bool> finished_ { false };
    juce::String result_;
};

AppModel::AppModel()
{
    // Mute, solo and volume act at once in the player, with no new song to build.
    rebuilder_.setMixInPlayer(true);
    devices_.initialiseWithDefaultDevices(0, 2);
    devices_.addAudioCallback(this);
    startTimerHz(30);
}

AppModel::~AppModel()
{
    wavExport_.reset(); // the render stops first
    devices_.removeAudioCallback(this);
}

bool AppModel::exportVgm(const juce::File& file, juce::String& error) const
{
    if (!tune_)
        return false;
    const auto song = bakedSong(*tune_);
    const auto bytes = writeVgm(*song, title().toStdString(), !file.hasFileExtension(".vgm"));
    if (bytes.empty() || !file.replaceWithData(bytes.data(), bytes.size()))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool AppModel::exportMidi(const juce::File& file, juce::String& error) const
{
    if (!tune_)
        return false;
    // Named after the rip, as the plugin finds a song's patches and drums by its name.
    const MidiExport midi = opm86::exportMidi(*tune_, tune_->source().name);
    if (!file.replaceWithData(midi.file.data(), midi.file.size()))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool AppModel::exportWav(const juce::File& file, const WavOptions& options)
{
    if (!tune_ || wavExport_)
        return false;
    wavExport_ = std::make_unique<WavExport>(*tune_, file, options);
    wavExport_->startThread();
    return true;
}

double AppModel::exportProgress() const
{
    return wavExport_ ? wavExport_->progress() : 0.0;
}

juce::File AppModel::exportFile() const
{
    return wavExport_ ? wavExport_->file() : juce::File();
}

void AppModel::cancelExport()
{
    if (wavExport_)
        wavExport_->signalThreadShouldExit();
}

std::optional<juce::String> AppModel::takeExportResult()
{
    if (!wavExport_ || !wavExport_->finished())
        return std::nullopt;
    const juce::String result = wavExport_->result();
    wavExport_.reset();
    return result;
}

bool AppModel::open(const juce::File& file, juce::String& error)
{
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes))
    {
        error = "Cannot read " + file.getFullPathName();
        return false;
    }
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    std::string message;
    auto tune = Tune::load({ begin, begin + bytes.getSize() }, file.getFileNameWithoutExtension().toStdString(), message);
    if (!tune)
    {
        error = message;
        return false;
    }
    take(std::move(tune), file);
    return true;
}

void AppModel::newProject()
{
    take(Tune::empty(), juce::File());
}

void AppModel::take(std::unique_ptr<Tune> tune, const juce::File& rip)
{
    stop();
    tune_ = std::move(tune);
    ++revision_;
    file_ = rip;
    undo_.clearUndoHistory();
    built_ = nullptr;
    report_ = {};
    position_.store(0.0);
    project_ = juce::File();
    imported_ = juce::File();
    loopChanged_ = false;
    auditionTrack_ = previewTrack_ = -1;
    auditionOps_ = 0;
    remaps_.clear();
    rebuild();
    sendMix();
    sendLoop();
    sendPreviewSong();
    savedRevision_ = revision_;
    sendChangeMessage();
}

bool AppModel::openProject(const juce::File& file, juce::String& error)
{
    juce::File rip;
    auto tune = project::load(file, rip, error);
    if (!tune)
        return false;
    take(std::move(tune), rip);
    // JSON under another name is imported: a save asks where, as binary.
    if (project::isBinary(file) || file.hasFileExtension(project::kExtension))
        project_ = file;
    else
        imported_ = file;
    savedRevision_ = revision_;
    sendChangeMessage();
    return true;
}

bool AppModel::exportJson(const juce::File& file, juce::String& error)
{
    return tune_ != nullptr && project::exportJson(*tune_, file_, file, error);
}

bool AppModel::saveProject(const juce::File& file, juce::String& error)
{
    if (!tune_)
        return false;
    if (!project::save(*tune_, file_, file, error))
        return false;
    project_ = file;
    imported_ = juce::File();
    savedRevision_ = revision_;
    loopChanged_ = false;
    sendChangeMessage();
    return true;
}

bool AppModel::edit(const juce::String& name, const std::function<void(Tune&)>& change, bool newStep)
{
    if (!tune_)
        return false;
    Tune changed = *tune_;
    change(changed);
    if (newStep)
        undo_.beginNewTransaction(name);
    undo_.perform(new TracksChange(*this, { tune_->tracks, tune_->chip, tune_->grid, tune_->sampleSet() },
                                   { std::move(changed.tracks), changed.chip, changed.grid, changed.sampleSet() }));
    return true;
}

bool AppModel::setTempo(double bpm, bool newStep)
{
    if (!tune_)
        return false;
    // A rip's grid alone, or a written song's notes with it.
    return edit(tune_->ripped() ? "Tempo" : "Change tempo", [bpm](Tune& tune) { tune.setTempo(bpm); }, newStep);
}

bool AppModel::setBeatsPerBar(int beats, bool newStep)
{
    return edit("Beats per bar", [beats](Tune& tune) { tune.grid.beatsPerBar = std::clamp(beats, 1, SongGrid::kMaxBeatsPerBar); }, newStep);
}

bool AppModel::barOneHere()
{
    const double at = position_.load();
    return edit("Bar 1 here", [at](Tune& tune) { tune.grid.firstBeat = at; });
}

void AppModel::setLoop(const LoopRegion& loop)
{
    if (!tune_)
        return;
    tune_->loop = loop.fitted();
    loopChanged_ = true;
    sendLoop();
    sendChangeMessage();
}

void AppModel::sendLoop()
{
    // The player still plays a song in the time before a tempo change: the
    // loop waits for the song in its own time.
    if (!remaps_.empty())
    {
        remaps_.back().loop = tune_->loop;
        return;
    }
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    loopCommand(tune_->loop);
}

void AppModel::loopCommand(const LoopRegion& loop)
{
    commands_.loop = true;
    commands_.loopStart = static_cast<double>(loop.start);
    commands_.loopEnd = loop.active() ? static_cast<double>(loop.end) : 0.0;
}

bool AppModel::undoStep(bool redo)
{
    return redo ? undo_.redo() : undo_.undo();
}

void AppModel::setTracks(const TuneState& state)
{
    // Mute, solo and volume alone change only the mix, and the chip's edits
    // go to the player with it: no new song to build. The grid changes no
    // write, unless it makes a song with no rip longer or shorter. A sample
    // loaded changes the ROM, and the drums' writes with it.
    const int64_t length = tune_->length();
    const bool notes = !sameBuild(tune_->tracks, state.tracks);
    const bool samples = state.samples != tune_->sampleSet();
    // A new tempo in a song with no rip moves the notes, and the loop keeps
    // its beats with them, on the edit, its undo and its redo alike.
    const bool tempo = !tune_->ripped() && !juce::exactlyEqual(state.grid.beatLength, tune_->grid.beatLength);
    const TimeMap moved = tempo ? TimeMap::scaled(state.grid.firstBeat, state.grid.beatLength / tune_->grid.beatLength) : TimeMap {};
    const bool rescale = tempo && tune_->loop.valid();
    if (rescale)
    {
        LoopRegion& loop = tune_->loop;
        loop.start = std::max<int64_t>(0, std::llround(moved(static_cast<double>(loop.start))));
        loop.end = std::max<int64_t>(loop.start + 1, std::llround(moved(static_cast<double>(loop.end))));
        loop = loop.fitted();
    }
    tune_->tracks = state.tracks;
    tune_->chip = state.chip;
    tune_->grid = state.grid;
    tune_->setSampleSet(state.samples);
    const bool build = notes || samples || tune_->length() != length;
    ++revision_;
    if (build)
    {
        // Playing, the new song plays on from the same beat once it is built,
        // its first snapshots made from there.
        const bool remap = tempo && playing();
        const uint64_t request = rebuild(remap ? moved : TimeMap {});
        if (remap)
            remaps_.push_back({ request, moved, std::nullopt });
    }
    if (rescale)
        sendLoop();
    if (samples)
        sendPreviewSong();
    // A new song brings the mix for its own channels when it is built; until
    // then the player keeps the one for the song it plays.
    if (!build)
        sendMix();
    sendChangeMessage();
}

void AppModel::setProbe(int voices, int track)
{
    probeVoices_ = voices;
    probeTrack_ = track;
    std::shared_ptr<const ChannelMix> owner;
    if (tune_ && voices != 0 && track >= 0)
    {
        owner = ownerOf(*tune_, report_, track);
        mixes_.push_back(owner); // kept here, so the audio thread never frees one
        freeUnused(mixes_);
    }
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    commands_.probe = true;
    commands_.probeVoices = voices;
    commands_.probeOwner = owner;
}

std::pair<std::shared_ptr<const ChannelMix>, std::shared_ptr<const PatchEdits>> AppModel::mixNow()
{
    std::shared_ptr<const ChannelMix> mix = mixOf(*tune_, report_);
    // The edited instruments, by the same channels.
    std::shared_ptr<const PatchEdits> edits = patchEditsOf(*tune_, report_, auditionTrack_, auditionOps_);
    mixes_.push_back(mix);
    freeUnused(mixes_);
    if (edits)
    {
        edits_.push_back(edits);
        freeUnused(edits_);
    }
    return { std::move(mix), std::move(edits) };
}

void AppModel::sendMix()
{
    auto [mix, edits] = mixNow();
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.mix = std::move(mix);
        commands_.edits = std::move(edits);
        commands_.newEdits = true;
    }
    // The probe's owner follows the notes and their channels too.
    if (probeVoices_ != 0)
        setProbe(probeVoices_, probeTrack_);
}

void AppModel::sendPreviewSong()
{
    // The preview's chips run as the song's do; its SegaPCM reads the tune's
    // ROM, with the samples loaded into it.
    std::shared_ptr<const VgmSong> song = tune_->sourcePointer();
    if (tune_->sampleSet())
    {
        auto loaded = std::make_shared<VgmSong>();
        loaded->header = song->header;
        loaded->fmRate = song->fmRate;
        loaded->pcmRate = song->pcmRate;
        loaded->rom = tune_->rom();
        song = std::move(loaded);
    }
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.previewChip = true;
        commands_.previewRate = tune_->source().fmRate;
        commands_.previewShift = tune_->clockShift();
        commands_.previewSong = song;
        commands_.previewPcmCount = 0;
    }
    // Kept here, so the audio thread never frees one.
    previewSongs_.push_back(std::move(song));
    freeUnused(previewSongs_);
}

const FmPatch& AppModel::editedPatch(int track) const
{
    const TuneTrack& tr = tune_->tracks[static_cast<size_t>(track)];
    return tr.patch ? *tr.patch : tune_->basePatch(tr);
}

ChipState AppModel::gameChip() const
{
    if (playing() || !built_ || keep_.empty())
        return player_.gameChip();
    // Stopped, from the newest build's snapshots, again whenever the place or the build changes.
    const double at = position_.load();
    if (chipSong_ != built_ || std::abs(chipAt_ - at) > 1e-9)
    {
        chipSong_ = built_;
        chipAt_ = at;
        chipState_ = chipAt(*built_, *keep_.back().snapshots, at);
    }
    return chipState_;
}

void AppModel::setAudition(int track, uint8_t operators)
{
    operators = static_cast<uint8_t>(operators & 0xF);
    auditionTrack_ = operators != 0 ? track : -1;
    auditionOps_ = auditionTrack_ >= 0 ? operators : 0;
    if (!tune_)
        return;
    sendMix();
    // A preview of the track sounding now takes the change at once.
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    commands_.previewMute = true;
    commands_.previewMuted = audition(previewTrack_);
}

void AppModel::previewPatch(const FmPatch& patch, int track)
{
    previewTrack_ = track;
    // With the chip's LFO as the song has it now, as every voice shares it.
    FmPatch heard = patch;
    if (tune_)
    {
        const ChipState chip = chipNow();
        heard.lfoFreq = chip.lfoRate, heard.lfoWave = chip.lfoWave, heard.pmd = chip.pmd, heard.amd = chip.amd;
    }
    auto shared = std::make_shared<const FmPatch>(heard);
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.previewPatch = shared;
        commands_.previewMute = true;
        commands_.previewMuted = audition(track);
    }
    // Kept here, so the audio thread never frees one.
    previewPatches_.push_back(std::move(shared));
    freeUnused(previewPatches_);
}

void AppModel::sendPreviewKey(int key, bool on, double seconds, int fine, int quieter, uint8_t sides)
{
    // A full queue, with no audio thread to take it, becomes a let go of
    // every key before this one: a key up is never lost, so no key stays down.
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    if (commands_.previewCount == commands_.previewKeys.size())
    {
        commands_.previewKeys[0] = { -1, false, 0.0, 0, 0, 0xC0 };
        commands_.previewCount = 1;
    }
    commands_.previewKeys[commands_.previewCount++] = { key, on, seconds, fine, quieter, sides };
}

void AppModel::previewKey(int key, bool on)
{
    // A key up always goes, so no key stays down.
    if (on && (playing() || !tune_))
        return;
    sendPreviewKey(key, on, 0.0);
}

void AppModel::previewOff()
{
    sendPreviewKey(-1, false, 0.0);
}

void AppModel::previewNote(int track, const TuneNote& note)
{
    if (!tune_ || playing() || track < 0 || static_cast<size_t>(track) >= tune_->tracks.size())
        return;
    const TuneTrack& tr = tune_->tracks[static_cast<size_t>(track)];
    if (tr.kind != TuneTrack::Kind::Fm)
        return;
    previewPatch(editedPatch(track), track);
    previewOff();
    // The key as the game or the tracker set it, and the fine tune on top.
    TuneNote key = note;
    key.fine = 0;
    sendPreviewKey(tune_->pitch(tr, key), true, std::min(static_cast<double>(note.length) / kVgmRate, kPreviewLength),
                   std::clamp(note.fine, -TuneNote::kMaxFine, TuneNote::kMaxFine), -std::clamp(note.velocity, TuneNote::kMinVelocity, 0),
                   note.pan ? TuneNote::sides(*note.pan) : uint8_t { 0xC0 });
}

void AppModel::previewHit(int track, const TuneNote& note)
{
    if (!tune_ || playing() || track < 0 || static_cast<size_t>(track) >= tune_->tracks.size())
        return;
    const TuneTrack& tr = tune_->tracks[static_cast<size_t>(track)];
    if (tr.kind != TuneTrack::Kind::Drum || tune_->sample(tr) == nullptr)
        return;
    Commands::PreviewPcm event;
    event.on = true;
    event.voice = tune_->hitVoice(tr, note, true);
    event.seconds = std::min(static_cast<double>(note.length) / kVgmRate, kPreviewLength);
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    if (commands_.previewPcmCount == commands_.previewPcm.size())
        commands_.previewPcmCount = 0;
    commands_.previewPcm[commands_.previewPcmCount++] = event;
}

void AppModel::previewSample(int track, int key, bool on)
{
    // A key up always goes, so no sample plays on.
    if (on && (!tune_ || playing() || track < 0 || static_cast<size_t>(track) >= tune_->tracks.size()
               || tune_->sample(tune_->tracks[static_cast<size_t>(track)]) == nullptr))
        return;
    Commands::PreviewPcm event;
    event.on = on;
    if (on)
        event.voice = tune_->sampleVoice(tune_->tracks[static_cast<size_t>(track)], key - 60);
    // A full queue, with no audio thread to take it, keeps only the newest.
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    if (commands_.previewPcmCount == commands_.previewPcm.size())
        commands_.previewPcmCount = 0;
    commands_.previewPcm[commands_.previewPcmCount++] = event;
}

double AppModel::playheadAhead(const TimeMap& next) const
{
    TimeMap map;
    for (const Remap& r : remaps_)
        map = map.then(r.map);
    return map.then(next)(position_.load());
}

uint64_t AppModel::rebuild(const TimeMap& next)
{
    const double at = playheadAhead(next);
    rebuilder_.setPlayhead(playing() ? at : -1.0);
    return rebuilder_.request(*tune_, at);
}

void AppModel::play()
{
    if (!built_)
        return;
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    commands_.play = true;
    commands_.stop = commands_.scrub = false;
    commands_.playAt = position_.load();
}

void AppModel::stop()
{
    // Stopped from now on for everyone here, while the audio thread fades out.
    playing_.store(false);
    const juce::SpinLock::ScopedLockType lock(commandLock_);
    commands_.stop = true;
    commands_.play = false;
}

double AppModel::displayPosition() const
{
    const double p = position_.load();
    if (!playing_.load())
        return p;
    // Up to 50 ms ahead of the last audio block, back round the loop as playback goes.
    const double elapsed = std::clamp(juce::Time::getMillisecondCounterHiRes() - positionStamp_.load(), 0.0, 50.0);
    const double ahead = p + elapsed * speed_.load() * kVgmRate / 1000.0;
    if (tune_ && tune_->loop.active())
    {
        const auto start = static_cast<double>(tune_->loop.start), end = static_cast<double>(tune_->loop.end);
        if (p < end && ahead >= end)
            return start + (ahead - end);
    }
    return ahead;
}

void AppModel::setPosition(double songTime)
{
    position_.store(std::max(0.0, songTime));
    if (playing_.load())
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.play = true;
        commands_.playAt = position_.load();
    }
    sendChangeMessage();
}

void AppModel::scrub(double songTime)
{
    position_.store(std::max(0.0, songTime));
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        commands_.play = commands_.scrub = true;
        commands_.playAt = position_.load();
    }
    sendChangeMessage();
}

void AppModel::timerCallback()
{
    rebuilder_.setPlayhead(playing() ? playheadAhead() : -1.0);
    auto version = rebuilder_.take();
    if (!version)
        return;
    report_ = version->report;
    ++reportRevision_;

    // Tell the player what stayed the same, compared with what it has. A
    // sample loaded since changes the ROM: what plays moves to the new one
    // with the crossfade.
    const bool same = built_ == version->song;
    size_t ym = 0, pcm = 0;
    if (built_ && !same)
    {
        firstDifference(*built_, *version->song, ym, pcm);
        if (built_->rom != version->song->rom)
            pcm = 0;
    }
    // The tempo changes the build has, how they move the playhead, and the
    // loop in its time.
    TimeMap moved;
    std::optional<LoopRegion> loop;
    for (; !remaps_.empty() && remaps_.front().request <= version->version; remaps_.pop_front())
    {
        moved = moved.then(remaps_.front().map);
        if (remaps_.front().loop)
            loop = remaps_.front().loop;
    }
    // The build may have moved notes to other channels: the mix for them
    // goes with the song, in the same block.
    auto [mix, edits] = mixNow();
    {
        const juce::SpinLock::ScopedLockType lock(commandLock_);
        // A song the audio thread has not taken yet: it still plays the one
        // before, so this one is told against that.
        const bool untaken = commands_.newSong;
        commands_.song = version->song;
        commands_.snapshots = version->snapshots;
        if (!same)
        {
            commands_.ymUnchanged = untaken ? std::min(commands_.ymUnchanged, ym) : ym;
            commands_.pcmUnchanged = untaken ? std::min(commands_.pcmUnchanged, pcm) : pcm;
            commands_.timeMap = untaken ? commands_.timeMap.then(moved) : moved;
            commands_.newSong = true;
        }
        commands_.sameSong = !commands_.newSong;
        commands_.fresh = commands_.fresh || built_ == nullptr;
        commands_.mix = std::move(mix);
        commands_.edits = std::move(edits);
        commands_.newEdits = true;
        // The audio thread sets the loop after the song.
        if (loop)
            loopCommand(*loop);
    }
    if (probeVoices_ != 0)
        setProbe(probeVoices_, probeTrack_);
    built_ = version->song;
    keep_.push_back(std::move(*version));
    // The newest stays, as the chip's state when stopped is read from it.
    keep_.erase(std::remove_if(keep_.begin(), keep_.end() - 1,
                               [](const TuneVersion& v) { return v.song.use_count() == 1 && v.snapshots.use_count() == 1; }),
                keep_.end() - 1);
    sendChangeMessage();
}

void AppModel::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate_ = device->getCurrentSampleRate();
    player_.prepare(sampleRate_);
    preview_.prepare(sampleRate_);
}

void AppModel::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                                int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    {
        const juce::SpinLock::ScopedTryLockType lock(commandLock_);
        if (lock.isLocked())
        {
            Commands& c = commands_;
            if (c.mix)
            {
                player_.setMix(c.mix);
                c.mix.reset();
            }
            if (c.newEdits)
            {
                player_.setPatchEdits(c.edits);
                c.edits.reset();
                c.newEdits = false;
            }
            if (c.probe)
            {
                player_.setProbe(c.probeVoices, c.probeOwner);
                c.probeOwner.reset();
                c.probe = false;
            }
            if (c.newSong || c.sameSong)
            {
                // Playing, a new song takes over at the playhead, or once its
                // snapshots reach it, the old one playing on meanwhile (see
                // SongPlayer::replaceSong).
                if (c.fresh || !player_.playing())
                    player_.setSong(c.song, c.snapshots);
                else if (c.newSong)
                    player_.replaceSong(c.song, c.snapshots, c.ymUnchanged, c.pcmUnchanged, c.timeMap);
                else
                    player_.updateSnapshots(c.snapshots);
                // The command keeps no reference: the model holds the version.
                c.song.reset();
                c.snapshots.reset();
                c.timeMap = {};
                c.newSong = c.sameSong = c.fresh = false;
            }
            if (c.previewChip)
            {
                preview_.setChip(c.previewRate, c.previewShift);
                // The command keeps no reference: the model holds the song.
                preview_.setPcm(std::move(c.previewSong));
                c.previewSong.reset();
                c.previewChip = false;
            }
            for (size_t i = 0; i < c.previewPcmCount; ++i)
            {
                if (c.previewPcm[i].on)
                    preview_.pcmOn(c.previewPcm[i].voice, c.previewPcm[i].seconds);
                else
                    preview_.pcmOff();
            }
            c.previewPcmCount = 0;
            if (c.previewMute)
            {
                preview_.setMuted(c.previewMuted);
                c.previewMute = false;
            }
            if (c.previewPatch)
            {
                // The command keeps no reference: the model holds the patch.
                preview_.setPatch(*c.previewPatch);
                c.previewPatch.reset();
            }
            for (size_t i = 0; i < c.previewCount; ++i)
            {
                const auto& k = c.previewKeys[i];
                if (k.on)
                    preview_.noteOn(k.key, k.seconds, k.fine, k.quieter, k.sides);
                else if (k.key < 0)
                    preview_.releaseAll();
                else
                    preview_.noteOff(k.key);
            }
            c.previewCount = 0;
            if (c.loop)
            {
                player_.setLoop(c.loopStart, c.loopEnd);
                c.loop = false;
            }
            if (c.stop)
            {
                player_.stop();
                playing_.store(false);
                c.stop = false;
            }
            if (c.play && c.scrub)
            {
                // 90 ms of sound from the scrub's time. Close to what sounds
                // already, the sound runs on rather than jump back and stutter.
                const bool near = player_.playing() && scrubLeft_ > 0 && std::abs(player_.position() - c.playAt) < 0.04 * kVgmRate;
                if (near || player_.play(c.playAt))
                    scrubLeft_ = static_cast<int>(0.09 * sampleRate_);
                c.play = c.scrub = false;
            }
            else if (c.play)
            {
                // The song takes over from the preview, which ends at once.
                preview_.stop();
                scrubLeft_ = 0;
                if (player_.play(c.playAt))
                    playing_.store(true);
                c.play = false;
            }
        }
    }

    player_.setSpeed(speed_.load());

    float scratch[2][1024];
    float* left = numOutputs > 0 && outputs[0] ? outputs[0] : scratch[0];
    float* right = numOutputs > 1 && outputs[1] ? outputs[1] : scratch[1];
    numSamples = std::min(numSamples, 1024);
    player_.render(left, right, numSamples);
    preview_.render(left, right, numSamples);
    if (scrubLeft_ > 0 && (scrubLeft_ -= numSamples) <= 0 && !playing_.load())
        player_.stop();
    if (!monitor_.load())
    {
        juce::FloatVectorOperations::clear(left, numSamples);
        juce::FloatVectorOperations::clear(right, numSamples);
    }
    for (int ch = 2; ch < numOutputs; ++ch)
        if (outputs[ch])
            juce::FloatVectorOperations::clear(outputs[ch], numSamples);

    if (player_.playing())
    {
        // While a stop fades out, the position is the one it stopped at, or
        // where it has been moved since.
        if (playing_.load())
        {
            position_.store(player_.position());
            positionStamp_.store(juce::Time::getMillisecondCounterHiRes());
        }
        // Stop after the song's end and its releases.
        if (const VgmSong* song = player_.song(); song && player_.position() > songEnd(*song))
        {
            player_.stop();
            playing_.store(false);
        }
    }
    else
        playing_.store(false);
}

} // namespace opm86
