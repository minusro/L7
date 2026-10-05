# Decisions, preferences and limits

## Agreed behaviour (don't change without asking)

* LINE IN input, always monitored. Fixed 4/4. Stereo tracks streamed from SD.
* Tempo changes repitch (varispeed); 30–240 BPM, with no per-loop speed
  limit (was 50–200 % of the recorded tempo; dropped so the whole range is
  reachable).
* Recordings are whole bars (any count up to 32), trimmed back to the bar
  line if stopped within a beat after it, else completed to the bar's end.
  (An early design snapped to powers of two; that was replaced.)
* MIDI clock out only (serial port), no clock input.
* Start position rule for 1.1.1 (playback.md).
* Scene semantics: 0\* empty, N+1\* temporary copy, changes on bar lines after
  SHIFT is released, quick operations on the next beat (scenes.md).
* Scene changes must not leave silence; same-length loops keep their
  playhead position across switches.

## Working preferences of the author

* Precise, iterative UI tweaks: colours, sizes, icon shapes are adjusted by
  eye on the device — make the requested change exactly, keep the rest.
* Double-tap window 200 ms (100 ms was too short).
* Comments in the code describe behaviour in plain sentences; match that
  style and keep the key list in `m8_looper.ino` up to date.
* The author sometimes reverts experiments through git. Commits: after each
  successful upload (build-and-test.md).

## Known limits

* RAM is nearly full: ring/input buffers can't grow. Tracks played much
  faster than recorded can drop out on the card's slow writes during
  recording (simulator: no dropouts at native tempo with 7 tracks + a
  recording).
* More than 16 recordings without stopping exhausts the pool → ~0.7 s stall.
* Card nearly full: the pool stays short (each empty file is ~22 MB); after a
  failed try it isn't retried until trimming a recording frees space (each
  try blocks the loop ~1 s — retrying every pass made the app crawl).
* Below ~110 BPM a queued recording after a trimmed one may start on the next
  bar (input history too short to catch up).
* B+arrow range edits are ignored on a track while a clip change waits.
* A switch replanned < 50 ms before its point goes to the next one.

## Projects (agreed 2026-10-04; 12 slots in the menu, scenes.md)

* A project is a list of scenes plus the state that belongs to none of
  them: which scene is selected, and the main screen's last selected cell
  (more may come later).
* Projects can be saved separately (several on the card). A project file
  holds no audio, only references to the sample files its scenes use.
* The loaded project is autosaved.
* Loading, cut and paste all work while playing (copies in the background).
* The tempo and the master volume are the project's (agreed 2026-10-04):
  changing one in a scene changes it in all of them. New projects start with
  the tempo and master volume last set by hand (before any: 120 BPM, 100%);
  loading a project without changing them leaves those as they were.

## L7: 7 tracks (agreed 2026-10-04)

* The app is called L7. Seven tracks instead of eight: the track rows are
  taller (30 px), the status row stays as it was, and all seven rings fit in
  RAM2.
* A scene holds 7 track units. No conversion of 8-track projects (none
  exist).
* The app's folder is `/L7` (an older `/looper` is renamed at boot); the
  files' tags (`M8SC`, `M8TU`, ...) stay. Recordings are named `XYZZZZ.wav`
  (project letter, track number, number: counting per project and track, so
  `A13333`, `A23333` and `B43333` can all exist), and SHIFT + LEFT/RIGHT steps
  through them in name order.

## Storage mode, over USB (2026-10-05)

