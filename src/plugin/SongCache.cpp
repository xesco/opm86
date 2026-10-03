#include "SongCache.h"

#include <map>
#include <mutex>

namespace opm86
{

namespace
{

struct Entry
{
    std::shared_ptr<const LoadedSong> song;
    bool started = false;
};

std::mutex& lock()
{
    static std::mutex m;
    return m;
}

std::map<juce::String, Entry>& entries()
{
    static std::map<juce::String, Entry> e;
    return e;
}

// One background thread; its destructor waits for a load in progress
// when the plugin is unloaded.
juce::ThreadPool& pool()
{
    static juce::ThreadPool p(1);
    return p;
}

} // namespace

std::shared_ptr<const LoadedSong> SongCache::get(const juce::File& file)
{
    const juce::String key = file.getFullPathName();
    {
        const std::lock_guard<std::mutex> guard(lock());
        Entry& entry = entries()[key];
        if (entry.song || entry.started)
            return entry.song;
        entry.started = true;
    }

    pool().addJob([file, key] {
        juce::MemoryBlock bytes;
        std::shared_ptr<LoadedSong> loaded;
        if (file.loadFileAsData(bytes))
        {
            const auto* begin = static_cast<const uint8_t*>(bytes.getData());
            std::string error;
            auto song = loadVgmSong({ begin, begin + bytes.getSize() },
                                    file.getFileNameWithoutExtension().toStdString(), error);
            if (song)
            {
                loaded = std::make_shared<LoadedSong>();
                loaded->song = song;
                loaded->snapshots = makeSnapshots(*song);
            }
        }
        const std::lock_guard<std::mutex> guard(lock());
        entries()[key].song = loaded;
    });
    return nullptr;
}

} // namespace opm86
