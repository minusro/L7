# Controls and screen

M8 keys: arrows, SHIFT, PLAY (Start), OPT (= **B**), EDIT (= **A**), power.
Keys are on/off only (no pressure). The full key list is the header comment of
`m8_looper.ino`; this page adds the details behind it.

## Keys

| Keys | Action |
|---|---|
| UP/DOWN | Select row: tracks 8 (top) to 1 (bottom), status row (no wrapping); starts on track 1 |
| LEFT/RIGHT | Select cell: waveform, filter, volume (status row: tempo) |
| A | Empty track: record / stop (another track recording: stop it, queue this one). Full track: mute/unmute. Status row: tap tempo on the tempo cell, the panel (as B+A) on the scene cell |
| B twice | Reset the current cell (whole loop / filter open / unity, centre) |
| SHIFT + B twice | Duplicate the scene |
| B + A | Open the panel (A closes it) |
| SHIFT + A | Cut the track to the clipboard / paste onto an empty track |
| SHIFT + LEFT/RIGHT | Previous / next sample in recording order (wraps; gapless) |
| SHIFT + UP/DOWN | Newer / older scene (see scenes.md); B + UP/DOWN does the same on the status row's scene cell (double taps and holding B included) |
| SHIFT + UP/DOWN twice, second held | To N+1\* / 0\* |
| B, SHIFT, A / SHIFT, B, A | Paste scene (B first) / cut scene (SHIFT first) |
| PLAY | Start from 1.1.1 / stop |
| SHIFT + PLAY (or SHIFT + A on status) | Restart from 1.1.1 |

Tap tempo (A on the status row): the tempo follows the taps; the 5th, 9th, …
tap restarts from 1.1.1 (running: snaps to the nearest bar start).

### B + arrows on the waveform (loop range)

* LEFT/RIGHT: move the start on a grid — clips ≤ 4 bars: 1 beat; ≤ 16 bars:
  half a bar; longer: a bar. Off-grid starts snap to the next grid point.
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
tempo (LEFT/RIGHT ±0.1, UP/DOWN ±1 to the nearest integer; 30–240 BPM).

Timing constants (input.cpp): key repeat after 200 ms, every 50 ms;
double tap within 200 ms (100 ms was too short); edge-jump hold 350 ms.

## Screen

320×240, no frame buffer: everything is drawn over its own background and
only what changed is redrawn (`uiGeneration` forces a full redraw, e.g. after
the panel or splash). 9 rows of 26 px: 8 tracks from the top of the screen, a 2 px gap, then
the status row; cells waveform | filter | volume | button (square). Columns:
waveform x 0–179, filter 182–241, volume 244–291, button 294–319, with a
block's space between each; the status row's two cells are half of x 0–291
each (scene x 0–145, tempo 144–291). Cells share a one-block border with the
cells above and below (and with one right next to it, as the status row's
two are): the selection frame is drawn there (`borderAt()` in ui.cpp), so
content uses the cell's full size.

Style (after Elektron's screens): seven colours (`colors.h`) — black (`BG`),
12.5% grey (`DARK`: background detail — outside the loop range, unlit volume lines, the selection frame while B is held, muted volume lines and filters), 50% grey
(`DIM`: inactive — muted clips, idle icons and text; filter fills, the selection frame), light grey (`LIGHT`, 81%: unselected waveforms, labels, selected fills and lines), white (`INK`: active,
selected) and the accent (`ACCENT`, green turquoise 48/230/170 for now, one constant; `ACCENT_DARK` at half brightness, light grey's role while editing: recording
and a scene change on its way). Everything sits on a grid of 2×2-pixel blocks
(`PX`), a 160×120 screen of chunky pixels. Selection: a 50% grey frame on the
selected cell only (dark grey while B alone is held). The selected track's button gets a 50% grey panel behind its number (the button's
full height: two blocks of border above, left and below; the accent colour while A is held, with the
action button, showing just the number then, in black).
None while the status row is selected. The status
button (the action button) is grey with black icons (dark grey with icons in the accent colour while A is held), a block taller than the others (down to the status cells' bottom border), its top left corner cut.
A recording track's waveform cell is in the accent colour.

* Waveform (`ui_wave.cpp`): 86 columns, inside a block of space within
  the frame (peaks up to 4 blocks each way from a two-block centre,
  square-root scale); light grey inside the loop range (white on the selected cell), dark grey outside (muted:
  50% grey range, dark grey outside). The start position: a short line
  across the middle (inverting what's there), on every track. While B alone
  is held on the selected waveform cell, the range is in the accent colour
  (muted: it stays grey). Playheads are accent-coloured lines (white while the
  range is in the accent colour); on muted clips black lines cutting through
  the waveform. A
  recording's record head inverts its column.
* Filter (`ui_filter.cpp`): inside a block of space within the frame, the
  response filled from the curve down, under a
  line one block thick. Fill / line: grey / light grey;
  selected: light grey / white; muted: dark grey, no line; B held on an
  unmuted one: the dark accent / the accent colour.
* Volume (`ui_mix.cpp`): 5 straight vertical lines a side, every other
  column (a column short of the cell's edges), longer further out. A channel's level in lines is
  the volume step minus its balance cut (one line per step of either). Up to
  the level / the outer lines: grey / dark grey; selected: light grey / grey;
  muted: dark grey / black. The meter: white. B alone held on an unmuted one:
  the meter in the accent colour, the level in the dark accent.
* Editing (B held on the selected cell, so B + arrows change it; volume and
  waveform: B alone): white turns to the accent colour and light grey to the
  dark accent (`ACCENT_DARK`, the same for the filter fill and the volume
  level): the loop range, the filter line and fill, the volume meter and
  level (`editingCell()` in ui.h).
* Icons (`ui_buttons.cpp`): bitmaps of blocks; "TAP" in the pixel font.
* Text (`ui_font.cpp`): a 3×5 pixel font (upper case, digits, a few signs;
  the splash's lower case and ©) and tags (plain / outlined / inverted).
  Status row (labels light grey, white on the selected cell): the scene cell
  (an inverted SCN tag with the scene position,
  flashing while a change waits), then the tempo cell (the same width) (the clock on its left, the tempo on its right followed by an
  inverted BPM tag), then the button. In the accent colour: the scene's tag
  and number while SHIFT is held, or B alone on the scene cell; the tempo and
  its tag while B alone is held on its cell. The clock: dark grey while stopped; running, in the accent colour on 1.1.1,
  white on each beat's first step, light grey on the others.
* Track buttons: the track's number (tracks count from the bottom: 1 is
  just above the status row) — dark grey on an empty track; in a grey-outlined
  square on a muted one (number grey); in a square filled with the accent
  colour on one playing (number black). The recording (or queued) track shows
  its recording icons instead (small, in the accent colour). After a recording
  the selection moves on to the next empty track up.
* Colours: all in `colors.h`.
* Working on the look without the device: `tools/ui_preview` renders the real
  drawing code to PNGs (see build-and-test.md).
* Splash (`ui_splash.cpp`): "L8" logo rasterised from the author's artwork
  into 43×26 blocks of 2×2 px (two tracks tall), white, with
  "© 2026 Daniel Stanciu" under it in a 3×5 block font (7×7 ©); at least
  2 s; startup messages print below it.
* Headphones plugged in → speaker off (sense pin 22 < 300).
