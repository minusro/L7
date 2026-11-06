# wip — test builds, not releases

Builds here are for testing before they're released; releases are on GitHub's
releases page.

To download one, open the file and use its "Download raw file" button:
right-click → "Save link as" on the folder page saves GitHub's HTML page,
which TyUploader rejects with an IHEX parse error on line 1.

## `L7-M8-Model02-diag-linein-2026-11-06.hex` — Model:02 only, not L7

A diagnostic: does the Model:02 have anything that senses a cable in its
LINE IN jack? L7's mic input (the mic used when nothing is plugged in)
depends on the answer. It's built from `src/m02_diag.h` with `M02_DIAG` 1.
It doesn't touch the SD card.

1. Flash it the way you flash L7. It starts straight on a diagnostic screen.
2. **A check first:** put headphones in and take them out. `A22` should light
   up (green). If it doesn't, stop and tell me: the diagnostic isn't working.
3. Press any key to clear it. Plug a cable into **LINE IN** and pull it out,
   three times (the other end can be connected or not). Take a photo.
4. Press any key. Plug in a cable from a source that's **switched off or
   silent**, wait 5 seconds, take a photo. Pull it out, wait 5 seconds, take
   another photo.
5. Clap or talk near the M8 and take a photo (the `MIC` peak should jump).
6. Send the photos, then flash L7 back. The power button switches it off.

SHA-256: `0a7162aa966526dd8359ed9ccd933d66188bc9ec4edd5ff811930a51e0de499e`

## `L7-M8-Model02-mic-2026-11-06.hex` — L7 with the built-in mic, Model:02 only

L7 using the Model:02's built-in mic when nothing is playing into LINE IN.
**Run the diagnostic above first** if you can: it tells me whether the
Model:02 can sense a cable in LINE IN, which this build can't yet - it
guesses from silence (below).

What's new, all in the menu's **SETTINGS**: **AUDIO IN**, at the bottom,
pressed to step through **AUTO**, **LINE** and **MIC**.

- **AUTO** (the default): the line input, and the mic once the line input has
  been silent for about 2 seconds between recordings. While on the mic it
  listens to the line input for a moment every second or so, and goes back to
  it as soon as anything is playing. It reads `AUTO: LINE` or `AUTO: MIC`.
- **LINE** and **MIC** force one or the other.
- The mic is **recorded but never heard**. A resampling leaves it out.
- USB audio from a computer is still heard, and recorded together with the mic.
- When the mic is the input, the track you'd record on shows a **mic in its
  button**: grey on the selected empty track, green while it records.

Please try, and tell me what happens:

1. With **nothing in LINE IN**: open SETTINGS and watch AUDIO IN go from
   `AUTO: LINE` to `AUTO: MIC` within a few seconds. If it never does, or keeps
   flipping, say so - and send the diagnostic's photos.
2. Close the menu, select an empty track: its button shows a mic. Press EDIT,
   talk or clap: you shouldn't hear yourself. Stop, play it back: you should.
3. **Plug in a source and play it**: within a second or two you hear it,
   AUDIO IN reads `AUTO: LINE`, the empty track shows its number again, and
   recording takes the line input.
4. **Resample** (OPT+EDIT) with the mic as the input: the mic isn't in it.
5. With **USB audio** playing from a computer and nothing in LINE IN: you hear
   the computer; a recording has the computer and the mic.
6. Set AUDIO IN to **LINE**, then **MIC**: each sticks, whatever's plugged in.
7. If a recording from the mic starts with a gap or a click, or anything above
   doesn't match, tell me which step.

SHA-256: `055321802ef09f5bee03f5d1c1d063978f8807aca1be8656b16725fd4de9943e`
