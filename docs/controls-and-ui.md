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
scales from the edge at the edge), volume (UP/DOWN) and balance (LEFT/RIGHT),
tempo (LEFT/RIGHT ±0.1, UP/DOWN ±1 to the nearest integer; 30–240 BPM).

Timing constants (input.cpp): key repeat after 200 ms, every 50 ms;
double tap within 200 ms (100 ms was too short); edge-jump hold 350 ms.

## Screen

320×240, no frame buffer: everything is drawn over its own background and
only what changed is redrawn (`uiGeneration` forces a full redraw, e.g. after
the panel or splash). 9 rows of 26 px: 8 tracks + status row; cells
waveform | filter | volume | button (square).

Style (after Elektron's screens): five colours (`colors.h`) — black (`BG`),
12.5% grey (`DARK`: background detail — outside the loop range, fills, unlit
arcs, the selected row's other frames, the divider, bar ticks), 50% grey
(`DIM`: inactive — muted clips, idle icons and text), white (`INK`: active,
selected) and the accent (`ACCENT`, green turquoise 48/230/170 for now, one constant: recording
and a scene change on its way). Everything sits on a grid of 2×2-pixel blocks
(`PX`), a 160×120 screen of chunky pixels. Selection: a grey frame on the
selected cell only (75% grey while B alone is held); the selected track also gets a grey line (4 px; the accent colour while A
is held, with the action button) down the
screen's right edge, in its button (none for the status row). The status
button (the action button) is 50% grey with black icons and its corners cut;
the accent colour while A is held. A recording track's waveform cell is in the accent colour.

* Waveform (`ui_wave.cpp`): 64 columns, a centre line with peaks up to 4
  blocks each way; white inside the loop range, dark grey outside (muted:
  50% grey range, dark grey outside). The start position: a short line
  across the middle (inverting what's there), on every track. While the
  waveform cell is selected: bar ticks along the bottom; while B alone is held
  on it, the range is in the accent colour, muted or not. Playheads are accent-coloured lines (white while the
  range is in the accent colour); on muted clips black lines cutting through
  the waveform. A
  recording's record head inverts its column.
* Filter (`ui_filter.cpp`): the response filled dark grey; the line itself
  only while the filter cell is selected and the clip plays (muted: only while
  B is held, in the accent colour).
* Volume (`ui_mix.cpp`): `((( )))`, 4 arcs a side: dark grey above the level,
  50% grey up to it, white for the meter.
* Editing (B held on the selected cell, so B + arrows change it): the loop
  range (B alone), the filter line, and the volume meter (B alone; its level
  arcs in the accent at half brightness) turn to the accent colour
  (`editingCell()` in ui.h).
* Icons (`ui_buttons.cpp`): bitmaps of blocks; "TAP" in the pixel font.
* Text (`ui_font.cpp`): a 3×5 pixel font (upper case, digits, a few signs;
  the splash's lower case and ©) and tags (plain / outlined / inverted).
  Status row (labels white while playing, grey while stopped): the scene cell
  (an inverted SCN tag with the scene position,
  flashing while a change waits), then the tempo cell spanning the filter and
  volume columns (the clock on its left, the tempo on its right, inverted
  while the cell is selected), then the button. In the accent colour: the
  scene's tag and number while SHIFT is held, or B alone on the scene cell;
  the tempo while B alone is held on its cell; the clock while START is held. The divider
  above the status row runs the full width.
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
