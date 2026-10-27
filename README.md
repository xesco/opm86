# OPM86 Player

OPM86 Player is a desktop app for macOS, Linux and Windows that plays the music of Sega's
arcade games from the late 1980s, such as Out Run, from VGM recordings of their sound
chips. It plays each song exactly as the game did, shows its notes track by track, and
shows the two sound chips at work. This page is for anyone who wants to listen to these
songs closely. It explains how to download the app, the sound board it emulates, where to
find songs and how to use the app, and, for developers, how to build it.

![OPM86 Player playing Like the Wind from Power Drift: the tracks with their notes, the keys
strip and the overview](docs/screenshot.png)

## Download

Download the app from the [latest release](https://github.com/xesco/opm86/releases/latest).
Take the file for your system, unpack it, and open the app inside. Older releases are on
the [releases page](https://github.com/xesco/opm86/releases), and [Build](#build) shows how
to build the app yourself.

| System | What it needs | File |
| --- | --- | --- |
| macOS | macOS 11 or later, on Apple Silicon or Intel | `OPM86-Player-macos.zip` |
| Windows | Windows 10 or later | `OPM86-Player-windows.zip`, or `OPM86-Player-windows-arm64.zip` on a PC with an ARM processor |
| Linux, x86-64 | A distribution from 2022 on (glibc 2.35 or later), such as Ubuntu 22.04, Debian 12 or Linux Mint 21, with the ALSA, FreeType, Fontconfig and X11 libraries, which most desktops install already | `OPM86-Player-linux.tar.gz` |
| Raspberry Pi | The 64-bit Raspberry Pi OS | `OPM86-Player-linux-arm64.tar.gz` |

A Raspberry Pi 5 or 500 plays songs, but the scrolling is not fully smooth, because the app
redraws 30 times a second on a Pi. A Pi 4 or 3 has not been tried, and is probably too
slow.

### Open the app the first time

The app is not signed with a developer certificate, so macOS and Windows ask you before
they open it the first time:

- **macOS:** move OPM86 Player to Applications and open it. When macOS says that it cannot
  check the app, open System Settings, then Privacy & Security, and click **Open Anyway**
  beside the message about OPM86 Player. When macOS says instead that the app is damaged,
  the download flag on the app is the cause, and the command
  `xattr -dr com.apple.quarantine "/Applications/OPM86 Player.app"` in Terminal clears it.
- **Windows:** when SmartScreen says that it protected your PC, click **More info**, then
  **Run anyway**.
- **Linux and Raspberry Pi:** the file `opm86-player` starts the app, and the file
  `opm86-player.png` is its icon, for an entry in your launcher.

## The sound board

Sega's arcade boards of the late 1980s, such as the Out Run board, the X Board and the Y
Board, play their music on two sound chips. The app emulates both chips, each at the clock
rate that the VGM file records for its board. Each chip produces its output at its own
sample rate, a fixed fraction of its clock, and the app converts both outputs to the sample
rate of your audio interface.

| Chip | Role | Voices | Sample rate |
| --- | --- | --- | --- |
| YM2151 (OPM) | FM synthesis for melody, chords and bass | 8 | Its clock ÷ 64 |
| Sega PCM (315-5218) | 8-bit sample playback, mostly for drums | 16 | Its clock ÷ 128 |

The boards run both chips at one clock, which sets their sample rates:

| Board | Games, for example | Clock | YM2151 sample rate | Sega PCM sample rate |
| --- | --- | --- | --- | --- |
| Out Run, X Board | Out Run, Turbo Out Run, After Burner II | 4MHz | 62.5kHz | 31.25kHz |
| Y Board | Power Drift, Galaxy Force II | 4.027MHz | 62.9kHz | 31.5kHz |

The project takes its name from the YM2151 and from Out Run. OPM is Yamaha's name for the
YM2151, short for FM Operator Type-M. 86 stands for 1986, the year Sega released Out Run,
as a tribute to a game that plays its music on these chips.

The YM2151 makes each voice from four sine oscillators called operators, named M1, C1, M2
and C2. An operator is either heard directly, as a carrier, or changes the sound of another
operator, as a modulator. The algorithm is a number from 0 to 7 that sets which operators
modulate which. A patch is the complete set of these settings for one sound. The YM2151 is
tuned for a 3.58MHz clock, so on these boards, which run it faster, it plays sharp, as it
did in the arcade. The app names each note by the pitch it plays at its board's clock.

The Sega PCM chip plays recorded sounds, called samples, from a memory whose size depends
on the board: 512KB on the Out Run board. Each of its 16 channels plays one sample at a
time.

## Build

Every system needs CMake 3.22 or later, a C++20 compiler, and a network connection the
first time, when CMake downloads JUCE 9.0.3 and the ymfm chip emulator. `make` builds the
app for the system it runs on and puts it in the repository root; `make clean` removes it.
On a machine short of memory, such as a Raspberry Pi, add `JOBS=1` to compile one file at a
time.

### macOS

The Xcode command line tools.

```sh
# Get the code
git clone https://github.com/xesco/opm86.git && cd opm86
# Build OPM86 Player.app
make
# Start it
open "OPM86 Player.app"
```

### Windows

Visual Studio 2022 with its C++ tools, whose CMake runs presets (3.25 or later).

```sh
# Get the code
git clone https://github.com/xesco/opm86.git && cd opm86
# Configure and build build/opm86_player_artefacts/Release/OPM86 Player.exe; on Windows,
# CMake also downloads zlib
cmake --workflow --preset release
```

### Linux and Raspberry Pi

```sh
# Get the code
git clone https://github.com/xesco/opm86.git && cd opm86
# Install the compiler and the libraries JUCE needs: apt on Ubuntu, Debian and
# Raspberry Pi OS, pacman on Arch and Omarchy
sudo scripts/linux-deps.sh
# Build ./opm86-player
make
# Start it
./opm86-player
```

### Linux, from a Mac

```sh
# Build the app and run the tests in an Ubuntu container, with Docker; build-linux keeps
# the build and its compiler cache, so a second run takes seconds instead of 5 minutes
make linux
```

## Get songs

A VGM file is a recording of the commands a game sends to its sound chips while a song
plays. The app reads only the commands for the YM2151 and the Sega PCM chip, and it ignores
the commands for any other chip. So a VGM file is useful only when it comes from a game
whose sound board has both chips, as the Out Run board does. The file can be `.vgm` or the
compressed `.vgz`.

The VGM archive at [vgmrips.net](https://vgmrips.net) holds these games, among others that use the YM2151 and the
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
a YM2203 instead of a YM2151, so the player plays only their drums.

The app does not include or download any of this music, so you open a file you have. While
no song is open, the window shows links to songs to start with, such as Magical Sound
Shower from Out Run. A click on a link downloads the song's file in your browser, and you
then open the file in the app.

## Use OPM86 Player

The app sends a VGM file's own commands to the two chips, so it plays the exact song. While
the song plays, you can mute, solo and change the volume of each part of it, loop a
section, and watch the registers of the two chips.

### Open a song

Click **Open** in the top bar or press Cmd+O to choose a `.vgz` or `.vgm` file, or drop the
file on the window. When the app starts, it opens the song you played last. The app also
remembers the size and place of its window and the pitch colours you chose.

To close the song, click **Close** in the top bar, beside **Open**, or press Cmd+W. The
window then shows the links to songs to start with (see [Get songs](#get-songs)), and the
next launch opens on them instead of the last song. While no song is open, every control
except **Open** is greyed out and does nothing.

### The window

The window has six areas, from top to bottom:

1. The top bar holds the app's version under its name, the transport buttons, the song's
   name, the master volume, **Close** and **Open**. The transport buttons are rewind, play
   or pause, stop, and to the end. A click on the app's name opens the credits: the
   version, the licence, the project's page, and the parts the app is built with.
2. The tool bar holds the controls in named groups (see [The tool bar](#the-tool-bar)).
3. The tracks area shows a ruler with the song's bars and beats, and a lane for each
   track. Each note shows as a rectangle in the colour of its pitch.
4. The keys strip is a keyboard of 3 to 7 octaves that lights the keys of the notes
   sounding at the playhead. Its buttons set the number of octaves and show or hide the
   note names.
5. The overview strip shows the whole song, with a frame around the part of the song
   that the tracks area shows.
6. The status bar counts the tracks and the notes, and shows the pitch colours. Click a
   pitch's colour to choose another one.

The keys strip and the overview strip slide down when you click the arrow in their header,
and they grow taller when you drag the grip in their top edge.

### The tracks

An FM track holds the notes that one patch plays on one voice of the YM2151, so each track
plays one note at a time. Each track is named after its voice and numbered by its patch on
that voice, with the most used patch first: V1 1, V1 2, then V2 1. The patch's number
follows the name, as in FM 6. The drum tracks come after the FM tracks. A drum note in
grey is a hit the game played at zero volume, which you do not hear.

A track's header holds its mute and solo buttons, a level meter, and a volume knob with
its level in dB under it. The knob controls the track like this:

| Action | Effect |
| --- | --- |
| Drag up or right | Raises the volume |
| Drag down or left | Lowers the volume |
| Double-click, or Option-click | Puts the volume back to 0dB, the level the game played |

A muted track is silent even when it is also soloed. Cmd+Z undoes a change to mute, solo
or volume, and Shift+Cmd+Z redoes it. The notes themselves cannot be edited.

The ruler's bars and beats come from the times the notes start, because a VGM file
records no tempo. Bar 1 starts at the song's first note.

### Pitch lines

A game can change the pitch of a note while the note sounds, without starting a new one.
It plays a run of keys this way, or slides from one pitch to another. Such a note shows a
line across it that follows its pitch over time. Each line has its own scale: the note's
lowest pitch is at its bottom and its highest at its top, over at least 2 semitones. So the
heights of two notes' lines do not compare.

| Change of pitch | How the line shows it |
| --- | --- |
| A jump of a semitone or more | A step |
| A slide, in small steps close together | A ramp |
| Less than a quarter of a semitone in all | No line |

The note's name is its pitch at its start. The keys strip lights the key nearest the pitch
that the note plays at the playhead. **Show pitch lines** in the tool bar shows or hides
the lines, and the app remembers the choice.

### The tool bar

The tool bar groups its controls by what they act on:

| Group | Controls |
| --- | --- |
| PLAYHEAD | **Keep playhead** keeps the playhead's line on screen, which otherwise fades a second after you last move the mouse or press a key. **Follow playhead** scrolls the tracks with the song, and it is on when the app starts. |
| SPEED | Plays the song from 25% to 200% of its speed, with the notes at their own pitch. Double-click the slider to go back to 100%. |
| LOOP | **Loop** plays the selected notes or the selection again and again, or the whole song when nothing is selected. The START and END fields show the selection's times and set them (see [Select and loop](#select-and-loop)). |
| MIX | **Reset mix** unmutes, unsolos and sets every track to 0dB. **Invert** flips the solos when any track is soloed, and otherwise flips the mutes. **Only instruments** and **Only drums** solo one kind of track, and a second click turns the solo off. |
| VIEW | **Order by patch** puts the FM tracks in patch order, FM 1 first, with the patch's number before the voice in each name. **Show the chips** shows the chips view in place of the tracks (see [The chips view](#the-chips-view)). **Show pitch lines** shows or hides the lines across the notes (see [Pitch lines](#pitch-lines)). |
| ZOOM | **Fit tracks**, **Fit time** and **Fit both** fit the tracks, the song, or both to the window. **Fit tracks** fits the tracks you can hear, from the first to the last, with any muted track between them. **1:1** puts both zooms back to where they start. |
| HEIGHT and TIME | The two sliders set the height of the tracks and the zoom in time. |

### Play and move

The keyboard controls the song like this:

| Key | Effect |
| --- | --- |
| Space | Plays, or pauses while playing; with notes selected, plays them alone (see [Play notes alone](#play-notes-alone)) |
| Return or Home | Goes to the start of the selection, or of the song |
| End | Goes to the end of the selection, or of the song |
| Cmd+= and Cmd+- | Zooms in and out in time |
| Option+Cmd+= and Option+Cmd+- | Makes the tracks taller and shorter |
| Escape | Drops the selected notes, or else the selection |
| Cmd+W | Closes the song |

The **stop** button stops the song, and a second click goes back to the song's start. When
the song plays to its end, the playhead goes back to the start 1s later. The mouse moves
around the song like this:

| Mouse | Effect |
| --- | --- |
| Click a lane off the notes, or the ruler | Moves the playhead there |
| Drag the lanes while playing, or with Shift | Moves the view with a hand, across and up or down |
| Scroll | Scrolls through time and through the tracks |
| Cmd+scroll, or pinch | Zooms in time |
| Option+scroll, or Shift+pinch | Changes the height of the tracks |
| Drag the overview's frame | Moves the view |

While **Follow playhead** is on, the overview's frame carries the playhead. Holding the
frame pauses the song, dragging it plays a moment of each place it reaches, and the song
plays on from where you release it.

### Select and loop

A selection is a section of the song that the playhead stays in. While the song is
stopped, drag across the lanes to select a section. The selection works like this:

| Action | Effect |
| --- | --- |
| Drag an edge of the selection | Moves that edge |
| Drag the selection's bar on the ruler | Moves the whole selection |
| Type a time in START or END, as m:ss.mmm | Sets that edge |
| Click the lanes off the notes while stopped, or press Escape | Drops the selection |

When **Loop** is on, the song plays the selection again and again.

### Play notes alone

You can select notes and play them without the rest of the song, whatever the mutes and
solos. Space or the play button plays the selected notes, each at its own time, and cuts
each one at its end. Then the playhead rests at the start of the first one. When **Loop**
is on, the notes play again and again. The notes are selected like this:

| Action | Effect |
| --- | --- |
| Click a note | Selects it, in place of any other, and outlines it |
| Click a selected note | Deselects it |
| Shift-click a note | Adds it to the selected notes, or removes it |
| Click a lane off the notes, or press Escape | Drops the selected notes |

Selected notes and a selection replace each other. Selecting a note drops the selection,
even a selection around the note, and selecting a section drops the selected notes.

### The chips view

The chips view shows the registers of the two chips at the playhead, in place of the
tracks. The values are the game's commands up to the playhead, and each value glows when
the game writes it. A value written again unchanged glows dimmer. The view only shows the
chips, so nothing in it can be changed. While the view shows, the keys strip and the
overview strip slide down, and they come back as they were when you turn the view off. The
app always starts with the tracks.

Only the channels you can hear light up. A channel that your mutes or solos silence keeps
the values it had when you last heard it, and shows no glow, key or envelope until you
hear it again.

Each chip has its own section. A section's header shows the chip's left and right output
levels, how many commands it receives a second (WRITES), and its last command (LAST). WRITES
and LAST count only the commands to channels you can hear:

| Chip | What its section shows |
| --- | --- |
| YM2151 | A CHIP card with the settings that all channels share: the LFO, the noise, the two timers, the control register, the two output pins and the test register. Below it, a card for each of the 8 channels with its note, its channel settings, a level meter, and a table with a row of settings for each of its four operators. The graph toggle, at the right end of a card's header, switches the card between its table and a graph of the channel: its algorithm as wired boxes, with each operator's frequency in Hz, and each operator's envelope, lit along the stretch it has run, with a dot that travels it. Channels 1 to 4 start as tables and 5 to 8 as graphs. |
| Sega PCM | A map of the samples the song plays, in rows by 64KB bank, where a playing sample lights up and, when its tile has room, shows the address being read. Below it, a row for each of the 16 channels with its start, loop and end addresses, its playback rate, its flags, its bank, its two volumes and its output level. |

Rest the pointer on any label in the view to see what it means.

## Tests

The tests check the app's core without the window. Run them after a build:

```sh
cmake --build build --config Release
ctest --test-dir build -C Release
```

The `core_test` program checks the VGM reader on a generated file, the Sega PCM addressing,
the split of a song into notes and tracks, the pitch of a note that the game bends, and the
player's mix, speed and volume, including notes played alone.

GitHub also checks that each build of the app starts. The `core_test --write-song song.vgm`
command writes the tests' song, and the app opens it, draws its window to a PNG and quits.
On Linux, a virtual display stands in for a screen.

## Limits

The app differs from the arcade board in these ways:

- The bars and beats have one tempo, found from the times the notes start. A song whose
  tempo changes keeps its notes' times, but its bars drift from its beats.
- The app reads VGM files only. A dump of the arcade's sample memory does not say where
  each sample starts, because the game's program holds that information.

## Licence

OPM86 Player is free software under the GNU Affero General Public License, version 3 (see
[LICENSE](LICENSE)). You may use, change and share it. When you share a changed version,
you must also share its source under the same licence.

OPM86 Player is built on these parts, each under its own licence:

| Part | What it does in OPM86 Player | Licence |
| --- | --- | --- |
| [JUCE](https://juce.com) | The window, the controls and the sound output | AGPLv3 or a commercial licence; OPM86 Player uses JUCE under the AGPLv3 |
| [ymfm](https://github.com/aaronsgiles/ymfm) | Emulates the YM2151 | BSD 3-Clause |
| [zlib](https://zlib.net) | Reads `.vgz` files; built from source on Windows only | zlib licence |
| [Press Start 2P](https://fonts.google.com/specimen/Press+Start+2P) and [JetBrains Mono](https://www.jetbrains.com/lp/mono/) | The app's fonts | SIL Open Font License 1.1, texts in `resources/fonts` |

The Sega PCM emulation is the project's own code, written to behave as `segapcm.cpp` in
[MAME](https://github.com/mamedev/mame) does. The
[VGM specification](https://github.com/vgmrips/vgmplay/blob/master/VGMPlay/vgmspec171.txt)
defines the format of the files the app reads. The app includes no music, and the songs in
VGM files belong to their rights holders.
