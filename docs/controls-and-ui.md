# Controls and screen

M8 keys: arrows, SHIFT, PLAY (Start), OPT (= **B**), EDIT (= **A**), power.
Keys are on/off only (no pressure). The full key list is the header comment of
`m8_looper.ino`; this page adds the details behind it.

## Keys

| Keys | Action |
|---|---|
| UP/DOWN | Select row: tracks 8 (top) to 1 (bottom), status row (no wrapping); starts on track 1 |
| LEFT/RIGHT | Select cell: waveform, filter, volume (status row: scene, clock, tempo) |
| A | Empty track: record / stop (another track recording: stop it, queue this one). Full track: mute/unmute. Status row: the menu (on release; the clock stops) on the scene cell, start from 1.1.1 on the clock cell (as SHIFT+START; icon |>), tap tempo on the tempo cell; SHIFT+A as SHIFT+START |
| B twice | Reset the current cell (whole loop / filter open / unity, centre) |
| SHIFT + B twice | Duplicate the scene |
| B (held) | On a cell it adjusts (B + arrows): the action button shows four small arrows round a small +, black on the accent colour; while an arrow is down (adjusting), or A is held with B, the pressed look, dark grey with the icon in the accent colour. A does nothing then (B+A on a cell: kept for finer adjusting later). On an empty track (any cell) the button shows resample (the record dot with a plus) and B + A **resamples** (unavailable, the button grey, while no clip plays unmuted: nothing to resample): records the app's output (everything heard) from the next bar line, queued like a recording; when it's saved it moves to a new scene, on the same track (scenes.md). Otherwise B + A does nothing (SHIFT+B+A: the scene clipboard, below) |
| A, B or START (menu, on a project slot) | Once let go: load its project and start the clock, from 1.1.1 (playing: at the next bar line, as a scene change; its tracks start over from their 1.1.1 positions there and the clock reads 1.1.1) (an empty slot: a new, empty project there, on track 1's waveform cell) and close the menu; the loaded one: just close it. Not while recording |
| SHIFT + A or B (menu, on a slot) | On a project: the first A only asks (the icon turns into a question mark); a second A, SHIFT still held, cuts it once let go (letting go of SHIFT or moving cancels). Once let go: cut its project to the project clipboard (the loaded one: its slot empties and an empty dummy project is loaded, for now), or paste the clipboard into an empty slot. Not while recording |
| SHIFT + A | Cut the track to the clipboard / paste onto an empty track / on a recording (or queued) track: cancel it at once, the recording thrown away (its file back to the pool; a queued one starts from the next bar). Icon: X |
| SHIFT + LEFT/RIGHT | Previous / next sample in the order of their names (wraps; gapless): the same project and track's first, then the project's other tracks', then other projects' (storage.md) |
| SHIFT + UP/DOWN | Newer / older scene (see scenes.md); B + UP/DOWN does the same on the status row's scene cell (double taps and holding B included) |
| SHIFT + UP/DOWN twice, second held | To N+1\* / 0\* |
| B, SHIFT, A / SHIFT, B, A | Paste scene (B first) / cut scene (SHIFT first) |
| PLAY | Start from 1.1.1 / stop |
| SHIFT + PLAY (or SHIFT + A on status) | Restart from 1.1.1 |

Tap tempo (A on the status row): the tempo follows the taps; the 5th, 9th, …
tap restarts from 1.1.1 (running: snaps to the nearest bar start).

### B + arrows on the waveform (loop range)

* LEFT/RIGHT: move the start on a grid — clips ≤ 8 bars: 1 beat; ≤ 16 bars:
  half a bar; longer: a bar. Off-grid starts snap to the next grid point.
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
scales from the edge at the edge), volume (UP/DOWN: 0–5, 7.5 dB steps, unity
at 4) and balance (LEFT/RIGHT: ±5; each step moves one channel's level by a
line, so every press shows),
tempo (LEFT/RIGHT ±0.1, UP/DOWN ±1 to the nearest integer; 30–240 BPM),
the clock cell: the master volume, the project's (any arrow a step; B twice: off, or from off back to 100%; new projects start with the last level set).

