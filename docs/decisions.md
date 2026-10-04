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
* The master volume is not part of a project: it stays global.
* Projects can be saved separately (several on the card). A project file
  holds no audio, only references to the sample files its scenes use.
* The loaded project is autosaved.
* Loading works while playing; cut and paste only while stopped.

## Open ideas (not started)

* Overdub with undo (agreed early on as required).
* Master effects; what the menu's buttons do.
* Possibly: shrink the input history to give the track rings more RAM.
