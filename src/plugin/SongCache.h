#pragma once

#include "SongPlayer.h"
#include "VgmSong.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace opm86
{

// A song ready for exact playback.
struct LoadedSong
{
    std::shared_ptr<const VgmSong> song;
    std::shared_ptr<const SongSnapshots> snapshots;
};

// Songs loaded once for every plugin instance in the process, so the
// tracks of one arrangement share a song and its snapshots. Message thread.
class SongCache
{
public:
    // The song in this file if it is ready; otherwise nullptr, and loading
    // starts in the background. A file that cannot be read stays nullptr.
    static std::shared_ptr<const LoadedSong> get(const juce::File& file);
};

} // namespace opm86