Timing constants (input.cpp): key repeat after 200 ms, every 50 ms;
double tap within 200 ms (100 ms was too short); edge-jump hold 350 ms.

## Screen

320×240, no frame buffer: everything is drawn over its own background and
only what changed is redrawn (`uiGeneration` forces a full redraw, e.g. after
the menu or splash). 8 rows: 7 tracks of 30 px from the top of the screen, a 4 px (two-block) gap,
then the status row (y 214–239, to the screen's bottom edge, its cells framed all round); cells waveform | filter | volume | button (square). Columns:
waveform x 0–187, filter 186–245, volume 244–291, then a block's space
before the buttons (x 294–319); the status row's three cells share
x 0–291 (scene x 0–109, clock 108–201, tempo 200–291). Cells share a one-block border
with their neighbours (left, right, above and below; a cell only shares a
side with one right next to it): the selection frame is drawn there (`borderAt()` in ui.cpp), so
content uses the cell's full size.

Style (after Elektron's screens): seven colours (`colors.h`) — black (`BG`),
17% grey (`DARK`: background detail — outside the loop range, unlit volume lines, muted volume lines and filters), 44% grey
(`DIM`: inactive — muted clips, idle icons and text; filter fills, the selection frame while B is held), light grey (`LIGHT`, 81%: unselected waveforms, labels, selected fills and lines, the selection frame), white (`INK`: active,
selected) and the accent (`ACCENT`: the loaded project's colour, green turquoise 48/230/170 before one is loaded; `ACCENT_DARK` at half brightness, light grey's role while editing: recording
and a scene change on its way). Everything sits on a grid of 2×2-pixel blocks
(`PX`), a 160×120 screen of chunky pixels. Selection: a light grey frame on the
selected cell only (grey while B is held, with or without other keys:
adjusting; white while only arrows are held and the last press moved the
selection: not at an edge). The selected track's button gets a 50% grey panel behind its number (from a block in,
the button's height: two blocks of border above, left and below the 7×9
badge; the accent colour while A is held, with the
action button, showing just the number then, in black).
None while the status row is selected. The status
button (the action button) is the accent colour with black icons (dark grey with icons in the accent colour while A is held; grey with a black icon, held or not, when its action can't act now: paste with the clipboard empty, resample with nothing playing, ...), as tall as the status cells, a block in from the left like the track panels, its top left corner cut. On a track with a clip its icon is a speaker: crossed out while the track plays (A mutes it; a block right of the other), with sound waves while it's muted (A unmutes it).
A recording track's waveform area is in the accent colour (a block of black between it and the cell's border), the waveform growing on it in white, its bar lines in the dark accent.

* Waveform (`ui_wave.cpp`): 90 columns, inside a block of space within
  the frame (peaks up to 4 blocks each way from a two-block centre,
  square-root scale); light grey inside the loop range (white on the selected cell), 50% grey outside (muted:
  50% grey range, dark grey outside). On every track (recording too), a black line the
  waveform's height just before the start of each bar but the first (on the
  same column edges as the range, so the range's colour stops at them). The
  start position, on every track: a dotted line the waveform's height (10
  blocks: a block of the waveform's colour there every third, black between). While B alone
  is held on the selected waveform cell, the range is in the accent colour,
  muted or not. Playheads are accent-coloured lines, a block taller than the waveform
  each way (white while the
  range is in the accent colour); on muted clips black lines cutting through
  the waveform. A
  recording's record head inverts its column.
* Filter (`ui_filter.cpp`): inside a block of space within the frame, the
  response filled from the curve down, under a
  line one block thick. Fill / line: grey / light grey;
  selected: light grey / white; muted: dark grey, no line; B held on it
  (muted or not): the dark accent / the accent colour.
* Volume (`ui_mix.cpp`): 5 straight vertical lines a side, every other
  column (a column short of the cell's edges), longer further out. A channel's level in lines is
  the volume step minus its balance cut (one line per step of either). Up to
  the level / the outer lines: grey / dark grey; selected: light grey / grey;
  muted: dark grey / black. The meter: white. B alone held on it (muted or not):
  no meter, the level in the accent colour.
* Editing (B held on the selected cell, so B + arrows change it; volume and
  waveform: B alone): white turns to the accent colour and light grey to the
  dark accent (`ACCENT_DARK`, the same for the filter fill and the volume
  level): the loop range, the filter line and fill, the volume meter and
  level (`editingCell()` in ui.h).
* Icons (`ui_buttons.cpp`): bitmaps of blocks; "TAP" in the pixel font, condensed (T and A touching), plain black letters. Cut:
  an arrow pointing up from a line below it; paste: an arrow pointing down to
  the line; the scene versions the same with a checkered square right round them.
* Text (`ui_font.cpp`): a 3×5 pixel font (upper case, digits, a few signs;
  the splash's lower case and ©) and tags (plain / outlined / inverted).
  Status row (labels 50% grey, light grey on the selected cell): the scene cell
  (an inverted SCN tag with the scene position,
  flashing while a change waits), then the clock cell (the clock
  centred), then the tempo cell (the tempo followed by an inverted BPM tag),
  then the button. In the accent colour: the scene's tag
  and number while SHIFT is held, or B alone on the scene cell; the tempo and
  its tag while B alone is held on its cell. The clock: stopped, dark grey (selected: 50% grey); running, 50% grey with
  light grey on each beat's first step (selected: light grey, white on the
  first step), the accent colour on 1.1.1; before it a small play icon, in
  the accent colour while it runs. While B alone is held on the clock
  cell it shows the master volume instead: 22 vertical lines across the cell
  (one per step 0–21, 1.5 dB apart: 0 off, 17 100% — the default —, 21 200%): the volume's
  line white, dark accent before it and dark grey after it; their upper halves meter the left output, the
  lower halves the right (accent, line 17 0 dBFS — the ones above it clipping —, falling back from
  peaks), from silence each time it shows.
* Track buttons: the track's number (tracks count from the bottom: 1 is
  just above the status row) — dark grey on an empty track (black when selected, on the panel); in a grey-outlined
  square on a muted one (number grey); in a square filled with the accent
  colour on one playing (number black). The recording (or queued) track shows
  its recording icons instead (small, in the accent colour; the record dot 5×5, while resampling the record dot with a plus). After a recording
  the selection stays on the track (`SELECT_NEXT_AFTER_RECORDING` in config.h,
  off for now, moves it on to the next empty track up).
* Colours: all in `colors.h`.
* Working on the look without the device: `tools/ui_preview` renders the real
  drawing code to PNGs (see build-and-test.md).
* Menu (`ui_menu.cpp`): the app starts in it (the clock stopped, on the
  last project's card, that project already loaded). Opened with A on the scene cell, or by stopping the
  clock (START, once let go), it takes
  the place of the track rows and the status row's cells, across the whole
  width; of the status row only the action button shows. Opening it stops the
  clock. On its items A and B both act (SHIFT with them: cut and paste); on
  a project card START does too (load / new). Loading a project, or making a
  new one, starts the clock (from 1.1.1; a new, empty one plays the
  metronome). In the looper's view,
  stopping the clock (START) goes back to the menu
  once that key is let go; SHIFT+START only restarts from 1.1.1. On its items A
  runs the action button's action when let go; B does nothing. The way out
  is loading a project (A on the loaded one: just back); with the dummy
  project loaded, one has to be loaded or made first. Opening the menu
  with a project that has no clips at all (not recording) deletes it: its
  slot empties and the dummy project is loaded.
  Twelve project cards (three rows of four, 21×15 blocks, 3 apart across and 3 down, centred):
  rectangles with the bottom right corner cut diagonally (3 blocks), lettered
  A-L from the top row. Each slot has its colour (`PROJECT_COLORS`: twelve
  pastels, rose to blush, picked by the author; the loaded project's is the
  app's accent colour, `ACCENT_DARK` at half its brightness; before one is
  loaded, the splash's turquoise). The loaded project's card is filled with
  it, its letter (the project's name) black; another
  project's is a thick outline (two blocks) in its colour, the letter in it too.
  An empty slot is a grey outline, its letter grey (light grey when selected; in the slot's colour while an action button is held). In the bottom
  left corner, the logo small (full resolution, 55 × 26 px: the status row's height), always
  in the action button's colour (not its pressed look: it stays put while A is held). Under them
  a list, "EXPORT ABLETON PROJECT" and "ENTER STORAGE MODE". Export
  (the loaded project, if it has clips; on release) writes it as an Ableton
  Live Set to `/Ableton/L7 A005/` (ableton.h), its label showing "EXPORTING
  3/14" (samples copied, then the set) and then "SAVED AS L7 A005" (or "EXPORT
  FAILED") until the selection moves. Storage mode (A or B, on release):
  writes everything out, then shows the card to a computer over USB as a
  mounted drive (USB mass storage, see usb_drive.h; it mounts like a flash
  drive on macOS, Windows and Linux) under a screen with the title going
  round the hues and the connection ("CONNECT A USB CABLE" / "WAITING FOR THE
  COMPUTER" / "CONNECTED"); ejecting the drive on the computer, or holding B
  for a second, leaves it by restarting the app (the computer may have changed
  any file). Four in
  view at a time (an item
  every 12 rows, the last place in the status row's, level with the action
  button); moving, the list scrolls so the item past the selection that way
  stays in view where there is one, never past its ends (no empty places);
  with four or fewer, from the top of the room: a box as wide as
  the longest label, centred, labels left-aligned, light grey (white when
  selected). Going to the menu, the action button is in the accent colour (and its
  icon while held; the selected track's panel stays grey then). The action button takes the item's colour: on a card, its
  slot's; on project-level items (export) the accent (grey, the label too, with
  nothing to export: no clips in the project's tracks or scenes, or the dummy
  project); on system-level ones (storage mode) light grey (its icon light
  grey while pressed too). The selected item's frame looks and behaves as the
  looper's cells': light grey, white while the arrows move it, grey while A
  is held (not B; on a card, an action button A, B or START held turns it to the project's
  colour), corners cut; round a card one block out (following its
  diagonal); round the list's box, three blocks out. On a card the
  action button shows load (a small play) or, empty, new (a small asterisk); with SHIFT,
  cut or paste (the button grey when it can't act: recording, another copy
  under way, clipboard empty); on the list, an arrow down into a tray (export) and the USB trident (drive mode),
  doing nothing yet. LEFT/RIGHT go through everything in order: the cards row by row, then the
  list; UP/DOWN go card row to card row and on into the list (back up: to
  the column it came from). The looper's own selection is kept
  meanwhile and comes back when the menu closes. The menu opens on the
  loaded project's card (with the dummy project: where its selection was,
  kept across power cycles too); if it was open at power off it opens again
  at boot (session.txt).
* Splash (`ui_splash.cpp`): the "L7" logo (italic, the 7's right edge one
  45° line to a sharp corner, two equal cuts near the bottom; design options in
  `docs/design/`), drawn in full resolution from its shape (`drawL7()`, at any
  scale: 138×65 px here, 12 px right of the middle; the menu's corner: 55×26),
  fading in (0–0.5 s) while the L slides in from the left and the 7 from the
  right (from 600 px off: still about at the screen's edges once well lit;
  cubic ease-out, in place at 1 s), its hue going
  once round, with "© 2026 Daniel Stanciu" centred near the bottom in light grey, fading in (1.5–2 s) as it rises 8 px from level with the status row's text (where the clock's is) into place (cubic ease-out, in place at 2.5 s), in
  the app's 3×5 block font (2×2 px blocks; 7×7 ©); 4 s (fading out from 3.5 s); startup messages
  print under the logo.
* Headphones plugged in → speaker off (sense pin 22 < 300).
