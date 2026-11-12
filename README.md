# revosf2

revosf2 converts Nintendo Wii sound archives (`.brsar`) to MIDI, SoundFont 2 and SFZ. Each
sequence gets a MIDI file and a SoundFont containing its instruments. SFZ output includes the
game's filters. SoundFont can only approximate them or leave them out (see
[SFZ files](#sfz-files)). revosf2 also converts the music and sound effects of Wii games that
play their sound through JAudio2 (see [JAudio games](#jaudio-games)).

Drop archives onto `revosf2.exe` on Windows or `Revosf2.app` on macOS. Output goes in a folder
beside each archive. An input can also be a whole game: a disc image, an extracted disc or a
WiiWare title (see [Games](#games)). It can also be a JAudio game's start-up archive (`.baa` or `.aaf`).
From a terminal:

```
revosf2 [options] <archive.brsar> [<archive.brsar> ...]

  --output DIR   write to DIR (one archive only)
  --only REGEX   convert matching sequence names (for example --only BGM)
  --loops N      repeat the main loop N times (default 1)
                 MIDI loop markers are included for players that support them
  --seed N       initial game random state (default 0x12345678, or 0 for
                 a JAudio game)
  --var N=V      set variable N (0-15 player, 16-31 global) to V before each
                 sequence starts; repeat for more variables
  --dol FILE     the game's main.dol, for the build of the game's sound library
                 (default: the game's main.dol when the input is a game, or
                 sys/main.dol of the extracted disc that holds the archive)
  --weather W    convert Animal Crossing: City Folk's hourly and town music in
                 one weather: fine, rain or snow (default: all three)
  --museum N     convert City Folk's museum music in one arrangement, from 1
                 to 7 (default: 1 to 6)
  --list         list sequences without converting
  --quiet        print errors only
  --version      show the version
```

Some sequences play nothing until the game sets one of their variables. `--var` sets one before
each sequence starts. As in the game, player variables start at -1 and global variables at 0.
Use `--only` to choose the sequences it applies to.

The game's `main.dol` shows which build of the sound library plays the archive (see
[Sequence playback](#sequence-playback)). Dolphin's "Extract Entire Disc" and wit put it in a
`sys` folder beside the `files` folder that holds the archives. revosf2 finds it there by
itself. A game given as the input provides its `main.dol` too. A WiiWare title provides its
main program. `--dol` gives it for an archive kept elsewhere. Without it, an archive plays by
its version. revosf2 notes that for an archive older than version 1.4.

Animal Crossing: City Folk sets up its music as each piece starts. Given the game's `main.dol`
(the USA release), revosf2 does the same. Each hourly piece and each piece of town music plays
the tracks that the game plays in fine weather. The game silences the piece's other tracks. Rain
and snow play other sets of tracks. revosf2 converts each piece once more in each. The files are
named `BGM_OUTDOOR_00_rain`, `BGM_OUTDOOR_00_snow` and so on. In the MIDI files, a silenced
track keeps its notes at CC11 0. The museum's music has seven arrangements. The game picks one
by room and starts the music in the first. So does revosf2. It converts the museum's music once
more in arrangements 2 to 6: `BGM_MUSEUM_arrangements_2_and_3`, `_arrangement_4`,
`_arrangement_5` and `_arrangement_6`. Arrangements 2 and 3 play the same tracks. No room plays
arrangement 7. `--weather rain` and `--weather snow` convert the music in one weather instead.
`--museum 2` to `--museum 7` convert the museum's music in one arrangement. Either option leaves
out the other files. CC91 holds the sends that the game's code gives the music. The gyroids'
sounds play in step with the music. The game counts the music's steps. A gyroid's sound plays a
step each time the count changes. revosf2 plays one step of each: the count changes one frame
after the sound starts. revosf2 notes when it plays an archive this way.

Wii Play's minigames mute tracks of their music while it plays. Given the game's `main.dol` (the USA
release, Rev 1, or the European release), revosf2 does the same. Pose Mii's music for play plays
only its first track. The game never lets the other tracks play. In the MIDI files, a muted track
keeps its notes at CC11 0. Table Tennis, Laser Hockey, Billiards, Find Mii and Tanks add or swap
tracks of their music as a game goes on. revosf2 plays each of those pieces with the tracks that
sound as the minigame starts it. It converts the piece once more for each later set of tracks. A
variant's files add its state to the piece's name: `_rally` for Table Tennis's rallies, `_last_30_s`
and `_match_point` for Laser Hockey, `_state_3` and `_state_4` for Billiards, the seconds left for
Find Mii (`_17_or_18_s_left` to `_last_6_s`), and `_arrangement_2` to `_arrangement_22` for Tanks,
in the order of the game's table. Table Tennis plays the rally's tracks for a few bars at a time. In
a match, once a player has 11 points, they stay. Billiards plays its states 2 and 3 at half the
sound's volume (-6 dB). The conversions play at full volume. revosf2 notes when it plays an archive
by Wii Play's code.

Wii Music's main menu plays its music with some of its tracks. The menu's state picks them. Given
the game's `main.dol` (the USA or the European release), revosf2 plays
`RP_MUSIC_MAINMENU_BACKTRACK` with the tracks of the state that the menu starts in. It converts the
piece once more in each of three other states, as `RP_MUSIC_MAINMENU_BACKTRACK_state_1`, `_state_3`
and `_states_5_to_7`. The game names none of the states. Its other states play the tracks of one of
these four. In the MIDI files, a silenced track keeps its notes at CC11 0. On the menu's carousel,
the game plays tracks 1 to 6 at a lower level than the others: 2.6 or 5.8 dB lower, by the stop
that faces the front. The files play them at full level.

Wii Music's Pitch Perfect plays its beats, `RP_KKM_BEATS_LOOP`, with tracks 0, 1 and 3 at its
lowest levels. Higher levels add tracks 4, 5 and 6. Track 2 joins once the round's time reads 10
seconds or less. revosf2 plays the lowest levels' tracks in the beats' files. It converts the beats
once more for each other set of tracks: `RP_KKM_BEATS_LOOP_levels_2_and_3`, `_levels_4_to_6` and
`_levels_7_to_9`, and the same four sets with track 2, as `RP_KKM_BEATS_LOOP_with_track_2`,
`_levels_2_and_3_with_track_2` and so on. In two of its modes, the game plays
`RP_KKM_BEATS_LOOP_MAKE_SCORE` instead. That piece has no tracks 5 and 6. It converts once more as
`_levels_2_to_9`, `_with_track_2` and `_levels_2_to_9_with_track_2`. Pitch Perfect also sends
its phrases and the jingles of its pictures (`RP_KKM_SE_PHRASE_1_*` and `RP_KKM_SE_IMG_*`) to the
reverb. revosf2 adds that send to their CC91. revosf2 notes when it plays an archive by Wii Music's
code.

The Mii Channel silences two tracks of its music, `SEQ_TMP_BGM`, as it starts it. Its scenes let
other tracks play. Given the channel's title (World, version 6), revosf2 plays the music with the
tracks that sound as the channel starts it. It converts the music once more in each of three other
sets of tracks: `SEQ_TMP_BGM_scene_4`, `_menu_out` and `_scenes_1_and_3`. In the MIDI files, a
silenced track keeps its notes at CC11 0. In the plaza, the channel plays the music about 9 %
faster. The files don't. The channel's other sounds send to its reverb. revosf2 adds that send to
their CC91.

The Mii Channel and the Wii Shop Channel (World, version 20) put no effect on the Wii's second
effect bus. Their sequences send to it all the same. The Wii mixes a bus into its output only when
an effect is on it. revosf2 therefore gives CC93, the chorus send, 0 in both channels. revosf2
notes when it plays an archive by either channel's code.

Wii Sports' golf plays its wind, `SE_LV_Wind`, only while the hole has wind. The game writes the
wind's speed and direction to the sequence's variables 0 and 1. Without them, the sequence plays
nothing. Given the game's main.dol (USA, Rev 1), revosf2 plays the wind at a speed of 8 and in
direction 0. `--var 0=N` gives another speed, from 1 to 15, and `--var 1=N` another direction,
from 0 to 7. revosf2 notes when it plays golf's archive by the game's code.

Mario Kart Wii's menus play their music with some of its tracks. The page on screen, or the
section of the menus, picks them. Given the game's `main.dol` (the USA or the European release),
revosf2 plays `SEQ_O_SELECT`, the music of the menus, with its first track alone, as the main menu
does. It plays `SEQ_O_EARTH`, the music of the Wi-Fi menus, with the four tracks that the first of
those menus plays. It converts each piece once more in each other state of the menus:
`SEQ_O_SELECT_state_1` and `_state_2`, and `SEQ_O_EARTH_state_1` to `_state_3`. The game names
none of the states. In the MIDI files, a silenced track keeps its notes at CC11 0. In states 1 and
2, the game plays the first track of `SEQ_O_SELECT` at 0.93 of its volume (0.63 dB lower). The
files play it at full level. Seven of the game's sound effects read their variable 0. The game
sets it each time it plays one. Six of them start with the value of their first play in the
game, from the start of a Grand Prix's first race with controller 1: the ring sounds of Rainbow
Road, the rank sounds and two menu sounds. `SE_RC_RANK_UP` gets 1, the others 0. The gate sound
`SE_CC_MSR_GATE_PASS` keeps -1. The course's file gives its value. `--var 0=N` gives another
value. revosf2 notes when it plays an archive by Mario Kart Wii's code.

WarioWare: Smooth Moves (USA) puts a reverb on effect bus A, or none, as each stage starts. The
microgames' music plays through the reverb of its stage. Given the game's `main.dol`, revosf2
plays each microgame's music as the stage that plays it first does. The music of Young
Cricket's, Penny's, 9-Volt's, Tiny Wario's and Dr. Crygor's stages gets CC91 0 throughout. The
other stages keep the music's sends in CC91. A microgame that a later stage plays again keeps
the reverb of its first stage. revosf2 notes when it plays the archive this way.

Mega Man 9 (WiiWare) plays each piece of its music as four sequences, one for each channel of
the NES's sound chip. The game starts the four together. Their names are the piece's name
followed by `_S1`, `_S2`, `_TR` and `_NO`. revosf2 converts each piece to one MIDI file, one
SoundFont and one set of SFZ files, named after the piece. The MIDI tracks and the SFZ files
take the names of the sequences and their tracks, such as `<name>_S1 Track 0`. The piece takes
the tempo of its first sequence. The other sequences' notes keep their times. The piece's loop
starts on the first pass that all four repeat. `--list` lists the pieces. `--only` picks a
piece by its name or by the name of any of its sequences. While a sound effect holds one of the
NES's channels, the game silences the music's sequence for that channel. The conversion plays
the music without sound effects.

Files use the sequence names, with adjustments for Windows: forbidden characters and trailing
dots or spaces become `_`, and reserved device names such as `CON` get an added `_`. If names
differ only in ASCII letter case, later sequences get a sound-index suffix to avoid collisions
on Windows and macOS. The same applies to a sequence named `samples`. That name would collide
with the SFZ sample folder. The default output folder is `<archive name>_revosf2`; its name
follows the same rules. Use `--output` to choose another location.

The conversion models the Wii's sound library, NW4R SND, and the CPU side of its audio system,
AX.

## Games

An input can be a whole game. revosf2 then converts each sound archive in the game the way it
converts an archive given on its own. Each archive's files go in a folder named after the
archive, inside `<game name>_revosf2` beside the game, or inside the folder that `--output`
gives. Where two archives in a game have the same name, their folders also take the names of
the folders above the archives. The game's `main.dol`, or a WiiWare title's main program, shows
the build of the sound library.

Some games keep their sound archives inside CRI Middleware's CPK archives (`.cpk`). A CPK
stores each file as it is or compresses it with CRILAYLA. revosf2 finds the sound archives in
both forms. It doesn't search a CPK that lists its files by number and not by name.

revosf2 reads three kinds of game:

- **Extracted discs:** a folder with `sys/main.dol` and `files/`, directly or in a `DATA`
  folder. Dolphin's "Extract Entire Disc" and wit extract a disc that way.
- **WiiWare titles:** a folder with the title's TMD (`.tmd`) and its decrypted contents
  (`.app`). A WAD unpacker that decrypts the contents writes such a folder. So does Dolphin:
  Tools > Install WAD installs a title in Dolphin's NAND, under `Wii/title/00010001` in
  Dolphin's user folder. A WAD holds its contents encrypted. revosf2 doesn't read a WAD.
- **Disc images:** ISO, WBFS and WDF images whose game partition is stored decrypted. NKit
  images store it that way. revosf2 can't decrypt a partition. It doesn't read RVZ, WIA, GCZ or
  CISO images either. For such an image, extract the disc with Dolphin: right-click the game
  and choose Properties. On the Filesystem tab, right-click the disc and choose Extract Entire
  Disc. Then convert the folder that Dolphin extracted it to.

Give games on the command line, or drop them onto `revosf2.exe` on Windows. `Revosf2.app` on
macOS takes only `.brsar`, `.baa` and `.aaf` files.

## JAudio games

Some Wii games have no sound archive. They play their sound through JAudio2, the sound library
in Nintendo EAD's JSystem. Twilight Princess, Link's Crossbow Training, New Play Control!
Donkey Kong Jungle Beat, Super Mario Galaxy and Super Mario Galaxy 2 are among them. revosf2
converts their music and sound effects. Give it the game, or the game's start-up archive with
the game's other sound files beside it. The game's files hold three kinds of sound file:

- **The start-up archive** (`.baa`) contains the sound table and the sounds' names, the
  instrument banks and the wave systems. Jungle Beat and the Galaxy games compress their
  start-up archives with Yaz0, in a `.szs` file.
- **The sequence archive** (an RARC archive in a `Seqs` folder beside the start-up archive)
  contains the sequences. The Galaxy games also compress the sequences inside it with Yaz0.
- **The wave archives** (`.aw` files in a `Waves` folder beside it) contain the samples.
  Jungle Beat keeps them in a `Banks` folder.

Given the game, revosf2 puts the files in a folder named after the start-up archive, inside
`<game name>_revosf2`. Given the start-up archive, it puts them in `<archive name>_revosf2`
beside the archive.

The sequences come from the game's sound table, in its order. Sound effects are sequences too.
Files take the names that the table gives the sounds. A sound without a name becomes
`SOUND_<section>_<group>_<index>`. Files of the sequence archive that no sound plays come last.
They keep the names of their files. `--loops`, `--seed` and `--only` work as for a sound
archive. The game's random state starts at 0 unless `--seed` gives another. The game steers some
sequences through ports. revosf2 starts each port at 0, as the game does, apart from the ports
of the effects below. JAudio2's sequences have no variables, and NW4R SND's builds don't apply
to them. `--var` and `--dol` change nothing in them.

Some rules for a game's sound lie in the game's code, not in its files. revosf2 recognizes the
start-up archive of New Play Control! Donkey Kong Jungle Beat, from the USA release. The game's
code mutes some tracks of its music, and it gives some effects a value as it starts them:

- **Muted tracks.** The game starts 45 pieces with some of their tracks turned off. It turns
  some of them on later, as play goes on. revosf2 leaves out the notes of the tracks that are
  off when the piece starts. One piece, JA_BGM_OP_CEMEMONY, starts with every such track off.
  The game turns them on as the piece goes on. revosf2 plays that piece with the tracks of its
  next state.
- **The music's states.** The game turns tracks on or off as play goes on: as a combo grows,
  while a Helibird carries DK, in each part of a boss fight, and while DK swims deep in
  TropicalCoast. revosf2 converts 44 pieces once more in each such state, 102 files in all. A
  file is named after the piece and what sets its state, for example `JA_BGM_MAIN_01_combo1` and
  `_combo2` for the layers of a combo, `_helibird` for a flight with a Helibird, `_round2` and
  `_change` for a boss's second round and the change before it, and `_underwater` for
  TropicalCoast's music while DK swims below. As the menus do, `JA_BGM_COURSE_SELECT_state1`
  plays the map's music with two tracks off. Each file plays its state from the piece's start.
  The game fades from one state to the next, and the files leave out those changes.
- **The beat.** In 62 pieces, one track gives the game the music's beat. The game mutes that
  track. revosf2 leaves its notes out.
- **The chord effects.** The game plays dozens of effects of its instruments in the chords of
  the music. The game's code works out their notes' keys from the music as it plays. revosf2
  doesn't follow that code. Most of those effects come out with no notes.
- **Effects that take a value.** Some effects add a value that the game's code gives them to
  the key of their notes. The game's state decides the value, such as the count of a combo.
  revosf2 plays six effects with the first value that the game gives them. Without it,
  JA_SE_PAUSE_MENU_CLAP_PARAM plays no note.

revosf2 also recognizes the start-up archives of Super Mario Galaxy and Super Mario Galaxy 2,
from the USA releases. Their code changes their sound in these ways:

- **Muted tracks.** The game starts 18 of Galaxy's pieces and 41 of Galaxy 2's with some
  tracks turned off. It turns some of them on later in play. revosf2 leaves out the notes of
  the tracks that are off when the piece starts.
- **The beat.** In 73 of Galaxy's 105 pieces and 83 of Galaxy 2's 127, one track gives the
  game the music's beat. The game mutes that track. revosf2 leaves its notes out. Some pieces
  play nothing else. The game plays them over a stream for their beat. Their files hold no
  notes.
- **Volumes.** The game plays its music at 1.2 times the volume in the sound table. It gives
  each of the effects' 16 categories a volume from 1.0 to 1.5. Galaxy 2 also gives each piece
  a volume in `AudioRes/Info/BgmParam.arc`. revosf2 applies all three.
- **The multi BGMs.** The sound table's MBGM_ sounds play a piece of music with a stream. For
  each of them, the game's code starts a BGM_ piece in their place. Their entries in the
  table all point at one stand-in sequence. revosf2 leaves the MBGM_ sounds out.
- **Luigi's voice.** When Luigi is the player, the game starts each of the player's voice
  effects (`SE_PV_`) with a value at a port that picks Luigi's waves in place of Mario's.
  revosf2 converts each voice that reads the port a second time, as Luigi's voice, with
  `_Luigi` at the end of its name: 75 of Galaxy's 87 voices and 120 of Galaxy 2's 133. Three of
  Galaxy 2's voices play nothing for Luigi, and their files hold no notes.
- **Effects that take values from play.** The game's code writes values for some effects
  from the state of play. The effects of the music notes take their keys from the music that
  plays. Some effects take the code of the map, or values of the scene. revosf2 plays these
  effects with those values at 0.
- **The echo.** The game sets up an echo at start-up. 50 of Galaxy's effects and 268 of
  Galaxy 2's send to it. A SoundFont can't hold an echo. The files hold the send as CC91, the
  send to the player's reverb. A file that sends to the echo lasts until the game's echo
  falls silent.

New Play Control! Pikmin and New Play Control! Pikmin 2 play their sound through the first
JAudio, the version before JAudio2. revosf2 converts their music and sound effects. Pikmin 2
keeps a start-up archive (`.aaf`) in its `AudioRes` folder. Pikmin has none. A bank file
(`pikibank.bx`) in `SndData/Banks` contains its instrument banks and wave systems. One file in
`SndData/Seqs` holds every sequence. The index that divides that file is in the game's code
(`main.dol`). For Pikmin, give revosf2 the game. revosf2 recognizes the USA releases: Pikmin by
its bank file and `main.dol`, and Pikmin 2 by its start-up archive. Another release converts
without its effects and without the rules below.

The game's code holds Pikmin's catalogue of effects. The sound library starts two sequences at
start-up. To start an effect of the system or of the player, the game writes the effect's
number to a port of a track of the first. That track then opens a track below it that plays
the effect. The effects of the game's events go to tracks of the second sequence. revosf2
plays each effect that way and names it after the catalogue. The game's code also plays six
effects that the catalogue doesn't list. Five belong to the system and the player. revosf2
names them `system-<n>` and `player-<n>`, with the effect's number in hexadecimal. The sixth is
the sound of the formation. The game plays it while the player moves the formation. revosf2
plays it for a full move straight ahead and names it `formation`. The game's code also sets
Pikmin's music:

- **Banks.** The game plays each piece with the bank whose number is the piece's number in the
  index. revosf2 does the same.
- **The cutscenes' music.** One sequence holds the music of the cutscenes, in parts. A cutscene
  picks its part with a port of the sequence's first track. The game plays eight of the parts.
  revosf2 converts each of them to a separate file, named after the sequence with a hyphen
  and the part's number.
- **The tracks' volumes.** The game sets a volume for each track of a piece, other than the
  cutscenes' music. Most pieces play at half their volume, one at 0.55 and one at 0.35. Five
  pieces start with some of their tracks off. The game turns some of them on as play goes on.
  revosf2 leaves out the notes of the tracks that are off when the piece starts.
- **Volumes as they are.** Pikmin's sound library plays a track's volume as it is. The later
  form of the first JAudio squares it. CC7 therefore holds the square root of the volume.
  revosf2 does the same.
- **The level of the mix.** The game's code sets the level of its whole mix at start-up. The
  level comes to 0.599 of the level that revosf2 gives the other games of the first JAudio,
  about 4.5 dB below it. revosf2 plays all of the game's notes at that level.

Pikmin's effects play louder against its music than in the game.

Pikmin 2 plays each piece of music through an object of the game's sound layer. The object
answers the piece's calls to the game. revosf2 follows these rules of the game:

- **The calls.** A piece's first track asks the game for the time between its beats, and a
  boss's music asks for its start. The game answers with 60 ticks, or 30 in two pieces. A middle
  boss starts m_boss at part 9, and the final boss starts l_boss at part 1. revosf2 gives the
  same answers.
- **The directors' tracks.** Some tracks of a piece follow the game's action, such as an enemy
  nearby or a treasure carried. The game mutes them as they start. revosf2 leaves out their
  notes, so each piece plays as it does with nothing going on.
- **The cave music.** A cave's piece has a conductor. The conductor picks the phrases and rests
  that each track plays, at random, and sets the track's instrument, volume and pan. revosf2
  plays each piece with the first conductor of its family, such as caveconc_00_0. It draws the
  random numbers as the game does from the start. Such a piece plays until each track reaches
  its last part. The file has no loop: the game picks the phrases at random from then on. The
  game's echo leaves quiet tails between the notes. MIDI and SoundFont can't describe an echo,
  so the files don't hold them.
- **The volumes.** The game plays each piece at a volume from a list in its files. revosf2 does
  the same.

As JAudio2's does below, Pikmin 2's music plays about 0.08% slower on the Wii than in the files.

Many of Pikmin 2's effects play quieter than in the game. Pikmin plays some effects from waves
in 2-bit ADPCM. revosf2 decodes them.

JAudio2 times its music for the GameCube's rate of about 32,028 Hz. The Wii plays it at
32,000 Hz. There the music plays 0.089% slower and 1.5 cents lower than in the files.

Twilight Princess gives its categories of sound effects their volumes at start-up. Category 1,
Link's voice, keeps the volume of the sound table. The others play at 120/127 of it. revosf2
does the same. During a conversation and in the pause menu, the game plays most categories
quieter for a while. revosf2 leaves that out.

As Twilight Princess starts five pieces of music, it mutes some of their tracks. Hyrule Field's
day music holds two arrangements. One plays while Link walks, and the other while he rides.
revosf2 converts the piece twice and leaves out the muted tracks' notes. `Z2BGM_FIELD_LINK_DAY`
holds the walking arrangement and `Z2BGM_FIELD_LINK_DAY_riding` the riding one.
`Z2BGM_TOAL_VILLEGE`, `Z2BGM_HORSE_BATTLE`, `Z2BGM_BOSS_SNOWWOMAN_1` and `Z2BGM_VS_GANON_02`
leave out the tracks that the game mutes as they start. Later, the game's scenes, fights and
other code change the tracks of 17 pieces. Some states play a track at part of its volume, such
as Ordon Village's tracks at 0.65. The piece's file plays the state that the game starts the
piece in, such as the state of a dungeon's first room. revosf2 converts each later state as one
more file of the piece. Its name adds the status that the game gives the piece, such as
`Z2BGM_TOAL_VILLEGE_status1`. The battle music's states follow the fight: `_near`, `_searched`,
`_near_searched` and `_port9_1`. Hyrule Field's day music also follows the field's code: what
Link does, the enemies near him and the time of day. revosf2 converts each way through the piece
as one more file in each arrangement. `_status12` plays the sections of a long walk or ride.
`_status4` to `_status7` play a fight's sections by their four entries. `_status10` and
`_status11` play the two endings of a day. `_status8` and `_status9` play the starts that the
field's code makes once no music plays. `_riding_status3` plays the section of Link standing on
his horse. The game moves the tracks to a new state gradually. revosf2 plays each state from the
start and leaves out the states in between.

Link's Crossbow Training runs a later build of Twilight Princess's sound code. The game plays
the sounds of the surroundings (category 9) at 100/127 of the sound table's volume. The other
categories play as in Twilight Princess. As the same five pieces start, the game mutes the same
tracks. revosf2 does the same. Some of the game's sound effects read values that the game
passes to them as they play, such as information about the map. revosf2 plays them with the
value 0. The game passes 0 when it has no other value.

Both games set up an echo at start-up. A sound that sends to it repeats about every 160 ms. Each
repeat has half the level of the one before. MIDI and SoundFont can't describe an echo. The MIDI
file of a sequence that sends to it ends when the echo falls silent. Looping and held sounds keep
their ends.

Programs 0xf0 and up play the DSP's built-in oscillators. The square wave, the sawtooth, the
pulse of 25% duty, the triangle and the sine become looping samples of one cycle, one for each
program. Each preset of such a program has one zone, for every key and velocity. The zone holds
the oscillators' envelope: full volume until the release, then a fall to silence over about
17 ms. The DSP changes its noise and its ramp as it plays. No sample can hold them.

A track can pass its notes through an IIR filter. In music, each region of a filtered track
becomes two zones. One zone plays the region unfiltered. The other plays it through a low-pass.
Their levels are fitted so that together they follow the filter's curve. The two zones double
the voices of the filtered tracks. A track whose filter changes from note to note stays
unfiltered. So does a filter that changes no band by 0.3 dB or more, and every sound effect's
filter. revosf2 reports the filters that it leaves out.

MIDI and SoundFont can't hold everything that JAudio2 does. The fitted envelopes can't follow an
oscillator's table that rises again or loops. Random instrument effects play at their base
values. Tremolos, pitch envelopes and notes on the DSP's noise, ramp and other oscillators that
no sample can hold are left out. A vibrato deeper than a semitone is kept to a semitone. revosf2
reports each of these for each sequence. The game plays a limited number of notes at once.
revosf2 keeps every note.

## Downloads

The Releases page has revosf2 for 64-bit Windows. On macOS on Apple Silicon, build it from
source (see [Building](#building)).

## Drag and drop

- **Windows:** drop one or more `.brsar`, `.baa` or `.aaf` files onto `revosf2.exe`, or games: their
  folders or disc images. The console shows the results and waits for Enter before closing.
- **macOS:** drop archives onto `Revosf2.app`, or open it to choose files. The app shows the
  results and offers to open the output folders. It wraps the command-line program because
  Finder cannot drop files onto a bare executable. The app is included in the
  [build](#building). It takes only `.brsar`, `.baa` and `.aaf` files. Convert games from a terminal.

An unreadable archive reports an error without stopping the others. If two inputs would use the
same output folder, the second is skipped to protect the first's files. This can happen when
names differ only in extension, or only in case on a case-insensitive filesystem. Filesystems on
Windows and macOS are usually case-insensitive.

## The conversion

### Sequence playback

A sequence (RSEQ) is bytecode, with variables, conditions, random arguments, calls, jumps, loops
and tracks that start other tracks. revosf2 executes it and records the resulting notes and
controllers. The performer models NW4R SND's sequence player, including:

- All commands and their variable, comparison, random and `if` forms.
- Note lengths, damper, tie, `notewait off` and waits for sounding notes to finish. Note ends
  are calculated frame by frame. AX starts a voice one frame after note-on. NW4R SND reads a
  one-shot wave's end one frame after AX passes it. One-shot duration follows the pitch from the
  note's sweep, track bend and pitch LFO, recalculated each frame. Envelopes start at -90.4 dB
  and first advance at frame end; notes released or stopped before then are inaudible and
  produce no MIDI note.
- Tick scheduling in single-precision milliseconds, once per 3 ms sound frame. The game's
  rounding occasionally places a tick in an adjacent frame.
- The game's random generator, including its once-per-frame advance. Random choices therefore
  depend on timing.
- Main-loop detection and MIDI loop markers. The main loop belongs to the lowest-numbered track
  that can still advance. A pass qualifies only if earlier tracks make no contribution. Tracks
  that wait forever, on held notes or for a negative number of ticks, do not block this choice.
  A pass that changes nothing, such as one that only polls or counts a variable, qualifies only
  while no other track changes the sound or plays on. Setting a variable that another track
  reads counts as a change. A tie or mono legato on a note below hearing (see below) changes
  nothing that can be heard. A track that has sounded nothing and changed nothing for 10 seconds
  doesn't count as playing on. A sequence whose tracks all reach their end has no main loop.
  Backward jumps to unvisited commands and passes without waits, such as random retries, are not
  loops. Inside a subroutine, a jump back isn't a loop when its pass played no note that can be
  heard and ran a conditional jump, call, return or end that could leave the loop. A tie or mono
  legato on a note below hearing (see below) plays nothing that can be heard. A held note that
  sounds until the game stops it keeps such a loop. A loop that a track runs inside a pass
  of another loop, such as a countdown of waits between notes, is an inner loop. An inner loop's
  passes don't qualify until the track has played no pass of another loop for 10 seconds. A main
  loop in which no note starts isn't a loop either once every earlier note has ended or is
  fading out. The sequence then ends with its last sound. The markers go on the first pass that
  a player looping on them repeats as the game plays the next one: the same notes at the same
  times, with the same settings and tempo. The first pass often differs. Setup before the loop
  can land in its first tick. The game can keep a value from pass to pass. Later passes may only
  tie again a note that the first pass strikes. If no pass so far repeats that way but the next
  pass does, the performance plays that pass too. Otherwise the markers stay on the first pass.
  They also stay there in a loop that draws random numbers. At the loop's start, the MIDI file
  sends each controller, pitch, preset and tempo that the game gives back there when the loop
  ends with another value. Without those events, a player that loops on the markers would keep
  the loop end's value.

NW4R SND changed how it plays sequences after its builds of 2006, and again in 2008 and 2009.
The game's `main.dol` shows which build it links. The sound library's banner dates the builds
from 2007 on. A build without a banner plays as the builds of 2006. Some of those come with a
later AX. AX of 2006 moves 0x120-byte parameter blocks, and every later AX moves 0x140 bytes.
revosf2 plays an archive as that build does. Without a
`main.dol`, an archive of version 1.4 or later plays as the builds from June 2008, and an older
archive as the builds of 2007. The archives of the builds of 2006 and 2007 have the same
versions. Some games from 2008 also keep an archive of an older version, such as the archive of
the Home Button menu.

The builds from March 2008 add time-prefixed volume and pan moves, the envelope's hold,
monophonic tracks, regions that ignore note-offs, key groups, a finer `sweep_pitch` and a tempo
limited to 1023. The builds from June 2008 add the velocity range, the biquad filter and the
envelope reset. The builds from November 2008 take the track low-pass's cutoff from a table. The
table's steps are about a third of an octave apart, from 80 Hz to 12.8 kHz. A setting between
two steps takes the lower step's cutoff. The builds from September 2009 move a time-prefixed
pitch bend to its value over the prefix's ticks. They also let a note's volume and envelope go
above full level (see "Volume limits" below). The builds of 2006 count ticks in 416ths, and have
no timebase. A tick there is 0.16% shorter than at the usual 48 ticks a quarter note. They play
stereo waves in dual mode. At the centre, that's 3 dB louder than balance mode. They also raise
a pan of 1 to 2 and stop a released note once its volume falls to 0. With AX of 2006, they have
64 voices.

Short loops, such as repeating sound effects, run until the MIDI is at least 10 seconds long.
Indefinitely held sounds are capped at 10 seconds after their last change. A note of 9,600 ticks
or more that its track waits out counts as held. Games use such notes, minutes long at a typical
tempo, for sounds that they stop themselves. The cap also covers a main loop longer than 10
seconds that ends with more than 10 seconds of no change and sounds only endless notes held with
tie or mono. The game's next pass usually continues those notes. A sequence that finishes
naturally ends with its last sound, including release. So does a sequence whose tracks all wait
forever. A note counts as below hearing once its envelope has decayed 40 dB. A note that plays
until its track releases it, such as a tie, ends where its envelope has decayed 60 dB. A held
note in a sequence whose tracks all wait forever ends there too. If the sustain level stays
above that, the note ends at 40 dB down. The game's channel goes on in silence. Performances
stop after 15 minutes; one that has been silent for more than 10 seconds by then ends with its
last sound too.
Tempo or timebase 0 stops sequence timing while notes keep playing; those notes finish normally,
or use the same 10-second limit if held indefinitely. A track that never waits would hang the
game; revosf2 stops it and reports it.

### MIDI

Each sequence track gets a MIDI track and channel of its own. In a piece of several sequences,
each sequence's tracks with notes get them, in the order of the sequences. Channel 10, reserved
for drums in General MIDI, is allocated last. SoundFont synths use bank 128 for that channel.
revosf2 copies that channel's presets to bank 128.

MIDI output preserves tempo changes and uses at least 7680 ticks per quarter note, with an
integer number of MIDI ticks per sequence tick. The game processes ticks in 3 ms sound frames,
and AX applies their changes at the next frame boundary. revosf2 places events at the start of
the frame containing each tick, rounded to the nearest MIDI tick. This shifts the whole sequence
one frame earlier while preserving event spacing; at 120 BPM, rounding error is at most 1/30 ms.
A moved event never crosses a loop marker. The loop's contents don't change. A tempo change on
the loop's first tick comes after the loop marker. A player that loops then plays every pass at
that tempo. A sweep or portamento given a portamento time moves on in every frame, between ticks
too. Its pitch bends go at the start of each frame. After its track closes, the sweep goes on
while the note's release sounds. It also goes on after the sequence's last tick, at the last
tempo. Once a released note falls 40 dB below full level, its sweep no longer bends the channel.

NW4R SND applies track settings to notes at frame end. A note therefore starts with its tick's
final settings, and MIDI controller changes are written before notes at the same tick.

| Game setting | MIDI output |
|---|---|
| Track volume, up to 127 | CC7 |
| Track volume above 127 × second volume × main volume × sound volume, up to full level, plus legato velocity changes without region volume | CC11; CC7 and CC11 both act squared, matching NW4R SND. A note's zones take the volume above full level. After a legato, values are scaled for sequences exceeding MIDI's range (see [Differences from the game](#differences-from-the-game)). |
| Track pan | CC10, mapped to the SoundFont pan law |
| Pitch bend, sweep, portamento and legato key changes | Pitch bend, with RPN 0 range set per channel |
| LFO depth | CC1 = depth / 2, mapped to vibrato or tremolo in the SoundFont |
| Effect sends A and B | CC91 and CC93 (reverb and chorus). A send that the game's code gives the sound adds to them. |
| Immediate track silence | CC120 |
| A track that the game's code silences | CC11 0 throughout. The track's notes stay. |
| Low-pass filters that change during sounding notes | CC102, mapped to SoundFont cutoff by a modulator |
| Track low-pass (0xd8), for SFZ | CC103 = 64 minus the argument; values <= 6 mean off |
| Biquad filter (0xb4/0xb5, builds from 2008), for SFZ | CC104 = filter value; 0 means off |
| Instrument and track envelope, LFO, initial pan and filter settings | Bank select (CC0) and program change |

A MIDI channel can hold only one note per key. Repeated strikes that overlap, common in drums,
get extra channels called layers, with matching controllers, up to the 16-channel limit.

A MIDI channel has one set of controllers for all its notes. The game's notes each keep their
settings. Notes that the game plays differently from the track's later notes therefore require
channels of their own. Two kinds of note do: release tails that the track has detached, and
notes still sounding when a note starts that sweeps, starts at a raised velocity or requires a
level of its own. While such notes sound and channels remain, the track goes on to another
channel. A note whose pitch LFO changes while it sounds goes on a channel of its own too (see
"LFO settings"). Each channel gets a MIDI track named after the sequence track. Layers get such
tracks too.

### SoundFont

Each distinct combination of bank instrument and track settings becomes a preset. Instrument
regions become zones with:

- Key and velocity ranges resolved by NW4R SND's key and velocity tables.
- The region's sample, root key, tuning, volume and pan.
- Envelope parameters after track overrides. NW4R SND's attack is exponential in decibels; the
  linear SoundFont attack is fitted to it at 0.55 of its duration. SoundFont decays and
  releases fall 100 dB in 101.6 s at the slowest, and the game's slowest take up to six times
  as long. Such a decay gets a longer hold, up to SoundFont's limit of 18 s: half the time by
  which the game's decay reaches its sustain level later. The note is then as far above the
  game's level at the end of the hold as below it when its decay reaches the sustain level.
  Such a release plays at SoundFont's slowest. The SFZ files keep the game's times.
- From the 2008 builds, the region's hold. A note of a key group releases the others through
  their MIDI note-offs, at their release rates.
- Loops repeated as needed to meet SoundFont padding rules: 8 samples before, at least 32
  within, and 32 after. This preserves playback.
- Continued playback for regions that ignore note-off. Their MIDI notes end when the game's
  notes would fall silent, with release settings that also preserve decay after a repeated
  strike.
- Stereo waves in balance mode, with samples scaled to match. Balance mode keeps each channel on
  its side, 3 dB below full level at the centre. The builds of 2006 have only dual mode.

revosf2 reads RSAR archives of versions 1.0 to 1.4 and the RSEQ, RBNK, RWAR and RWAV files in
them. A bank's waves are either an RWAR of RWAV files (RBNK 1.2) or raw samples described by the
bank (RBNK 1.1). Supported wave encodings are PCM8, PCM16 and DSP-ADPCM. Files are found in the
archive's groups. Truncated archives convert as far as their data goes, and the sequences they
lack are reported.

## SFZ files

SoundFont has one two-pole low-pass filter per voice. That filter only approximates the game's
one-pole track low-pass, and can't reproduce the high-pass and band-pass presets of the biquad
filter. SFZ supports all of these filters. revosf2 also writes these SFZ files:

- `sfz/<sequence>/Track <n>.sfz`: instruments for MIDI `Track <n>` and its layers, for each
  active sequence track.
- `sfz/samples/`: 16-bit WAV samples shared by all sequences in the archive. Stereo channels
  are separate files, panned left and right.

SFZ players ignore MIDI channels, and each file contains one instrument. Load the MIDI in a DAW
and assign an SFZ player, such as sfizz, to each track with its matching file. The player tested
is sfizz 1.2.

Presets use program change, plus bank select (CC0) when a sequence has more than 128 presets.
Regions match the SoundFont, with these differences:

- **Filters.** sfizz's `lpf_1p` matches AX's one-pole low-pass, coefficient for coefficient at
  AX's 32 kHz. `lpf_2p`, `hpf_2p` and `bpf_2p` match the biquad presets of the builds from 2008
  the same way. When both filters are active, they run in series. CC103/CC104 and SFZ curves map
  the track's settings to cutoff, resonance and gain, including changes during notes. An
  inactive low-pass stays at sfizz's maximum 20 kHz cutoff, attenuating 10 kHz by about 0.5 dB
  and 16 kHz by 1 dB. SFZ has no biquad bypass like the game's value 0 or absent preset.
  Instead, CC104 selects filtered or unfiltered regions at note-on, and that choice lasts for
  the note. As in the game, the choice is the one that the note's sound frame ends with. Each
  track uses its first biquad type. Later types are omitted and reported.
- **Envelopes.** As in the game, decays and releases fall linearly in dB. The documentation's
  curve falls by a factor of e^-8 over the specified time. That's 69.49 dB. They use the game's
  time for that fall. Attack uses the SoundFont fit described above.
- **LFO.** SFZ uses a sine, matching the game, and supports pan LFO.
- **Pan.** A curve maps CC10 to `(value - 64) / 64`, the SoundFont convention that the MIDI
  output uses.
- **Level.** SFZ region `volume` restores any gain lost when scaling MIDI CC11. Playback then
  keeps the game's level.
- **Controllers.** CC7, CC10 and CC11 act at once, without sfizz's default 10 ms smoothing. A
  note whose track sets its volume or pan in the note's starting tick begins at that level and
  pan.
- **Loops.** sfizz crossfades the loop end with data before the loop start over at least 1 ms.
  Samples include 4 ms of repeated loop data before the start. The crossfade is then inaudible.

Five sfizz 1.2.3 quirks affect playback:

- **Decays and releases.** sfizz ends them sooner than the documentation's curve. Over the
  specified time, sfizz falls by a factor of e^-9: 78.17 dB. Where the game has fallen 40 dB,
  sfizz has fallen 45 dB.
- **Sample rate.** Loading a file configures its filters for 48 kHz. A subsequent sample-rate
  change corrects them. At other rates, load files before setting the rate. `sfizz_render` and
  `sfizz_jack` set it first; `sfizz_render` defaults to 48 kHz unless told otherwise.
- **Streaming.** During offline rendering, streamed samples can fall silent after their
  preloaded portion. The files request full loading with `hint_ram_based=1`. sfizz prints
  "Unsupported value for hint_stealing: 1" for this; the message is harmless.
- **Mono samples.** sfizz pans a mono sample by the sin/cos law and then raises both sides by
  3 dB. Every sample of the files is mono. Every region therefore plays 3.0 dB above the game's
  level, at every pan. The balance of the mix stays the same. Lowering sfizz's volume by 3 dB
  gives the game's level.
- **Short decays and releases.** sfizz makes each decay and release last at least 6 ms. The
  documentation sets no such floor. A shorter time plays over 6 ms.

The opcodes follow their documentation at sfzformat.com, with these exceptions:

- **Long decays and releases.** A decay or release can last longer than the documented limit of
  100 s. The game's envelope falls that slowly. sfizz 1.2 plays the time as written.
- **Long attacks.** A note that the game releases during its attack gets a longer attack (see
  "Differences from the game"). That attack can last up to 101.6 s. The documented limit is
  100 s. sfizz 1.2 plays the time as written.
- **Loud regions.** A region's `volume` can lie above the documented limit of 6 dB. sfizz 1.2
  takes volumes up to 48 dB.
- **Low resonances.** The game's biquad presets have resonances down to -49 dB. The documented
  range starts at 0 dB. Each biquad track's curve reaches resonances below 0 dB. sfizz 1.2 plays
  those resonances.
- **A wide resonance amount.** Biquad type 5 has steps down to -49 dB. A track of that type
  moves its resonance in units of 50 dB. The documentation gives the amount a range of -40 to
  40 dB.
- **High cutoffs.** A region pitched far up can get a low-pass cutoff above half the output's
  sample rate. The documentation gives half the rate as the limit.

## Differences from the game

revosf2 reports approximations for each sequence. Some behavior cannot be represented exactly
in MIDI, SoundFont or SFZ:

- **Per-note pitch.** Game sweeps, portamento and legato key changes affect individual notes.
  MIDI pitch bend affects the whole channel. A note that sweeps beside another note still
  sounding goes on another channel (see "MIDI").
- **Legato.** Tie and mono normally continue the existing note. Pitch bend gives the key changes,
  and CC11 the velocity changes. A note that legatos take above its velocity starts at the
  highest velocity they reach. CC11 lowers it until then. That works while the instrument plays
  the same region at that velocity. Past that, CC11 goes above 127 (see "Volume limits"). CC11
  would lower any other note on the channel too. Beside another note's release, such a note goes
  on another channel (see "MIDI"). When no channel is left, a rising legato requires a new MIDI
  note. A changed envelope or LFO preset always does. The new note restarts the sample and
  envelope. The game would continue them.
- **Attack.** The linear SoundFont attack rises sooner than the game's. It's louder for the
  first quarter of the attack, by up to tens of decibels at its very start. A note that the game
  releases during its attack gets a longer attack of its own preset. That attack reaches the
  game's level when the note is released. The release then starts at the game's level.
- **Detached notes.** Toggling tie or mono, muting with release or restarting a track detaches
  its sounding notes. They finish releasing with their old settings. In MIDI, they keep those
  settings on a channel of their own (see "MIDI"). When no channel is left, they stay on the
  track's and receive later changes. Changes affecting a detached note above -40 dB are
  reported.
- **Combined pan.** NW4R SND adds region pan, track initial pan and track pan before applying
  its pan law. Synths receive track pan separately through CC10 and combine it with zone pan by
  their law.
- **Pan curves and modes.** revosf2 always uses the default curve. In the game, a track pan on a
  stereo wave in balance mode changes the level of each side. revosf2 pans the channels as in
  dual mode instead. Both are reported.
- **LFO settings.** Notes keep the speed, range, type and delay their track has at the end of
  their note-on's tick. That's when the game first gives them to the note. The game updates
  these during playback. A note whose pitch LFO changes while it sounds plays on a MIDI channel
  of its own, with a preset that has no LFO. The channel's pitch bends follow the game's LFO in
  every sound frame. When no channel is left, the note keeps its note-on settings. revosf2
  reports these notes and the notes whose volume or pan LFO changes while they sound. Depth
  changes otherwise work through CC1.
- **Deep tremolo.** The game's volume LFO can lower a note as far as silence but raise it by no
  more than 6 dB. The SoundFont and SFZ LFOs swing as far up as down. A volume LFO deeper than
  6 dB is kept to 6 dB and reported. It then dips less than in the game. Before the builds of
  September 2009, the game also never takes a note above full level. While the note's envelope
  is at full level, the upper half of the swing changes nothing. The SoundFont and SFZ LFOs
  can't stop at full level. A note that the game holds at full level gets a preset whose smaller
  swing and lower level are fitted to the game's level. A note at full level throughout swings
  half as far around a level that is lower by the swing over pi. revosf2 reports those notes.
  From September 2009, the game lets the swing pass full level, as the SoundFont and SFZ do.
- **SoundFont filters.** The two-pole low-pass matches the biquad low-pass of the builds from
  2008. It approximates the one-pole track filter: it passes slightly more just above cutoff and
  less in the top octave, and its slope is twice as steep. ntdlib's
  `tools/sequence_export/filter_tables.py` fits a cutoff to the one-pole's curve. A zone keeps
  that cutoff where its sample plays within 1 dB of the game's level at every key that the
  sequence plays. Otherwise the zone takes the cutoff at which FluidSynth passes as much of the
  sample's power as the game's filter. That cutoff can follow the key. The sample then plays at
  the game's level. Its spectrum can differ more from the game's. The biquad's high-pass and
  band-passes are omitted and reported. Two active low-passes use the lower cutoff. SFZ has all
  these filters.
- **Routing.** Surround pan, main send and effect send C are omitted. Pan LFO is omitted from
  SoundFont but included in SFZ. Effect sends A and B become CC91 and CC93. Synths play them as
  reverb and chorus. In the game, a send reaches whatever effect the game puts on that bus in
  that scene: any of NW4R SND's ReverbHi, ReverbStd, Chorus and Delay, or none.
- **Volume limits.** Before its builds of September 2009, NW4R SND never plays a note above full
  level, however high its volumes go. It limits the product of the velocity squared, the
  region's volume and the track's volumes. Track volume, second volume and main volume can reach
  255. So can a velocity read from a variable. MIDI's velocity stops at 127. CC7 and CC11 carry
  the track's volumes up to full level. A note's preset raises its zones by the rest, up to full
  level. A note whose level still differs from the game's by more than 0.1 dB is reported. That
  happens when a zone reaches full level before its note does, or when a track's volumes change
  during a note. After a legato, CC11 sets the note's level and can pass 127. All CC11 values
  are then scaled equally. That keeps the balance between tracks and lowers the sequence's
  level. revosf2 reports the reduction. SFZ restores the MIDI gain. The builds from September
  2009 let a note's volume pass full level. revosf2 still plays such a note at full level, and
  doesn't report it.
- **AX output.** AX resamples with 4-tap filters whose coefficients depend on the pitch ratio.
  It darkens a voice playing more than 4/3 of its samples per output sample: a 44.1 kHz sample
  at its recorded pitch loses about 3 dB at 9 kHz. Synths interpolate differently. The zones and
  SFZ regions of notes that play more than one sample per output sample therefore take
  key-tracked low-passes fitted to AX's filters. In FluidSynth, the fits leave an error of about
  1.3 dB over the top two octaves. A SoundFont zone holds one low-pass. A zone that also has the
  track filter's fitted cutoff takes the lower of the two at its original key. AX also runs a
  compressor on the final mix. While the mix would pass full scale, the compressor turns the
  whole mix down. A synth plays such a passage louder, or clips it.
- **Voice limits.** AX has 96 voices, or 64 in AX of 2006. Synths usually have more.
  With all of them in use, the game gives a new note the voice of the lowest priority. AX also
  stops the voices of the lowest priority when a frame would take more DSP time than AX allows.
  revosf2 plays every note to its end. If a track starts more than 1,024 notes in one tick,
  revosf2 keeps the first 1,024. The game would keep the newest. After as many legatos in one
  tick as AX has voices, the MIDI note continues with its existing preset and no gain above its
  starting level. Legatos that would otherwise require a new note are reported.

SoundFont output follows the specification, apart from the zones' attenuation and the biquad
low-pass's resonance. Player differences remain:

- **Level.** FluidSynth runs the SoundFont low-pass filter on every note. That includes notes
  whose filter is open. The filter divides the level by the square root of its Q. FluidSynth
  converts the default resonance of 0 dB to a Q of 0.707. Every note therefore plays 1.5 dB
  louder than the SoundFont specifies. Multiplying FluidSynth's gain by 0.84 gives the specified
  level.
- **Pan.** Pans are mapped from NW4R SND's square-root law to FluidSynth's sin/cos law.
  TinySoundFont uses a square-root law, and its pans come out slightly narrower.
- **Envelopes.** Times follow the specification's 100 dB range. FluidSynth uses 96 dB. Decays
  and releases then fall 4% slower in decibels, and sustain levels are 4% shallower: -11.9 dB
  plays as -11.4 dB. FluidSynth also slows any release faster than 15.6 ms (-7200 timecents)
  to that time.
- **Attenuation.** FluidSynth, BASSMIDI and the E-mu chips apply 0.4 of a zone's initial
  attenuation. The specification calls for all of it. revosf2 therefore writes 2.5 times a
  zone's attenuation in the generator, up to the generator's limit of 1440 cB. Those players
  play that limit as 57.6 dB. A modulator without a source holds any attenuation beyond 57.6 dB.
  FluidSynth 2.4.5 and later apply that modulator. BASSMIDI and earlier versions of FluidSynth
  ignore it. A player that follows the specification plays every attenuated zone too quietly.
  TinySoundFont plays a quarter of the attenuation. Before 27 January 2025, TinySoundFont played
  2.5 times the attenuation.
- **Low-pass level.** In FluidSynth, a note through the SoundFont's version of the biquad
  low-pass plays 0.85 dB below an unfiltered one. The game's filter lowers it by 0.5 dB.
- **Resonance.** FluidSynth, BASSMIDI and the E-mu chips play a resonance of r dB as r - 3 dB at
  the cutoff. The specification calls for r dB. revosf2 writes the biquad low-pass's resonance
  for those players. A player that follows the specification plays 3 dB more at the cutoff.
- **Pitch.** FluidSynth rounds pitch down to whole cents. Fractional bends can be almost a cent
  flat. A long note's loop can then drift by up to 35 ms per minute against the game. The
  beating of notes detuned by less than a cent changes too.
- **Modulators.** BASSMIDI 2.4.16 ignores the CC1 and CC102 modulators. CC1 then gives the
  specification's default vibrato in every zone: 50 cents at CC1 127. A tremolo becomes that
  vibrato. Filters that change during a note stay open. TinySoundFont lacks SoundFont modulator
  support. CC1 LFO depth and CC102 filter changes have no effect there.
- **Tempo changes.** FluidSynth rounds its position to the nearest MIDI tick at each change
  and restarts its clock there. This can advance playback by half a tick per change: 1/30 ms
  at 120 BPM with 7680 ticks per quarter note. TinySoundFont rounds tempo-change times down to
  milliseconds. Its playback runs ahead by about half a millisecond per change on average.
- **Note timing.** FluidSynth applies MIDI events at the start of each 64-sample render block.
  Notes can start up to 64 samples late, about 1.5 ms at 44.1 kHz. Notes that play the same
  wave at the same pitch add up differently when their start times move.
- **Short notes.** FluidSynth holds every note for at least 10 ms (`synth.min-note-length`).
  Shorter notes therefore sound longer than in the game.

## Building

Requires C++20 and CMake 3.20 or later, with no other build dependencies. revosf2 supports
macOS on Apple Silicon and Windows x64 with clang-cl. CI builds on both. The tests run in CI
on Windows only.

- **Windows:** install Visual Studio 2022 with "Desktop development with C++", including CMake.
  Add the "C++ Clang tools for Windows" component. Use Developer PowerShell or Developer Command
  Prompt.
- **macOS:** install Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake` or the installer from cmake.org).

The libraries seqlib, xsflib and ntdlib are git submodules in `externals/`. Clone with
`git clone --recursive`, or run `git submodule update --init` in a checkout.

From the repository root:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

On Windows, the first command is `cmake -S . -B build -A x64 -T ClangCL`. It selects clang-cl
and 64-bit output.

The executable is `build/revosf2`, or `build/Release/revosf2.exe` with Visual Studio. Xcode and
other multi-configuration generators also use `build/Release`. The Windows build links the
runtime statically. The executable requires no runtime DLLs. CI uploads the Windows executable as an
artifact. macOS builds also produce `build/Revosf2.app`, containing a copy of the program; you
can move the app elsewhere.

C++ formatting uses clang-format with the repository's `.clang-format`.

## Tests and tools

`ctest` checks the build of the sound library that a game's `main.dol` shows and the archive
settings that follow from it, the low-pass of the builds from November 2008, the set-up of
Animal Crossing: City Folk's music and gyroids, the tracks of Wii Play's music, the set-up of
the sounds of the Mii Channel, the Wii Shop Channel, Wii Sports and Mario Kart Wii, and the
search for Mega Man 9's pieces by their sequences' names. It checks which inputs are games, the
messages for disc images that revosf2 can't read, and the output folders of games. It checks
that the program reports its version, and rejects a variable that the game doesn't have and a
`--dol` file without AX's microcode. It also rejects a weather and a museum arrangement that
City Folk doesn't have. It converts a small JAudio2 game that the test program writes, from its
start-up archive and from an extracted disc. The conversion's unit tests are in the seqlib and
ntdlib libraries. They run on synthetic files, without game data.

Other checks and tools:

- `-DREVOSF2_TEST_ARCHIVE=path/to/archive.brsar` adds the conversion of every sequence in that
  archive. Independent readers check each output file.
- `python3 externals/ntdlib/tools/sequence_export/filter_tables.py revolution <main.dol>`
  derives the SoundFont and SFZ filter tables from a locally supplied Wii Music `main.dol`
  (`sys/main.dol` in the game's files). Requires Python 3 and numpy.

These development tools require game files supplied separately. revosf2 doesn't distribute those
files. Normal conversion requires only the input: a `.brsar` archive, a game or a JAudio2
game's start-up archive.

## Licence

revosf2 is under the [MIT](LICENSE) licence. The libraries it builds on, seqlib, xsflib and
ntdlib, are under the MIT licence too. The one exception is a table in ntdlib made from Unicode's
data. It's under the Unicode License v3. `THIRD-PARTY-NOTICES.md` gives their licence texts. Game
data is not covered by this licence.
