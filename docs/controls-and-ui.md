# Controls and screen

M8 keys: arrows, SHIFT, PLAY (Start), OPT (= **B**), EDIT (= **A**), power.
Keys are on/off only (no pressure). The full key list is the header comment of
`m8_looper.ino`; this page adds the details behind it.

## Keys

| Keys | Action |
|---|---|
| SHIFT and B with the arrows | Exclusive, for the arrows and the looks (not the action button): the one pressed last is the arrows' modifier (its arrow actions, its highlights); let go, the other one, if still held, takes over again (`arrowModifier()` in input.cpp) |
| UP/DOWN | Select row: tracks 1 (top) to 7, then the status row (no wrapping); starts on track 1. A held arrow (repeating) stops at the top or bottom row: only a fresh press there switches. Any arrow off an edge switches once let go, or, still held, after 0.6 s (`SWITCH_HOLD_MS`); held on, it does nothing more (no repeats on the other screen) until let go. UP on track 1 or DOWN on the status row (a fresh press) opens the menu once let go (landing on its bottom item / top card row); in the menu, UP on the top card row (A–D) or DOWN on the list's last item goes back once let go (landing on the status row / track 1, at the column that row was last left on; with no project loaded: a new one in the first empty slot, the menu's selection staying where it was, as A there). (The notes screen, ui.h, isn't reached for now: every edge leads between main and the menu.) |
| LEFT/RIGHT | Select cell: waveform, filter, volume (status row: scene, clock, tempo; UP/DOWN between the tracks keeps the column, across empty tracks (one cell) too; the tracks and the status row each remember their own: going to the other, the column it was last left on (the first time: the nearest, the scene cell below the waveform)); LEFT on a row's first cell or RIGHT on its last (a fresh press, not a held arrow repeating) opens the menu, to the looper's left, once let go (the clock goes on); in the menu, RIGHT off its right edge (a card row's right end, or on a list item) goes back once let go (with no project loaded: a new one in the first empty slot) |
| LEFT + RIGHT held (0.3 s, `LR_HOLD_MS`) | Switch screens from anywhere: the menu opens, or closes (the looper's selection as it was; with no project loaded: a new one in the first empty slot; with every slot full: nothing; a project loaded from the menu still on its way: once it has come in). Only when the two were pressed within 200 ms of each other (`LR_WINDOW_MS`), nothing else held; the second one's press doesn't move the selection (it steps back where the first one moved it), and the looper's selection frame hidden while they're held; once switched, they do nothing more until both are let go (`switchHold()` in input.cpp) |
| A | Empty track: record / stop (another track recording: stop it, queue this one; the waveform cell is selected). A recording starts (or is queued) once A is let go, nothing else pressed meanwhile (and the track still empty); stopping or cancelling one acts on the press. Full track: mute/unmute; **swipe**: A still held, UP / DOWN (each tap, and a held one's repeats) step a track up / down (wrapping from the top track to the bottom one and back, never onto the status row) and do the same to each track stepped onto: after a mute, each track heard is muted, except the last one heard, where it stops (a solo); after an unmute, each muted track is unmuted, and it stops on the track before the one it started from (round again); empty tracks (and one recording) are stepped over as they are; stopped, UP / DOWN do nothing more until A is let go. Status row: the next scene on the scene cell, once A is let go (1 … N, then round to 1; from N+1* or 0* to 1 too: neither is stepped onto; `sceneNext()`; as SHIFT+UP: on the next bar line while playing; the clock isn't started; icon: two small play triangles a block apart, a block right of centre), SHIFT+A there duplicates the scene (as SHIFT + B twice; icon: a + in a checkered square); play / stop on the clock cell (icon play or stop; SHIFT+A as SHIFT+START), tap tempo on the tempo cell (B+A: the metronome's next setting, of 0 off / A auto, in a silent scene only (no clip on any track, muted or not) / 1 on: from 1, 0; from A, 0 in a silent scene, else 1; from 0, A in a silent scene, else 1 (it always changes what's heard); icon: four beat dots, the first raised (the bar's), 10×10 blocks, crossed out while it would click with the clock running; SHIFT+A: **audio follow** on / off, playback.md (while on, B+DOWN halves the tempo and B+UP doubles it, the beat kept theirs; following any external clock, B+LEFT/RIGHT on the tempo cell set the clock's offset from it); icon: a jack plug coming in from the right under an arrow pointing in, 10×10 blocks, crossed out while it's on) |
| B twice | Reset the current cell (whole loop / filter open / unity, centre; the clock cell: master volume 100%, balance centred; the tempo cell: the current scene's saved tempo, the one it was made at or its latest recording's, if it has one) |
| SHIFT + B twice | Duplicate the scene |
| B (held) | On a cell it adjusts (B + arrows): the action button shows four small arrows round a small +, black on the accent colour; while an arrow is down (adjusting), or A is held with B, the pressed look, dark grey with the icon in the accent colour. A does nothing then (B+A on a cell: kept for finer adjusting later). On the tempo cell (no arrow down) the button shows the metronome and B + A goes to its next setting (see A, the status row). On an empty track (any cell) the button shows resample (the record dot with a plus) and B + A **resamples** (once A is let go, as recording; with no clip unmuted too: it records the input as heard): records the app's output (everything heard) from the next bar line, queued like a recording; when it's saved it moves to a new scene, on the same track (scenes.md). Otherwise B + A does nothing (SHIFT+B+A: the scene clipboard, below). B + UP/DOWN on an empty track (a change on its way counts) **moves it** up / down: the track in its way takes its place (its clip, range, mute, filter, volume, balance; the empty one's settings go with it), wrapping round the 7 (UP on track 1: to track 7); while playing on the next quarter beat after the lead, the clip in step with the clock (no gap); the selection goes with the empty track (`swapTracks()` in storage.cpp). While B is held on an empty track, its button shows up and down arrows instead of its number (two filled 3×2 triangles a block apart, up above down, in the accent colour on its panel). Not with a recording on (or queued for) either |
| A or B (menu, on a project slot) | At once, on the press: load its project (playing: at the next bar line, as a scene change; its tracks start over from their 1.1.1 positions there and the clock reads 1.1.1; a project with no clips, a new one too: at once, the clock started again from 1.1.1 (`loadFromMenu()`); stopped: it stays stopped, ready to play) the menu staying open (icon: the small arrow right; once loaded, held on: the menu closes too after 0.6 s, `menuHold()` in input.cpp; the clock stopped: it starts too, as START). While it's held its card shows as the loaded one, steady; once let go, until the new project has come in (playing: its switch point, the next bar line), its card flashes every eighth (loaded on the first half of each beat, so on at the switch, N.1.1; a project's outline on the second; `menuNoteLoad()`, `shownLoaded()` in ui_menu.cpp), then stays loaded. The one before shows as not loaded from the press on, but keeps the play icon (its letter 2 blocks left for it) until the switch, when the new one takes it (`playingCard()`). An empty slot: a new, empty project there, opened once let go (the menu closes, on track 1's waveform cell; icon: the asterisk). On the loaded project's card: close the menu once let go, back to the looper (icon: the small arrow right). On either, held 0.6 s instead: the same then, without letting go, and the clock started if stopped (`openCard()`, `menuHold()`). While a project loaded here is on its way (playing: until its switch point, the next bar line), the menu can't be left: no edge leads out, and closing it (A on its card again, or holding) waits until it has come in, then happens by itself (`closeToLooper()` in input.cpp, `menuLoadPending()`). (With SHIFT, and on the list's items: once let go.) Loading not while recording |
| SHIFT + A or B (menu, on a slot) | Once let go (no confirmation): cut its project to the project clipboard (the loaded one: its slot empties and an empty dummy project is loaded, for now), or paste the clipboard into an empty slot. A project with no clips is only emptied, the clipboard keeping what it had (so paste needs a project with clips on it). Not while recording |
| SHIFT + A | Cut the track to the clipboard / paste onto an empty track / on a recording (or queued) track: cancel it at once, the recording thrown away (its file back to the pool; a queued one starts from the next bar). Icon: X |
| SHIFT + LEFT/RIGHT | On a track (any of its cells; while SHIFT is held its waveform cell is framed in the accent colour, the selected cell's frame hidden): previous / next sample in the order of their names (wraps; gapless): the same project and track's first, then the project's other tracks', then other projects' (storage.md); the waveform cell is selected. Empty recordings (every sample within 256 of each other: silence, a constant level, a little noise; `EMPTY_SPAN`) are stepped over, and, the clock stopped, deleted if no project uses them (`deleteEmptySamples()`) |
| SHIFT + LEFT/RIGHT (status row) | Playing: nudged while held, as Live's nudge buttons — RIGHT faster, LEFT slower, by 1% at first and 3% more a second, at most 8% (in 0.25% steps). Our own clock: its tempo, set again once let go (the shift in phase stays), the tempo cell showing the nudged tempo in the accent colour meanwhile. Following an external clock: its offset moved as far as the nudge would have moved the clock (RIGHT earlier), the follower taking ours there; it stays once let go; the offset shown meanwhile (`nudgeService()` in input.cpp) |
| B + LEFT/RIGHT, B + UP/DOWN (tempo cell, following an external clock) | LEFT/RIGHT: our clock's offset from it, 10 ms a press or repeat, RIGHT later (L7's material plays later), LEFT earlier, within ±250 ms; each source its own, 0 by default, saved in the session (`offmidi=`, `offusb=`, `offaudio=`); both together: back to 0; while held the tempo cell shows it (e.g. +20, the tag MS) in the accent colour (`syncOffsetMs` in midi_in.h). UP/DOWN: the tempo doubled / halved (fresh presses; within 30–240 BPM): audio follow's (playback.md), or a MIDI clock's taken times that (ours restarting on their next beat that fits; `midiScale()`) |
| SHIFT + UP/DOWN | Newer / older scene (see scenes.md); B + UP/DOWN does the same on the status row's scene cell (double taps and holding B included) |
| SHIFT + UP/DOWN twice, second held | To N+1\* / 0\* |
| B, SHIFT, A / SHIFT, B, A | Paste scene (B first) / cut scene (SHIFT first; a scene with no clips on its tracks is removed but doesn't replace the clipboard, and cutting 0* does nothing) |
| PLAY | Start from 1.1.1 / stop |
| SHIFT + PLAY (or SHIFT + A on status) | Restart from 1.1.1 |

Tap tempo (A on the status row): the tempo follows the taps, each tap in a
series refining it (a least-squares fit of every tap since the series began,
not only the last few; a pause over 2.1 s or another key starts a new one); with the
clock stopped, the 5th tap starts it from 1.1.1 (running: the clock is left as it is,
no snapping to the taps).

### B + arrows on the waveform (loop range)

* LEFT/RIGHT: move the start on a grid — clips ≤ 8 bars: 1 beat; ≤ 16 bars:
  half a bar; longer: a bar; a range of the whole loop moves on the start
  position's own grid (`startGrid()`: half bars on clips ≤ 8 bars, bars on longer ones), so
  every press moves the start position. Off-grid starts snap to the next grid point.
  The start position (where 1.1.1 plays from) is then the range's first half
  bar or bar line on clips ≤ 8 bars, its first bar line on longer ones
  (playback.md).
* Twice LEFT/RIGHT: move by the range's own length; a range inside the clip
  stops at the clip edge, one already touching it jumps to the other edge,
  one straddling the edge moves freely (wraps).
* UP/DOWN: length steps 1/4, 1/2, 3/4, 1, 1 1/2, 2, 3, 4, 5, 6 … bars.
* Twice UP/DOWN: powers of two 1/4 … 32 bars; a length of 3 × 2ⁿ beats
  doubles/halves instead (3/4 bar halves to 1/2).
* The start stays when the length changes.

Other cells with B + arrows: filter (LEFT/RIGHT move, UP/DOWN width; width
scales from the edge at the edge), volume (UP/DOWN: 0–10, each step a half line, on the clip's own scale: the
top step plays it peaking at 0 dBFS, its unity step — where it starts — plays it as
recorded, as many 3.75 dB steps below the top as its peak is below 0 dBFS, rounded
up, the step just below the top taking what's left; 3.75 dB steps below unity; 0
off; a clip peaking under ~-48 dBFS counts as peaking at 0 dBFS. The track keeps
its volume as steps from unity, so another clip plays at its own level as
recorded. Its meter: the top half at 0 dBFS. `volumeStepOf()` in engine.cpp; the
clip's peak is kept in its WAV header, older ones' taken from their overview) and balance (LEFT/RIGHT: ±10; each step moves the
other channel's level by a half line, so every press shows; all the way one side, that way again: the volume up a step; UP at the top step, panned: the balance a step back to the centre),
tempo (LEFT/RIGHT ±0.1, UP/DOWN ±1 to the nearest integer; 30–240 BPM),
the clock cell: the master volume and balance, one for all projects (UP/DOWN the volume a step, LEFT/RIGHT the balance, ±22, all the way: the other side silent; past it, the volume up a step; UP at full volume, panned: the balance a step back; B twice: back to 100% and centred, as B twice resets any cell; saved in the session as `out=` and `pan=`, older `gain=`, `level=`/`balance=` and `volume=` converted).

Timing constants (input.cpp): key repeat after 200 ms, every 50 ms;
double tap within 200 ms (100 ms was too short); edge-jump hold 350 ms.

## Screen

320×240, no frame buffer: everything is drawn over its own background and
only what changed is redrawn (`uiGeneration` forces a full redraw, e.g. after
the menu or splash). 8 rows: 7 tracks of 30 px from the top of the screen, a 4 px (two-block) gap,
then the status row (y 214–239, to the screen's bottom edge, its cells framed all round); cells waveform | filter | volume | button (square). Columns:
waveform x 0–187, filter 186–245, volume 244–291, then a block's space
before the buttons (x 294–319); the status row's four cells share
x 0–291 (menu x 0–17, scene 18–119, clock 118–211, tempo 210–291). Cells share a one-block border
with their neighbours (left, right, above and below; a cell only shares a
side with one right next to it): the selection frame is drawn there (`borderAt()` in ui.cpp), so
content uses the cell's full size.

The rows draw the tracks from a copy of their looks (`shownTrack()`,
`followTracks()` in ui.cpp), not the tracks themselves: while a change is on
its way to any track (a scene's, a project's, a move; from its request until
its switch point has passed: some of a track's data changes ahead of that, as
the stream takes the new clip on), the tracks it's on its way to keep their
looks; once none is on its way, they all take the new ones together and their
rows (cells and buttons) are redrawn in that same pass. So the old scene's
waveforms, filters, volumes and badges go and the new one's come at one
moment; what's the same in both stays. The playheads follow the range shown
(the old clip's until the switch).

Style (after Elektron's screens): seven colours (`colors.h`) — black (`BG`),
17% grey (`DARK`: background detail — outside the loop range, unlit volume lines, muted volume lines and filters), 44% grey
(`DIM`: inactive — muted clips, idle icons and text; filter fills, the selection frame while B is held), light grey (`LIGHT`, 81%: unselected waveforms, labels, selected fills and lines, the selection frame), white (`INK`: active,
selected) and the accent (`ACCENT`: the loaded project's colour, green turquoise 48/230/170 before one is loaded; `ACCENT_DARK` at half brightness, light grey's role while editing: recording
and a scene change on its way). Everything sits on a grid of 2×2-pixel blocks
(`PX`), a 160×120 screen of chunky pixels. Selection: a light grey frame on the
selected cell only (grey while B is held, with or without other keys:
adjusting; white while only arrows are held and the last press moved the
selection: not at an edge). An empty track (no clip, not recording) has one cell spanning
the whole row (`SPAN`, x 0–291: nothing in it), its outline round all of it; LEFT/RIGHT have
nowhere to go in it (LEFT still opens the menu), and the column selected is kept for the rows
with three (borders shared with the rows above and below are drawn by place on the screen); all the selected track's cells take their selected look
(the frame stays on the selected one, and B's editing look too), on a dark grey ground
(`rowGround()`): a rectangle as tall as the frame (its band and the block below; shared border
rows go dark grey when either row is selected), the screen's whole width, under the button
too (round its panel and badges); the status row's ground the same when it's selected (from the scene cell's left edge to the tempo cell's right one, all its corners cut a block, the selection frame's too; but only its
selected cell takes the selected look); anything dark grey on it is drawn black instead (a
muted clip's filter fill and volume level, its waveform outside the range). The selected track's button gets a 50% grey panel behind its number (from a block in to a block short of the screen's right edge,
the button's height: two blocks of border above, left and below the 7×9
badge; its bottom left corner cut a block; the accent colour while A is held, with the
action button, showing just the number then, in black).
None while the status row is selected. The status
button (the action button) is the accent colour with black icons (dark grey with icons in the accent colour while A is held; grey with a black icon, held or not, when its action can't act now: paste with the clipboard empty, resample with nothing playing, ...), as tall as the status cells, a block in from the left like the track panels, its top left and bottom left corners cut (a block each). On a track with a clip its icon is a speaker: crossed out while the track plays (A mutes it; a block right of the other), with sound waves while it's muted (A unmutes it).
A recording track's waveform area is in the accent colour (a block of black between it and the cell's border), the waveform growing on it in white, its bar lines in the dark accent.

* Waveform (`ui_wave.cpp`): 90 columns, inside a block of space within
  the frame (drawn in pixels, not blocks: 180 pixel columns, a 2-pixel
  centre line, the peaks up to 11 pixels each way, symmetric; square-root scale); light grey inside the loop range (white on the selected track), 50% grey outside (muted:
  50% grey range, dark grey outside). On every track (recording too), a black line the
  waveform's height, a block wide, just before the start of each bar but the first (on the
  same column edges as the range, so the range's colour stops at them). The
  start position, on every track: a dotted line the waveform's height (a block wide, a checkerboard of 1-pixel squares in white and black, over the waveform, while editing the range too; in the column a bar line there would take, so on a bar start it sits on the bar line). While B
  is held on the selected waveform cell, the range is in the accent colour,
  muted or not. Playheads are accent-coloured lines, a block taller than the waveform
  each way (white while the
  range is in the accent colour); on muted clips black lines cutting through
  the waveform. A
  recording's record head inverts its column.
* Filter (`ui_filter.cpp`): inside a block of space within the frame, the
  response filled from the curve down, under a
  line 2 pixels thick, the curve drawn in pixels (not blocks). Fill / line: grey / light grey;
  the selected track: light grey / white; muted: dark grey, no line; B held on it
  (muted or not): the dark accent / the accent colour.
* Volume (`ui_mix.cpp`): 5 straight vertical lines a side, every other
  column (a column short of the cell's edges), longer further out, each two
  1-pixel halves (10 a side, lit from the inside out). A channel's level in
  halves is the volume step minus its balance cut (one per step of either);
  the meter lights them by the half too. Up to
  the level / the outer lines: grey / dark grey; the selected track: light grey / grey;
  muted: dark grey / black. The meter: white. B held on it (muted or not, other keys too):
  the level in the accent colour, the outer lines grey, the meter still white over them (unmuted).
* Editing (B held on the selected cell, with other keys or not, so B + arrows change it): white turns to the accent colour and light grey to the
  dark accent (`ACCENT_DARK`, the same for the filter fill and the volume
  level): the loop range, the filter line and fill, the volume meter and
  level (`editingCell()` in ui.h).
* Icons (`ui_buttons.cpp`): bitmaps of blocks; "TAP" in the pixel font, condensed (T and A touching), plain black letters, their bottom row left out (4 rows, once, centred in the button; a block lower while A is held). Cut:
  an arrow pointing up from a line below it; paste: an arrow pointing down to
  the line; the scene versions the same with a checkered square right round them.
* Text (`ui_font.cpp`): a 3×5 pixel font (upper case, digits, a few signs;
  the splash's lower case and ©) and tags (plain / outlined / inverted).
  An arrow held that switches screens once let go (any of the four, off an edge): the selection frame hidden meanwhile, and towards the menu, a line 8 blocks thick at the edge (6 along the top or bottom), a block-wide one inside it, then another, their ends stepping in from the screen's corners: `TTTT0LJJJJL0TTTT`, `TTTT00LLLL00TTTT`, `TTTTT000000TTTTT` (L light grey, J dark grey, 0 black, T left as it is; the middle letter along the rest); leaving the menu, the same with black for dark grey (`0000`… in the thick one) drawn along the screen's edge that way, in the menu too (over its corner logo): gone again if let go without switching (`drawEdgeLine()`, ui.cpp).
  Status row (labels 50% grey, light grey on the selected cell): the scene cell, from the screen's left edge
  (an inverted SCN tag with the scene position,
  flashing while a change waits), then the clock cell (the clock
  centred), then the tempo cell (the tempo followed by an inverted BPM tag),
  then the button. In the accent colour: the scene's tag
  and number while SHIFT is held, or B alone on the scene cell; the tempo and
  its tag while B alone is held on its cell. The clock: stopped, as the scene's and tempo's labels (grey; light grey on the selected cell); running, 50% grey with
  light grey on each beat's first step (selected: light grey, white on the
  first step; while the metronome clicks, the accent colour on each beat's first
  step instead), the accent colour on 1.1.1; before it a small play icon, in
  the accent colour while it runs. While B alone is held on the clock
  cell it shows the master volume and balance instead: 22 vertical lines across the
  cell (one per step 1–22, 1.875 dB apart: 0 off, 17 100% — the default —, 22 +9.375 dB;
  balance ±20), their upper halves the left channel's, the lower halves the right's:
  in the accent colour, as many as that channel's level (the volume less the balance away
  from it; none at 0), black after it; each half meters its channel (white, line 17
  0 dBFS — the ones above it clipping —, falling back from peaks), from silence each
  time it shows (`drawMasterVolume()` in ui_status.cpp).
* Track buttons: the track's number (tracks count from the top: 1 is
  the first row, 7 just above the status row) — dark grey on an empty track (black when selected, on the panel); in a grey-outlined
  square on a muted one (number grey); in a square filled with the accent
  colour on one playing (number black). The recording (or queued) track shows
  its recording icons instead (small, in the accent colour; the record dot 5×5, while resampling the record dot with a plus). After a recording
  the selection stays on the track (`SELECT_NEXT_AFTER_RECORDING` in config.h,
  off for now, moves it on to the next empty track up).
* Colours: all in `colors.h`.
* Working on the look without the device: `tools/ui_preview` renders the real
  drawing code to PNGs (see build-and-test.md).
* Menu (`ui_menu.cpp`): the app starts in the menu, on the loaded
  project's card (with the dummy project loaded: the first empty slot's), with the clock stopped. Opened with any arrow off the looper's edges (LEFT on a row's first cell, RIGHT on its
  last, UP on track 1, DOWN on the status row), it takes the whole screen; it has no
  action button (nothing in that place). Opening and leaving the menu never starts or stops the clock;
  exporting to Ableton and storage mode stop it first, and start it again (from 1.1.1) when done if it was running. On its items A and B both act
  (SHIFT with them: cut and paste); START starts / stops the clock there too (SHIFT+START: from 1.1.1). A new project opens with the selection on track 1 (another project: the selection as it was). Loading a project, or making a new one, doesn't start the clock either
  (playing: the project comes in on the next bar line; one with no clips at once). On its items A
  runs the item's action when let go; B does nothing. The way out
  is loading a project (A on the loaded one: just back), or RIGHT off its right edge (on a card of the right column, or on a list item), LEFT off its left edge (on a card of the left column, or on a list item), UP off its top (on the top card row) or DOWN off its bottom (on the list's last item), once let go; with no project loaded those make a new one in the first empty slot (the menu's selection staying where it was) and go back (all slots full: nowhere to go); with the dummy
  project loaded, one has to be loaded or made first. Opening the menu
  with a project that has no clips at all (not recording) deletes it: its
  slot empties and the dummy project is loaded.
  The menu's ground is dark grey (`MENU_BG`), the whole screen's.
  Twelve project cards (three rows of four, 21×15 blocks, 3 apart across and 3 down, centred):
  rectangles with the bottom right corner cut diagonally (3 blocks), lettered
  A-L from the top row. Each slot has its colour (`PROJECT_COLORS`: twelve
  pastels, mint round to seafoam, picked by the author; the loaded project's is the
  app's accent colour, `ACCENT_DARK` at half its brightness; before one is
  loaded, the splash's turquoise). The loaded project's card is filled with
  it, its letter (the project's name) black; another
  project's is a thick outline (two blocks) in its colour, the letter in it too.
  While the clock runs, the playing project's card shows a small play icon right of its letter (both 2 blocks left of centre), in the letter's colour, on the first half of each beat (pulsing): the loaded one's, or while another is on its way in, the one still heard. An empty slot is a grey outline, its letter grey (selected too; in the slot's colour while an action button is held). In the bottom
  left corner, the logo small (full resolution, 55 × 26 px: the status row's height; a block
  in from the left edge, on the status row's line: `CORNER_LOGO_X/Y`), always
  in the colour the action button had for the item (`actionRestColor()`).  No mark for the
  way out (any edge, `menuLeavesOn()`); while an arrow is held that leaves it once let go,
  the selection frame is hidden. Under them
  a list, "EXPORT ABLETON PROJECT", "ENTER STORAGE MODE" and "SETTINGS" (off for now: grey, as export with nothing to export, the action button grey with no icon; still selectable), up to three in view at a time (scrolling once it has more; the row level with the logo left clear). Export
  (the loaded project, if it has clips; on release) writes it as an Ableton
  Live Set to `/L7/EXPORTS/L7 A005 Project/` (ableton.h), its label showing "EXPORTING
  3/14" (samples copied, then the set) and then "SAVED AS L7 A005" ("SAVED AS" grey,
  the name in the project's colour) until the project changes (an edit,
  a recording, a scene cut / pasted / duplicated, the tempo; `projectEdits()`) or another one
  is loaded: closing the menu, storage mode and moving the selection leave it ("EXPORT
  FAILED" goes when the menu closes); while it shows,
  the export item's action button is greyed and A does nothing. Storage mode (A or B, on release):
  writes everything out, then shows the card to a computer over USB as a
  mounted drive (USB mass storage, see usb_drive.h; it mounts like a flash
  drive on macOS, Windows and Linux) under a grey screen with the title (the L7 logo,
  in the menu's corner place and size, in the same colour at all times):
  light grey until the computer is connected, then going round the hues; the
  connection's state under it in white, the hint in black (the messages that replace it, "EJECTED", "RESTARTING", too),
  all left-aligned with the menu's list ("ENTER STORAGE MODE"), the hint ("PRESS ANY KEY TO EXIT"; the messages
  replacing it in its place), the
  title at y 78, the state 32 px under it, the hint on the screen's bottom line (level with the status row's text, the corner logo's row);
  and the connection ("CONNECT A USB CABLE" / "WAITING FOR THE
  COMPUTER" / "CONNECTED"); ejecting the drive on the computer, or pressing any key,
  leaves it with "WAIT" ("EJECTED" after an eject): the app drops everything it knew of the card
  and reads it all again, as at a start (the computer may have changed any
  file), and the menu is back as it was. Four in
  view at a time (an item
  every 12 rows, the last place in the status row's); moving, the list scrolls so the item past the selection that way
  stays in view where there is one, never past its ends (no empty places);
  with four or fewer, from the top of the room: a box as wide as
  the longest label, centred, labels left-aligned, light grey (white when
  selected). The item's colour (the corner logo's): on a card, its
  slot's; on project-level items (export) the accent (grey, the label too, with
  nothing to export: no clips in the project's tracks or scenes, or the dummy
  project); on system-level ones (storage mode) light grey. The selected item's frame looks and behaves as the
  looper's cells': light grey, white while the arrows move it, grey while A
  is held (not B; on a card, an action button A or B held turns it to the project's
  colour), corners cut; round a card one block out (following its
  diagonal); round the list's box, three blocks out. On a card A loads (an empty
  slot: new); with SHIFT, cuts or pastes (nothing when it can't: recording, another copy
  under way, clipboard empty). LEFT/RIGHT go along a card row (not on to
  another; RIGHT or LEFT off its ends leaves the menu, as they do on a list item); UP/DOWN go card row
  to card row and on into the list (back up: to the column it came from, on
  the I–L row). Switching screens, each screen keeps its own selection, row
  and column (the menu's: a card or list item, or its status row; the
  looper's: its row and cell, the tracks and the status row each their own
  column). With LEFT+RIGHT or A on the loaded project's card it's where it was
  left. Off an edge it lands on the other screen's far edge, as if they wrapped
  round: UP/DOWN onto its bottom / top row at the column that row was left on
  (UP off track 1: the menu's last list item; DOWN off the status row: the A–D
  row, at the card column last selected; UP off the A–D row: the looper's
  status row; DOWN off the last list item: track 1), LEFT/RIGHT onto its right
  / left side in the row it was left on (LEFT off the looper: the menu's right
  column, D H L, a list item or its status row as it was; RIGHT off the menu:
  the looper's row's first cell; LEFT off the menu's left side: its last).
  `menuLand()` in ui_menu.cpp, `leaveMenu()` in input.cpp. The menu's
  selection is kept across power cycles; if it was open at power off it
  opens again at boot (DATA/SESSION.DAT).
* Splash (`ui_splash.cpp`): the "L7" logo (italic, the 7's right edge one
  45° line to a sharp corner, two equal cuts near the bottom; design options in
  `docs/design/`), drawn in full resolution from its shape (`drawL7()`, at any
  scale: 138×65 px here, 12 px right of the middle; the menu's corner: 55×26),
  fading in (0–0.5 s) while the L slides in from the left and the 7 from the
  right (from 600 px off: still about at the screen's edges once well lit;
  cubic ease-out, in place at 1 s), its hue going
  once round, with "© 2026 Daniel Stanciu" centred near the bottom in light grey, fading in (1.5–2 s) as it rises 8 px from level with the status row's text (where the clock's is) into place (cubic ease-out, in place at 2.5 s), in
  the app's 3×5 block font (2×2 px blocks; 7×7 ©). This intro (2.5 s) plays through
  first (`splashIntro()`); the startup work comes after it, everything still meanwhile
  (startup messages print under the logo), and the fade out (0.5 s) starts once that's
  done, not before 3.5 s.
* Headphones plugged in → speaker off (sense pin 22 < 300).