* The card is shown to a computer as a mounted drive (USB mass storage). The
  Teensy 4 core has no such USB type, so `tools/teensy_msc/patch.py` adds one
  to Teensyduino ("Serial + Mass Storage (L7 patch)", `usb=serialmsc`, flag
  `USB_SERIAL_MSC`; reversible, everything behind that flag) with a bulk-only
  SCSI engine (`usb_msc.c`). Read ~7.5 MB/s, write ~3 MB/s over USB 2.0 (the
  card on the Teensy's SDIO).
* Outside storage mode the M8 answers the computer's disk commands as an empty
  card reader, from a timer (queued answers, never waited for, so safe in an
  interrupt): unanswered, macOS resets the USB connection every ~11 s.
* The app is stopped and written out first; the card is the computer's. Eject
  on the computer (or hold B) restarts the app.
* RAM: the recordings list (20 KB) is in RAM1, and the engine's buffer (16 KB)
  too (RAM1 free ~78 KB).
* A first attempt froze the M8 (USB gone, torn graphics) as soon as the computer
  talked to it. Causes: one `transfer_t` queued twice in a row (the core links
  a new transfer behind the endpoint's last, so it ended up pointing at itself),
  and answers waited for inside an interrupt. A test watchdog (WDOG1) left
  running by a test build persists across resets until power is cycled, which
  confused the next tests. A clear-halt must reset the endpoint's data toggle
  (patched into usb.c).
* An MTP build (`usb=serialmtp`, no patch) also works, as a device in Android
  File Transfer rather than a drive; the recordings list is in RAM1 for its
  buffers too.

## Card layout (agreed 2026-10-05)

`/L7` has four folders: `SAMPLES` (all samples; recordings end up here),
`PROJECTS` (the 12 project files `A.L7P` … `L.L7P`; an empty project has no
file), `EXPORTS` (the Ableton projects) and `DATA` (everything else the app
needs, the temporary audio files too: recording space, and a recording's file
until it's named and moved to `SAMPLES`). An older card is moved over at boot
(see storage.md). A project's file is made at full size when its slot is first
loaded or pasted to (a short stall if playing), so saving never allocates.

## Ableton Live Set export (agreed 2026-10-04)

* The loaded project, to `/L7/EXPORTS/<app name> <letter><001-999> Project/`
  (named as Live names its project folders): the next number for that letter. Its samples are copied into the folder
  (`Samples/Imported/`), so it stands on its own.
* The folder is a Live project: it has an (empty) `Ableton Project Info`
  folder, so the clips' sample paths, relative to it, resolve (without it
  Live showed every sample offline).
* The set (Live 11): the project's tempo; 7 audio tracks, each
  with two Auto Filters, highpass then lowpass, fully open, and a Utility (as with no clip
  playing); a scene per project scene with clips (empty ones, like 0*, left
  out); each track's clip there: Re-Pitch warp
  mode, the loop and start position as in the app (a range across the
  loop's end, which Live can't loop: the whole loop), deactivated where the
  track is muted; the current scene's clips saved as playing (triggered when
  the set opens).
* An empty scene is added last (Live's usual spare one to launch into).
* Scenes have no name. The 7 tracks and every clip are coloured with the
  palette entry of Live (the 70 clip colours) nearest, by CIE Lab distance,
  to the project's colour (plain RGB distance picked greys).
* The folder has Live's custom icon on a Mac: an empty `Icon<CR>` file, and
  the AppleDouble files (`._Icon<CR>` with the icon as resource fork and the
  invisible flag, `/L7/EXPORTS/._<folder>` with the custom-icon flag) a Mac
  itself writes on a FAT card for the folder's extended attributes. Written
  by hand (ableton.cpp, icon data from tools/ableton/make_icon.py). SdFat
  refuses control characters in names, so `patch.py` also lets it take a CR
  (re-run it after a Teensyduino update). Checked on a FAT disk image: macOS
  read the flags and the icon from such files.
* Each clip carries its track's settings: the clip's own gain is the volume
  (`SampleVolume`, linear: 0 dB = 1, from the volume step), and three flat
  clip envelopes (a value at the start and at 0): the Utility's balance (the
  pan, balance / BALANCE_MAX, -1..1) and the two Auto Filters' cutoffs (a
  note number, Hz = 440 * 2^((n - 69) / 12), 20-135; fully open: 135 lowpass,
  20 highpass). Every clip has all of them, off (muted) ones too, so a clip
  with open filters resets what the one before it left. The envelopes point
  at the parameters' automation targets (structure and ids as in a set with
  clip envelopes drawn in Live, `docs/design/ableton/L7 Test 2 Project`, which
  is now the template's source). The gain's scale and the balance's range are
  Live's usual ones, not from a Live example: to be checked in Live.
* A track's devices, in this order: Auto Filter highpass, Auto Filter
  lowpass, Utility (its balance automated by the clips). The track's own
  mixer volume and pan stay at their defaults. (The example's master
  Limiter isn't in the export.)

## Open ideas (not started)

* Master effects; what the menu's buttons do.
* Possibly: shrink the input history to give the track rings more RAM.
