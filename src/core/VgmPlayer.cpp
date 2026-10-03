#include "VgmPlayer.h"

#include "SongPlayer.h"
#include "VgmSong.h"

namespace opm86
{

VgmRender renderVgm(const std::vector<uint8_t>& file, double sampleRate, double tailSeconds)
{
    VgmRender out;
    const auto song = loadVgmSong(file, "", out.error);
    if (!song)
        return out;

    SongPlayer player;
    player.prepare(sampleRate);
    player.setSong(song, makeStartSnapshot(*song));
    player.play(0.0);

    const auto total = static_cast<size_t>((song->length / kVgmRate + tailSeconds) * sampleRate);
    out.left.assign(total, 0.0f);
    out.right.assign(total, 0.0f);
    constexpr size_t kBlock = 4096;
    for (size_t pos = 0; pos < total; pos += kBlock)
    {
        const auto n = static_cast<int>(std::min(kBlock, total - pos));
        player.render(&out.left[pos], &out.right[pos], n);
    }
    return out;
}

} // namespace opm86
