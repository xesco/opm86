# OPM86 Player

Listen to the music of Sega's late-1980s arcade games, such as Out Run, on macOS, Windows or
Linux. OPM86 Player opens VGM files, which record the commands a game sent to its sound
chips, and plays them back on emulated YM2151 and Sega PCM chips, so you hear each song
exactly as the game played it.

The name comes from OPM, Yamaha's name for the YM2151 sound chip, and 1986, the year Out Run
came out.

![OPM86 Player playing Like the Wind from Power Drift: the tracks with their notes, the keys
strip and the overview](docs/screenshot.png)

## Features

- See every note, track by track, with a keyboard and an overview of the whole song.
- Mute, solo or change the volume of any track.
- Loop a section, or play just the notes you select.
- Play a song from 25% to 200% of its speed without changing its pitch.
- Watch both sound chips at work while the song plays.

## Download

Get the file for your system from the
[latest release](https://github.com/xesco/opm86/releases/latest), unpack it and open the app.
Older versions are on the [releases page](https://github.com/xesco/opm86/releases).

| System | Requirements | File |
| --- | --- | --- |
| macOS | macOS 11 or later, Apple Silicon or Intel | `OPM86-Player-macos.zip` |
| Windows | Windows 10 or later | `OPM86-Player-windows.zip` |
| Windows on ARM | Windows 10 or later | `OPM86-Player-windows-arm64.zip` |
| Linux, x86-64 | glibc 2.35 or later (Ubuntu 22.04, Debian 12, Linux Mint 21 or newer), with ALSA, FreeType, Fontconfig and X11, which most desktops already have | `OPM86-Player-linux.tar.gz` |
| Raspberry Pi | 64-bit Raspberry Pi OS | `OPM86-Player-linux-arm64.tar.gz` |

A Raspberry Pi 5 or 500 can play songs, but scrolling isn't fully smooth. A Pi 3 or 4 hasn't
been tried and is probably too slow.

### First launch

The app isn't signed with a developer certificate, so macOS and Windows warn you the first
time you open it.

- **macOS:** Move the app to Applications and open it. If macOS says it can't check the app,
  go to System Settings, then Privacy & Security, and click **Open Anyway**. If it says the
  app is damaged, run `xattr -dr com.apple.quarantine "/Applications/OPM86 Player.app"` in
  Terminal.
- **Windows:** When SmartScreen appears, click **More info**, then **Run anyway**.
- **Linux and Raspberry Pi:** Run `opm86-player` to start the app. Use `opm86-player.png` as
  the icon if you add it to your launcher.

## Get songs

The app doesn't come with any music, so you need your own `.vgm` or `.vgz` files. Songs play
in full only if the game used both of these chips:

| Chip | What it plays |
| --- | --- |
| YM2151 | Melody, chords and bass |
| Sega PCM | Recorded sounds, mostly drums |

The app ignores any other chip. Hang-On, Space Harrier and the YM2203 version of Enduro Racer
use a YM2203 instead of the YM2151, for example, so you only hear their drums. Dumps of the
arcade's sample memory won't work either, because they don't say where each sample starts.

[vgmrips.net](https://vgmrips.net) has plenty of songs that work. Here are some good places
to start:

| Game | Sega board |
| --- | --- |
| [Out Run](https://vgmrips.net/packs/pack/out-run-arcade) | Out Run |
| [Turbo Out Run](https://vgmrips.net/packs/pack/turbo-out-run-arcade) | Out Run |
| [Super Hang-On](https://vgmrips.net/packs/pack/super-hang-on-arcade) | Out Run |
| [After Burner II](https://vgmrips.net/packs/pack/after-burner-ii-sega-x) | X Board |
| [Thunder Blade](https://vgmrips.net/packs/pack/thunder-blade-sega-x) | X Board |
| [Power Drift](https://vgmrips.net/packs/pack/power-drift-sega-y) | Y Board |
| [Galaxy Force II](https://vgmrips.net/packs/pack/galaxy-force-ii-sega-y) | Y Board |

You can also browse [every pack that uses the Sega PCM chip](https://vgmrips.net/packs/chip/segapcm).
When no song is open, the app shows links to a few songs, such as Magical Sound Shower from
Out Run. Click one to download it in your browser, then open the file in the app.

## Quick start

1. Click **Open** or press Cmd+O to choose a song, or drop the file on the window.
2. Press Space to play or pause.
3. Use the mute and solo buttons and the volume knob in a track's header to change the mix.
4. Stop the song, drag across the tracks to select a section, then click **Loop**.
5. Click **Show the chips** to watch the chips at work.

See the [user guide](docs/usage.md) for every control and shortcut.

## Build

You need CMake 3.22 or later and a C++20 compiler. The first build also needs a network
connection, because CMake downloads JUCE 9.0.3 and the ymfm chip emulator. Start by getting
the code:

```sh
git clone https://github.com/xesco/opm86.git && cd opm86
```

On macOS, install the Xcode command line tools, then run:

```sh
make
open "OPM86 Player.app"
```

On Linux or a Raspberry Pi, run the commands below. The first one installs the compiler and
libraries with apt or pacman.

```sh
sudo scripts/linux-deps.sh
make
./opm86-player
```

On Windows, install Visual Studio 2022 with its C++ tools, including CMake 3.25 or later, then
run the command below. It also downloads zlib, and it builds
`build/opm86_player_artefacts/Release/OPM86 Player.exe`.

```sh
cmake --workflow --preset release
```

To build for Linux on a Mac, run `make linux`. It builds the app and runs the tests in an
Ubuntu container, using Docker, and keeps the results in `build-linux`.

`make clean` removes what `make` built. If your machine is short of memory, as a Raspberry Pi
can be, add `JOBS=1` to compile one file at a time. To run the tests after a build:

```sh
cmake --build build --config Release
ctest --test-dir build -C Release
```

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

The Sega PCM emulation is the project's own code, written to behave like
[MAME](https://github.com/mamedev/mame)'s `segapcm.cpp`. The
[VGM specification](https://github.com/vgmrips/vgmplay/blob/master/VGMPlay/vgmspec171.txt)
defines the format of the files the app reads. The app includes no music, and the songs in
VGM files belong to their rights holders.
