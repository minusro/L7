# Controls and screen

M8 keys: arrows, SHIFT, PLAY (Start), OPT (= **B**), EDIT (= **A**), power.
Keys are on/off only (no pressure). The full key list is the header comment of
`m8_looper.ino`; this page adds the details behind it.

## Keys

| Keys | Action |
|---|---|
| UP/DOWN | Select row: tracks 1–8, status row (no wrapping) |
| LEFT/RIGHT | Select cell: waveform, filter, volume (status row: tempo) |
| A | Empty track: record / stop (another track recording: stop it, queue this one). Full track: mute/unmute (unmute while stopped starts the clock). Status row: tap tempo |
| B twice | Reset the current cell (whole loop / filter open / unity, centre) |
| SHIFT + B twice | Duplicate the scene |
| B + A | Open the panel (A closes it) |
| SHIFT + A | Cut the track to the clipboard / paste onto an empty track |
| SHIFT + LEFT/RIGHT | Previous / next sample in recording order (wraps; gapless) |
| SHIFT + UP/DOWN | Newer / older scene (see scenes.md) |
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
waveform | filter | volume | icon button.

* Waveform: overview inside/outside the range, bar lines, start-position line,
  playheads (white with a black edge; muted: single line in the range colour,
  only when selected). Empty tracks draw nothing.
* Filter: anti-aliased response curve. Volume: `((()))` arcs + level meter.
* Icons: record (red) / stop / play / pause / cut / paste / menu / home /
  scene cut / scene paste; the status button shows what A does now and dims
  while A is held. Recording waits flash.
* Status row: tempo, clock (bar.beat.step, wraps at 256 bars), scene label.
* Colours: all in `colors.h` (grey, not bluish, for idle icons).
* Splash (`ui_splash.cpp`): "L8" logo rasterised from the author's artwork
  into 43×26 blocks of 2×2 px (two tracks tall), white, with
  "© 2026 Daniel Stanciu" under it in a 3×5 block font (7×7 ©); at least
  1.5 s; startup messages print below it.
* Headphones plugged in → speaker off (sense pin 22 < 300).
