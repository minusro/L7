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
