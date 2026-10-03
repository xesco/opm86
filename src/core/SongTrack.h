#pragma once

#include "SongPlayer.h"
#include "VgmSong.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opm86
{

// A song track is a MIDI track that drives a song player: one note per
// beat of the song, each meaning "play this beat". Played in order, the
// song plays straight through; moved, cut or repeated, it plays the song
// rearranged, every beat exactly as the game plays it.
//
// The track starts with controllers that set up the player:
//   CC 86       the stem: 0 the whole song, 1-8 a YM2151 channel, 9 the drums
//   CC 87-90    the song: songId() in four 7-bit parts, high part first
// A beat note's number and velocity hold the beat: beat = note + 128 x
// (velocity - 1) - 1, so beat -1, the lead-in before the first beat, is
// note 0 at velocity 1.
inline constexpr int kStemController = 86;
inline constexpr int kSongIdController = 87;
inline constexpr int kStemDrums = 9;

void beatToNote(int beat, uint8_t& note, uint8_t& velocity);
int beatFromNote(int note, int velocity);
void stemChannels(int stem, uint8_t& fm, uint16_t& pcm);
std::string stemName(int stem);

struct SongTrackFile
{
    std::vector<uint8_t> file;
    std::vector<int> stems; // one track per stem, in file order
    double tempo = 120.0;   // BPM written to the file
};

// One track per stem the song uses, or one track for the whole song.
// Before the first beat comes one bar of lead-in, so the song's beats fall
// on the bars of a music program set to the file's tempo.
SongTrackFile writeSongTrack(const VgmSong& song, bool stems);

// Plays a song track: reads its controllers and beat notes. A beat that
// follows the one playing within a quarter beat continues without a jump,
// so a host tempo a little off the song's does not cause jumps; any other
// beat jumps there with a short crossfade. With no beat note held, the
// player stops. Audio-thread safe.
class SongTrackPlayer
{
public:
    SongPlayer& player() { return player_; }

    void handleMidi(const uint8_t* data, int size);
    void endOfEvents(); // after all the messages at one point in time

    // A song asked for by the track's controllers, 0 if none.
    uint32_t requestedSong() const { return idParts_ == 0x0F ? requestedId_ : 0; }
    int stem() const { return stem_; }
    void setStem(int stem);

private:
    SongPlayer player_;
    int held_ = 0;
    int stem_ = 0;
    uint32_t requestedId_ = 0;
    int idParts_ = 0; // bit n: part n received
};

} // namespace opm86
