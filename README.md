# OPM86

OPM86 is a software synthesizer that emulates the sound board of Out Run, the arcade game
Sega released in 1986. It plays the board's two sound chips from MIDI: a Yamaha YM2151 for
melody and bass, and a Sega PCM chip for drums. You can use it as a command-line player and
renderer, or as an AU, VST3 or standalone plugin on macOS. This page shows how to build
and play it, and how to load the instruments and songs of any game that uses the same two
chips.

## The sound board

The Out Run sound board has two sound chips, and OPM86 emulates both at their original
clock rates. Each chip produces its output at its own sample rate, and OPM86 converts both
outputs to the sample rate of your audio interface.

| Chip | Role | Clock | Sample rate | Voices |
| --- | --- | --- | --- | --- |
| YM2151 (OPM) | FM synthesis for melody, chords and bass | 4MHz | 62.5kHz | 8 |
| Sega PCM (315-5218) | 8-bit sample playback for drums | 4MHz | 31.25kHz | 16 |

The YM2151 makes each voice from four sine oscillators called operators, named M1, C1, M2
and C2. An operator is either heard directly, as a carrier, or changes the sound of another
operator, as a modulator. The algorithm is a number from 0 to 7 that sets which operators
modulate which. A patch is the complete set of these settings for one sound. The YM2151 is
tuned for a 3.58MHz clock, so it plays sharp at Out Run's 4MHz. OPM86 corrects the pitch,
so A4 plays at 440Hz.

The Sega PCM chip plays recorded sounds, called samples, from a 512KB memory. A drum pad is
a sample that one MIDI note plays. OPM86 has 16 drum pads, and each pad plays on its own
chip channel. MIDI reaches the two chips by channel:

| MIDI channel | Plays | Notes |
| --- | --- | --- |
| 10 (you can change it) | Drum pads | 36 to 51, one pad per note, as in the General MIDI drum map |
| Every other channel | FM voices | All notes, with 8 sounding at once in total; a ninth note stops the oldest |

The FM voices also respond to three MIDI controllers:

| Controller | Effect |
| --- | --- |
| Pitch bend | Bends the pitch by up to 2 semitones |
| Mod wheel (CC 1) | Adds vibrato |
| Sustain pedal (CC 64) | Holds notes after their keys are released |

## Build

OPM86 builds on macOS with CMake 3.22 or later and the Xcode command line tools. The first
configure needs a network connection, because CMake downloads JUCE 9.0.3 and the ymfm chip
emulator. Configure and build from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The build writes these files:

| File | What it is |
| --- | --- |
| `build/opm86_cli_artefacts/Release/opm86` | The command-line player and renderer |
| `build/OPM86_artefacts/Release/AU/OPM86.component` | The Audio Unit plugin, for Logic and GarageBand |
| `build/OPM86_artefacts/Release/VST3/OPM86.vst3` | The VST3 plugin, for most other hosts |
| `build/OPM86_artefacts/Release/Standalone/OPM86.app` | The plugin as an application, with no host needed |

To build only the command-line program and put it in the repository root as `./opm86`,
run `make`. To build the tracker app and put it in the repository root as
`OPM86 Tracker.app`, run `make tracker`, then open it with `open "OPM86 Tracker.app"`.
`make all` builds both. Each configures the `build` folder the first time, and `make clean`
removes both copies.

A host finds a plugin only in the plugin folders of your user account, so copy the plugin
there:

```sh
cp -R build/OPM86_artefacts/Release/AU/OPM86.component ~/Library/Audio/Plug-Ins/Components/
cp -R build/OPM86_artefacts/Release/VST3/OPM86.vst3 ~/Library/Audio/Plug-Ins/VST3/
```

## Use the command line

