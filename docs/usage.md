# OPM86 Player user guide

This guide covers every control in OPM86 Player. To install the app or find songs, see the
[README](../README.md).

## Open and close a song

Click **Open** in the top bar, press Cmd+O, or drop a `.vgm` or `.vgz` file on the window.
The app reopens your last song when it starts, and it remembers its window size and position
and your pitch colours.

To close a song, click **Close** or press Cmd+W. The window then shows links to a few songs,
and the app opens on those links next time instead of your last song. Until you open another
song, every control except **Open** is greyed out.

## The window

From top to bottom, the window has six areas:

| Area | What's in it |
| --- | --- |
| Top bar | The app's version, rewind, play or pause, stop and to-the-end buttons, the song's name, the master volume, **Close** and **Open**. Click the app's name to see the credits. |
| Tool bar | The controls described in [The tool bar](#the-tool-bar). |
| Tracks | A ruler with bars and beats, and a lane for each track. Each note is a rectangle coloured by its pitch. The playhead is the line that marks where you are in the song. |
| Keys strip | A keyboard of 3 to 7 octaves that lights up the notes at the playhead. Its buttons change the number of octaves and show or hide the note names. |
| Overview | The whole song, with a frame around the part shown in the tracks. |
| Status bar | The number of tracks and notes, and the pitch colours. Click a colour to change it. |

To hide the keys strip or the overview, click the arrow in its header. To make one taller,
drag the grip along its top edge.

## Tracks

FM tracks come first, then drum tracks. A patch is the set of settings that makes one sound
on the YM2151. Each FM track holds the notes one patch plays on one voice, so it never plays
more than one note at a time.

Tracks are named by voice and patch, with each voice's most used patch first: V1 1, V1 2, then
V2 1. The label after the name, such as FM 6, is the patch's number. A grey drum note is a hit
the game played at zero volume, so you can't hear it.

Each track has mute and solo buttons, a level meter and a volume knob. Mute wins if a track is
both muted and soloed. Cmd+Z undoes a change to mute, solo or volume, and Shift+Cmd+Z redoes it.
You can't edit the notes.

| Knob action | Effect |
| --- | --- |
| Drag up or right | Raises the volume |
| Drag down or left | Lowers the volume |
| Double-click or Option-click | Resets the volume to the level the game played |

A moment after a song opens, each knob moves to the track's loudest level in the song, in dB under
the loudest track, which sits at 0dB. The tick on the ring marks that level, so the pointer shows
how far you have moved the track from what the game played. A track can go up to 6dB above its
tick, so a quiet track can't reach the top of the ring.

VGM files don't record a tempo, so the app works out bars and beats from when the notes start.
Bar 1 starts at the first note. The ruler uses one tempo for the whole song, so if a song's
tempo changes, its bars drift away from its beats. The notes themselves still play at the right
time.

## Pitch lines

A game can change a note's pitch while it sounds, to play a run of notes or a slide. A line
across the note shows that change:

| Pitch change | Line |
| --- | --- |
| A jump of a semitone or more | A step |
| A slide in small, close steps | A ramp |
| Less than a quarter of a semitone in total | No line |

Each line has its own scale, from the note's lowest pitch to its highest, covering at least 2
semitones. That means you can't compare the height of one note's line with another's. A note's
name is its starting pitch, and the keys strip lights the key closest to its pitch at the
playhead.

## The tool bar

| Group | Controls |
| --- | --- |
| PLAYHEAD | **Keep playhead** keeps the playhead visible. Otherwise it fades a second after you last move the mouse or press a key. **Follow playhead** scrolls the tracks with the song, and is on when the app starts. |
| SPEED | Plays the song from 25% to 200% of its speed without changing its pitch. Double-click the slider to go back to 100%. |
| LOOP | **Loop** repeats the selected notes or section, or the whole song if nothing is selected. START and END show and set the selection's times. |
| MIX | **Reset mix** unmutes and unsolos every track and sets it to 0dB. **Invert** flips the solos if any track is soloed, and the mutes if not. **Only instruments** and **Only drums** solo that kind of track. Click again to turn the solo off. |
| VIEW | **Order by patch** sorts the FM tracks by patch, FM 1 first, and puts the patch's number before the voice in each name. **Show the chips** swaps the tracks for the [chips view](#chips-view). **Show pitch lines** turns [pitch lines](#pitch-lines) on or off, and the app remembers your choice. |
| ZOOM | **Fit time** fits the song to the window. **Fit tracks** fits the tracks you can hear, from first to last, including any muted tracks between them. **Fit both** does both. **1:1** resets both zooms. |
| HEIGHT and TIME | These sliders set the track height and the time zoom. |

## Keyboard and mouse

| Key | Effect |
| --- | --- |
| Space | Play or pause, or [play the selected notes](#play-selected-notes) |
| Return or Home | Go to the start of the selection or song |
| End | Go to the end of the selection or song |
| Cmd+= and Cmd+- | Zoom in and out in time |
| Option+Cmd+= and Option+Cmd+- | Make tracks taller or shorter |
| Escape | Clear the selected notes, or else the selection |
| Cmd+W | Close the song |

| Mouse or trackpad | Effect |
| --- | --- |
| Click the ruler, or a lane away from the notes | Move the playhead there |
| Drag the lanes while playing, or with Shift held | Pan across the song and tracks |
| Scroll | Scroll through time and tracks |
| Cmd+scroll or pinch | Zoom in time |
| Option+scroll or Shift+pinch | Change the track height |
| Drag the overview's frame | Move the view |

Click **stop** once to stop the song and again to go back to the start. When a song ends, the
playhead returns to the start after 1s.

While **Follow playhead** is on, the overview's frame carries the playhead. Holding the frame
pauses the song, and dragging it plays a moment of each place it passes. The song carries on
from where you let go.

## Select and loop a section

The playhead stays inside a selected section. To select one, stop the song and drag across the
lanes. With **Loop** on, the section repeats.

| Action | Effect |
| --- | --- |
| Drag an edge | Move that edge |
| Drag the selection's bar on the ruler | Move the whole selection |
| Type a time in START or END, as m:ss.mmm | Set that edge |
| Click away from the notes while stopped, or press Escape | Clear the selection |

## Play selected notes

You can play selected notes on their own, whatever the mutes and solos are set to. Press Space
or the play button, and each note plays at its own time and stops at its end. The playhead then
rests at the first note. With **Loop** on, the notes repeat.

| Action | Effect |
| --- | --- |
| Click a note | Select it instead of any other note |
| Click a selected note | Deselect it |
| Shift-click a note | Add it to the selected notes or remove it |
| Click a lane away from the notes, or press Escape | Clear the selected notes |

You can have selected notes or a selected section, not both. Selecting one clears the other,
even if the section contains the note.

## Chips view

The chips view replaces the tracks with the registers of both chips at the playhead. Registers
are the values the game's commands have set on a chip. A value glows when the game writes it,
and glows less if the new value is the same. You can only look at this view, not change it. The
keys strip and overview slide away while it's open and come back when you close it. The app
always starts with the tracks showing.

The view calls each voice a channel. Only channels you can hear light up. A channel silenced by
your mutes or solos keeps its last values and stays dark until you can hear it again.

Each chip's section starts with its left and right output levels, its commands per second
(WRITES) and its last command (LAST). WRITES and LAST only count channels you can hear.

| Chip | What you see |
| --- | --- |
| YM2151 | A card of settings shared by all channels, and a card for each channel with its note, level and settings. The toggle at the right of a card's header switches between a table and a graph. |
| Sega PCM | A map of the song's samples, where playing samples light up, and a row for each channel with its addresses, rate and volumes. |

Rest the pointer on any label to see what it means.