The `opm86` program plays the synthesizer without a host. Every command except `tune`
accepts the options listed after the commands (see [Options](#options)).

### play

The `play` command plays from every MIDI input that is connected when it starts, through
your default audio output. Connect the keyboard first, because the command does not look for
new inputs later. While it runs, the command reads what you type in the terminal, so run it
in a terminal window of its own. It accepts one entry per line:

| Entry | Effect |
| --- | --- |
| A patch number | Switches every channel to that patch |
| `l` | Lists the patches |
| `q` | Quits |

```sh
$ opm86 play --patch "slap bass"
MIDI input: <the name of your keyboard>
audio: MacBook Pro Speakers at 44100 Hz
playing "Slap Bass". Type a patch number to switch, l to list, q to quit.
> 
```

### render

The `render` command plays a standard MIDI file through the synthesizer and writes a 24-bit
stereo WAV file. It renders every track of the MIDI file, then keeps rendering for 2s after
the last event, so that the last notes can fade. The `--tail` option changes that time.

```sh
$ opm86 render song.mid song.wav --patch 3
wrote song.wav: 2.6 s, peak -7.0 dBFS
```

The peak is the level of the loudest point in the file, in decibels below full scale. When
the command reports `(clipped)`, the output went past full scale, and the file holds
distortion.

### demo

The `demo` command renders a short groove when you have no MIDI file. The groove plays
chords with the chosen patch on channel 1, a bass line with patch 2 on channel 2, and
drums on channel 10.

```sh
$ opm86 demo demo.wav
wrote demo.wav: 6.1 s, peak -2.8 dBFS
```

### vgm2mid

The `vgm2mid` command converts a VGM file to a standard MIDI file. A VGM file is a
recording of a game's song (see [Import sounds from a VGM file](#import-sounds-from-a-vgm-file)).
The MIDI file has one track for each instrument of the song and one track for its drums,
so a music program shows each instrument's notes in its own piano roll. The file runs at
the song's tempo, which the converter finds from the times the notes start, and a bar of
lead-in puts the song's first beat at the start of the second bar. The notes then sit on
the music program's beats.

A note's volume is its velocity, so the volume moves with the note when you edit it. What
the game changes while a sound plays, such as pitch slides and panning, travels as MIDI
controllers (see [MIDI messages](#midi-messages)). A note on one of the game's channels
keeps that channel's MIDI channel, so two notes of one instrument keep their own slides.
The command also copies the VGM file to the plugin's library, where the plugin finds each
track's instrument.

```sh
$ opm86 vgm2mid "01 Magical Sound Shower.vgz" shower.mid
wrote shower.mid: 7 tracks, 5:58
  track                                MIDI channels    patch  notes
  01 Magical Sound Shower FM 1         5,6,7,8              1   4141
  01 Magical Sound Shower FM 2         4                    2   1418
  ...
  01 Magical Sound Shower FM 6         1                    6    437
  01 Magical Sound Shower Drums        10                   -   7583
tempo 127.19 BPM; set a music program to 127 BPM before you bring the file in
added the song to the plugin's library: ~/Library/Application Support/OPM86/VGM/01 Magical Sound Shower.vgz
play it with: opm86 render shower.mid OUT.wav --vgm "01 Magical Sound Shower.vgz"
```

The patch column is the patch number that `opm86 patches --vgm` lists for the same file
(see [Play a game's song](#play-a-games-song)).

### vgmplay

The `vgmplay` command renders a VGM file by sending its recorded commands straight to the
emulated chips. The result is the song as the game plays it, so it is the reference to
compare a `render` of the converted MIDI file against.

```sh
$ opm86 vgmplay song.vgm game.wav
wrote game.wav: 5.5 s, peak -8.8 dBFS
```

### songtrack

The `songtrack` command writes a song track: a MIDI file that plays a VGM song exactly,
through the plugin's Song mode, one beat at a time (see
[Rearrange a song exactly](#rearrange-a-song-exactly)). The command also copies the VGM
file to the plugin's library, where the plugin finds the song the track names.

```sh
$ opm86 songtrack "01 Magical Sound Shower.vgz" shower-track.mid
wrote shower-track.mid: 9 tracks, 764 beats, 5:58
added the song to the plugin's library: ~/Library/Application Support/OPM86/VGM/01 Magical Sound Shower.vgz
tempo 127.19 BPM; set the project to 127 BPM
  FM 1
  ...
  FM 8
  Drums
```

### export

The `export` command writes a song as the tracker app plays it, with the instruments you
edited and the volume, mute and solo of each track. The command reads a project that the
tracker app saved, as a binary `.opm86` file or as JSON. The command also reads a VGM file
alone, and then writes the song as the game plays it. The extension of the output file
chooses what the command writes:

| Extension | What the file holds |
| --- | --- |
| `.vgz` or `.vgm` | The chips' commands with every edit in them, so any VGM player plays the song; a `.vgz` file is compressed |
| `.wav` | The sound of the song, rendered as the tracker app plays it |
| `.mid` | The notes of the song, without the FM sound of the edited instruments |

An exported VGM file can differ slightly from what the tracker app plays, because the chips
set a volume only in steps of 0.75dB. A WAV file has no such difference, because the
command renders the WAV file with the tracker app's own player.

```sh
$ opm86 export shower.opm86 shower.wav
wrote shower.wav: 5:58, 16-bit, 44100 Hz
```

### patches

The `patches` command lists the patches and the drum pads with the options applied. A
MIDI program change selects patches by this list: program change P selects patch P + 1.

```sh
$ opm86 patches
patches (MIDI program change P selects patch P + 1):
    1  Shower Lead              alg 4  (M1 > C1) + (M2 > C2)
    2  Slap Bass                alg 4  (M1 > C1) + (M2 > C2)
    3  Beach Piano              alg 4  (M1 > C1) + (M2 > C2)
    4  Breeze Bell              alg 4  (M1 > C1) + (M2 > C2)
    5  Sunset Strings           alg 4  (M1 > C1) + (M2 > C2)
    6  Coin Organ               alg 7  M1 + C1 + M2 + C2
    7  Init Sine                alg 7  M1 + C1 + M2 + C2
drum pads (MIDI channel 10):
   36  Kick                       7029 bytes
   37  Side Stick                  737 bytes
   ...
   51  Ride                      18750 bytes
```

Each patch line shows the algorithm, and an arrow points from a modulator to the operator
it modulates. A plus sign joins operators that are heard together.

### tune

The `tune` command plays six A notes, from A1 to A6, and measures the pitch of each. It
exits with status 1 when a note is more than 3 cents off, so `ctest` runs it as a test.

```sh
$ opm86 tune
note  33  expected    55.00 Hz  measured    54.95 Hz   -1.4 cents
...
note  93  expected  1760.00 Hz  measured  1759.53 Hz   -0.5 cents
worst error 1.4 cents
```

### Options

These options apply to `play`, `render`, `demo` and `patches`, and none of them is
required. The `vgmplay` command takes only `--rate` and `--tail`, `export` takes only
`--rate`, `--bits` and `--loop`, `songtrack` takes only `--mix`, and `vgm2mid` takes none.
The `export` command uses these three options only when the output is a WAV file.

| Option | What it does | Default |
| --- | --- | --- |
| `--patch N\|NAME` | Uses this patch on every MIDI channel; a name ignores case | 1 |
| `--vgm FILE` | Imports patches and drum samples from a VGM file (see [Import sounds from a VGM file](#import-sounds-from-a-vgm-file)) | None |
| `--pad NOTE=FILE` | Puts an audio file, such as WAV, AIFF or FLAC, on the pad for NOTE, from 36 to 51; repeat it for more pads | The built-in kit |
| `--drum-channel N` | Plays drums on MIDI channel N, on every channel when N is `all`, or on no channel when N is 0 | 10 |
| `--rate HZ` | Sets the sample rate of `render`, `demo`, `vgmplay` and `export` | 48000, and 44100 for `export` |
| `--bits N` | Sets the sample size of the WAV file that `export` writes, 16 or 24 bits | 16 |
| `--loop` | Makes `export` write only the part of the song that the project's loop marks, played once, when the loop is on | The whole song |
| `--tail SECONDS` | Sets how long `render`, `demo` and `vgmplay` continue after the last event | 2 |
| `--song FILE` | Makes `render` play a song track, with the song in FILE | None |
| `--mix` | Makes `songtrack` write one track for the whole song instead of one per stem | One track per stem |

## Use the plugin

The plugin plays one patch on every MIDI channel except the drum channel. Its window has
five areas, from top to bottom:

1. The header holds the mode switch, the patch list, the **Import VGM** menu, the
   **Default Kit** button, and the knobs for the FM level, the PCM level, the velocity sensitivity, the
   pitch bend range and the drum channel. The patch list shows the built-in patches
   first, then a submenu for each imported song. The drum channel knob goes from Off
   through 1 to 16 to All, and All makes the plugin play only drums, on every channel.
   The mode switch chooses Synth, described here, or Song (see
   [Rearrange a song exactly](#rearrange-a-song-exactly)).
2. The voice row holds the algorithm, the feedback of M1, the LFO and the output. The
   LFO is a low-frequency oscillator that adds vibrato (PM) or tremolo (AM), and the
   output sends the voice to both speakers, to the left or to the right. The row also
   draws the current algorithm as a diagram.
3. The four operator panels hold the settings of M1, C1, M2 and C2. A panel names its
   operator in orange when the operator is a carrier and in blue when it is a modulator.
4. The drum section holds the 16 pads. Click a pad's name to play the pad, or click
   **Sample** to choose its sound.
5. The keyboard plays the patch on MIDI channel 1 when you click its keys.

Each operator panel holds the same ten knobs and one switch:

| Control | Range | What it sets |
| --- | --- | --- |
| Level | 0 to 127 | The attenuation, in steps of 0.75dB; 0 is loudest |
| Attack | 0 to 31 | The speed of the rise at the start of a note |
| Decay 1 | 0 to 31 | The speed of the fall from the peak to the sustain level |
| Sustain | 0 to 15 | The sustain level, in steps of 3dB below the peak |
| Decay 2 | 0 to 31 | The speed of the fall while the note is held |
| Release | 0 to 15 | The speed of the fall after the note ends |
| Mult | 0 to 15 | The frequency as a multiple of the note; 0 means half |
| Detune | -3 to 3 | A fine pitch offset |
| Coarse | 0 to 3 | A coarse pitch offset, in fixed inharmonic steps |
| Key Scl | 0 to 3 | How much faster the envelope runs on higher notes |
| LFO AM | On or off | Whether the LFO tremolo reaches this operator |

The velocity of a note lowers the level of the carriers only, so a soft note keeps the
timbre of a loud one. The plugin saves the pad samples and every setting in your project,
so a project opens with its sounds even when the source files are gone.

## MIDI messages

The synth responds to these MIDI messages, on the command line and in the plugin. The
`vgm2mid` command uses all of them to carry a game's song:

| Message | On an FM channel | On the drum channel |
| --- | --- | --- |
| Note on and off | Plays a note; velocity lowers the carriers' level | Plays a pad; velocity sets its volume |
| Program change | Selects a patch, on the command line only | Nothing |
| Pitch bend | Bends the channel's pitch | Changes the pitch of new hits and of the last pad named |
| RPN 0 (CC 101, 100, 6) | Sets the bend range, up to 96 semitones | Sets the bend range, up to 96 semitones |
| CC 1, modulation | Adds vibrato | Nothing |
| CC 10, pan | Sends the channel to the left (0 to 42), both speakers or the right (86 to 127) | Pans the next hit or volume change |
| CC 11, expression | Lowers the carriers by 127 minus the value, in 0.75dB steps | Nothing |
| CC 64, sustain | Holds notes | Nothing |
| CC 85 | Nothing | At 64 or more, a note off ends its hit, as the game cuts samples |
| CC 87 to 90 | Name an imported song, in four parts | Name an imported song, in four parts |
| CC 102 | Loads that song's patch of this number, in the plugin | Nothing |
| CC 103 | Loads that song's drums onto the pads and plays drums on every channel, in the plugin | The same |
| Polyphonic aftertouch | Nothing | Sets the volume of the pad's latest hit |

The MIDI standard leaves CC 85 undefined, so it cannot clash with another use. Without
it, the drum channel works like a drum machine, and a one-shot sample plays to its end.

## Import sounds from a VGM file

A VGM file is a recording of the commands a game sends to its sound chips while a song
plays. OPM86 reads only the commands for the YM2151 and the Sega PCM chip, and it ignores
the commands for any other chip. So a VGM file is useful only when it comes from a game
whose sound board has both chips, as the Out Run board does. A file with neither chip
fails to import. A file with only one of the two chips gives only patches or only drum
samples.

The VGM archive at vgmrips.net holds these games, among others that use the YM2151 and the
Sega PCM chip:

| Game | Sega board | Pack |
| --- | --- | --- |
| Out Run | Out Run | [out-run-arcade](https://vgmrips.net/packs/pack/out-run-arcade) |
| Turbo Out Run | Out Run | [turbo-out-run-arcade](https://vgmrips.net/packs/pack/turbo-out-run-arcade) |
| Super Hang-On | Out Run | [super-hang-on-arcade](https://vgmrips.net/packs/pack/super-hang-on-arcade) |
| After Burner II | X Board | [after-burner-ii-sega-x](https://vgmrips.net/packs/pack/after-burner-ii-sega-x) |
| Thunder Blade | X Board | [thunder-blade-sega-x](https://vgmrips.net/packs/pack/thunder-blade-sega-x) |
| Power Drift | Y Board | [power-drift-sega-y](https://vgmrips.net/packs/pack/power-drift-sega-y) |
| Galaxy Force II | Y Board | [galaxy-force-ii-sega-y](https://vgmrips.net/packs/pack/galaxy-force-ii-sega-y) |

The archive lists every pack that uses the Sega PCM chip at
[Packs using SegaPCM](https://vgmrips.net/packs/chip/segapcm). Hang-On, Space Harrier and
the YM2203 version of Enduro Racer are in that list, but they pair the Sega PCM chip with
a YM2203 instead of a YM2151, so OPM86 imports only their drum samples.

OPM86 does not include or download any of this music, so you load a file you have. The
file can be `.vgm` or the compressed `.vgz`, and the importer reads two things from it:

| From | OPM86 makes | Order |
| --- | --- | --- |
| Each YM2151 note start | A patch, from the settings the channel had at that moment | Most used first |
| Each Sega PCM sample start | A pad sample, cut from the sample memory the file holds | Most used first |

The importer treats two note starts as one patch when they differ only in carrier level,
because the game sets that level to change the volume of a note. The patch keeps the
loudest level it saw. Each patch takes the file's name and a number, such as
`Magical Sound Shower FM 1`.

On the command line, `--vgm` puts the imported patches before the built-in ones and the 16
most used samples on the pads, for that run only. In the plugin, **Import VGM** copies the
file to a library folder and puts the song's most used samples on the pads. Every instance
of the plugin loads the library folder when it starts, so an imported song stays available
in every project. The patch list shows each song as a submenu after the built-in patches,
and the **Sample** menu of each pad shows the song's samples under **Imported samples**.

The **Import VGM** menu holds three items:

| Item | What it does |
| --- | --- |
| **Import VGM file...** | Imports a file into the library and loads its first patch and its drum kit |
| **Load drum kit from** | Puts the samples of an imported song on the pads, most used first from pad 36 |
| **Show imported files in Finder** | Opens the library folder |

The library folder is `~/Library/Application Support/OPM86/VGM`. Deleting a file from the
folder removes its song the next time the plugin starts. The `OPM86_LIBRARY_DIR`
environment variable sets a different folder.

```sh
opm86 play --vgm "02 Magical Sound Shower.vgz"
```

## Play a game's song

A game's song plays through OPM86 in two steps. First `opm86 vgm2mid` converts the VGM
file to a MIDI file, and then the command line or GarageBand plays the MIDI file. Each
track of the MIDI file plays one patch of the song, and the drum track plays the song's
samples on pads 36 and up.

The command-line route sounds like the game. On the 12 songs of the Out Run rip, the
converted songs match the `vgmplay` renders in level to within 0.01dB. The difference
between the two waveforms is between 30dB and 63dB below the music. The `vgmplay`
render itself is the exact song, because it sends the game's own commands to the chips.

### On the command line

The `render` command plays every track of the converted file at once, when `--vgm` loads
the same VGM file. Each track starts with a MIDI program change, and the program change
selects the track's patch:

```sh
opm86 vgm2mid "Magical Sound Shower.vgz" shower.mid
opm86 render shower.mid shower.wav --vgm "Magical Sound Shower.vgz"
```

To hear how close the conversion is, render the same file with `vgmplay` and compare the
two WAV files:

```sh
opm86 vgmplay "Magical Sound Shower.vgz" shower-game.wav
```

### In GarageBand

GarageBand gives each track its own copy of the plugin. Each track of the converted file
starts with controllers that load the track's instrument into its plugin, or the song's
drums for the drum track (see [MIDI messages](#midi-messages)). Follow these steps:

1. Convert the song with `opm86 vgm2mid`, as in the command-line route. The command
   prints the song's tempo and adds the song to the plugin's library.
2. Set an empty GarageBand project to the printed tempo. GarageBand keeps the project's
   tempo when you bring a MIDI file in, so the bars line up only when the tempo is set
   first.
3. Drag the MIDI file into the project. GarageBand makes one track for each instrument
   and one for the drums.
4. On each track, replace the instrument with **AU Instruments > xesco > OPM86**.
5. Play the project from the start once. Each copy of the plugin loads its instrument, or
   the drums, when the track's first controllers reach it.

Edit the notes in each track's piano roll. Leave the plugin's velocity knob at 50%,
because the converter writes each note's volume for that setting. The converted file
sets its own pitch bend range, so the bend knob does not matter. The route is less exact
than the command line, for three reasons:

| Cause | Effect |
| --- | --- |
| GarageBand ignores program changes | A patch the game changes during a note keeps its first settings |
| GarageBand's tempo is a whole number of BPM | The song plays a fraction of a percent faster or slower than the game |
| A music program may shorten a note when the same note starts again | Two overlapping hits of one drum pad can cut each other short |

This route has not been tried in GarageBand yet.

## Rearrange a song exactly

The plugin's Song mode plays a VGM song exactly as the game does: it sends the song's own
commands to its own copy of the chips. A song track decides which part of the song plays
when. A song track holds one MIDI note for each beat of the song, and a note means "play
this beat". Played in order, the notes play the song straight through. Moved, cut or
repeated, they play the song rearranged, and every beat still sounds exactly as in the
game.

The `songtrack` command finds the song's beats from the times its notes start. It writes
one track for each stem, where a stem is one YM2151 channel or all the drums. A plugin set
to one stem still runs the whole song, so its stem sounds exactly as in the full mix. Each
track starts with controllers that name its song and its stem:

| Controller | Value |
| --- | --- |
| CC 86 | The stem: 0 for the whole song, 1 to 8 for a YM2151 channel, 9 for the drums |
| CC 87 to 90 | The song's number, in four parts |

A plugin that receives these controllers switches to Song mode and loads the song from
its library. The plugin's window shows the song and the stem, and lets you change both.

### Rearrange in GarageBand

GarageBand gives each track its own copy of the plugin, so each stem plays on its own
track. Follow these steps:

1. Write the song track with `opm86 songtrack`, which also puts the song in the plugin's
   library.
2. Set the GarageBand project to the tempo that `songtrack` printed.
3. Drag the MIDI file into the empty project. GarageBand makes one track for each stem.
4. On each track, replace the instrument with **AU Instruments > xesco > OPM86**.
5. Play the project from the start once. Each copy of the plugin sets itself to its song
   and stem.

To rearrange the song, select the regions of all the tracks together and move, cut or
copy them. A beat that follows the one before it plays on without a jump. A beat
anywhere else jumps there with a 3ms crossfade, which avoids a click. GarageBand's
tempo may differ a little from the song's, because the song's tempo is not a whole
number. Between jumps the plugin follows the song's own timing, so the difference causes
no jumps. This route has not been tried in GarageBand yet.

### Check on the command line

The `render` command plays a song track when `--song` names the song. It plays each
track with its own player, as GarageBand plays each track with its own plugin, and mixes
the players:

```sh
opm86 songtrack "01 Magical Sound Shower.vgz" shower-track.mid
opm86 render shower-track.mid shower-track.wav --song "01 Magical Sound Shower.vgz"
```

Played straight through, the nine stems of Magical Sound Shower mix to the whole song,
played from the same start, with a difference 58dB below the music. That difference
comes from the YM2151's output converter, which rounds each stem on its own instead of
rounding the mix. A beat reached by a jump is identical to the same beat in a render
started there.

## Drum pads

A pad holds one sample in the Sega PCM chip's format, which stores each sample point in 8
bits. OPM86 converts a loaded audio file to this format at 15,625 samples per second, so a
pad sounds as rough as the arcade board. An imported sample keeps the playback rate the
game used. Each pad has four settings:

| Setting | Range | What it does |
| --- | --- | --- |
| Tune | -24 to 24 | Shifts the pitch, in semitones |
| Level | 0 to 127 | Sets the volume, on the chip's 7-bit scale |
| Pan | -63 to 63 | Moves the sound from left to right |
| Choke | 0 to 3 | Stops the other pads of the same group when the pad plays; 0 stops none |

The closed, pedal and open hi-hats start in choke group 1, so a closed hi-hat cuts an open
one short. A sample holds at most 65,280 bytes, which is about 4.2s. The 16 pads share the
chip's 512KB memory, and OPM86 shortens a sample that does not fit. A sample that loops
plays until its note ends, and any other sample plays to its end.

## Tests

The tests check the chips and the plugin without a host. Run them after a build:

```sh
cd build && ctest
```

The `ctest` command runs five programs:

| Test | What it checks |
| --- | --- |
| `core_test` | The VGM importer on a generated file, the Sega PCM addressing, and the default kick |
| `tuning` | The `opm86 tune` command, which fails when a note is more than 3 cents off |
| `export_test` | A generated song converted to MIDI against the same song sent straight to the chips, in loudness over time, level, spectrum and sample by sample |
| `song_test` | A generated song's song track, played straight through against the whole song, and a jump against a start at the same beat |
| `plugin_test` | The plugin's sound on both chips, a pad change reaching the audio, saved settings, imports that stay in the library, instruments and drums loaded by a converted file's controllers, and Song mode set up by a song track |

The `editor_snapshot` target draws the plugin window to a PNG file, to check the layout
without a host. The build skips it unless you name it:

```sh
cmake --build build --target editor_snapshot
build/editor_snapshot_artefacts/Release/editor_snapshot editor.png
```

## Limits

OPM86 differs from the arcade board and from a full emulator in these ways:

- The built-in patches are approximations made by ear, not copies from the game. A VGM
  file gives the real ones.
- The plugin ignores MIDI program changes, because its knobs hold one patch. The command
  line uses them.
- The LFO is one circuit on the YM2151, so it follows the patch of MIDI channel 1 when
  channels play different patches.
- The importer reads VGM files only. A dump of the arcade's sample memory does not say
  where each sample starts, because the game's program holds that information.
- Changing a pad's sample stops all drum sounds at once, with a short click.
- A converted MIDI file comes close to the game but is not identical to it. The
  `vgmplay` command gives the exact song.
- The converted MIDI file has one tempo, found from the times the notes start. A song
  whose tempo changes keeps its notes' times, but its bars drift from its beats.
- The drum track uses the 16 most used samples. The `vgm2mid` command reports the hits
  of any other sample it leaves out.

The emulation comes from [ymfm](https://github.com/aaronsgiles/ymfm) for the YM2151, and
from the behaviour of `segapcm.cpp` in [MAME](https://github.com/mamedev/mame) for the Sega
PCM chip. The [VGM specification](https://github.com/vgmrips/vgmplay/blob/master/VGMPlay/vgmspec171.txt) defines the
VGM format, and [JUCE](https://juce.com) is the plugin framework.
