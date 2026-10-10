# Storage and recording

SdFat through `SD.sdfs` (SDIO, DMA mode; `sdBegin()`, which also has files stamped with the Teensy's clock, as `SD.begin()` would: set by the Teensy Loader at every upload, kept through power-off only if the board's RTC is battery-backed; before 1980: no date). Everything lives in `/L7` (an
older card's `/looper` is renamed to it at boot), in four folders:

* `SAMPLES`: all the samples the tracks use; recordings end up here.
* `PROJECTS`: the 12 project files, `A.L7P` … `L.L7P` (an empty slot has none).
* `EXPORTS`: Ableton Live Sets (see ableton.h):
  `EXPORTS/L7 A005 Project/L7 A005.als` and its samples in `Samples/Imported/`.
* `DATA`: everything else the app needs, including the temporary audio files
  (recording space, and a recording's file until it's named and moved into
  `SAMPLES`).

An older card's layout (`/L7/samples`, `/L7/projects`, `/Ableton`, and the
unit, session and recording-space files) is moved into these at boot
(`storageBegin()`, `makeProjectFiles()`); the folders are renamed to upper
case if made in lower case. Export folders made before ("L7 A005") are left
as they are. DATA's files from the layout before (`NNNN.wav`, `TRACKn.UNIT`,
`SESSION.TXT`) are renamed to `.DAT` ones.

## Files

| Path | Contents |
|---|---|
| `SAMPLES/A30012.wav` … | The recordings, named `XYZZZZ.wav`: X the letter of the project loaded when it was recorded (A–L), Y the track's number on the screen (1–7), ZZZZ its number (0001–9999) **counting per project and track**: `A13333`, `A23333` and `B43333` can all exist (recordings from before: just `NNNN.wav`; those named by the version before this one, `A10019.wav`, keep being known by their number, see `older` in storage.cpp). A recording is known (by the scenes) by this name from the moment it starts, but it goes into recording space (`DATA/NNNN.DAT`, made ahead; with no project letter, as in the dummy project, the recording gets the next plain number) and its file is moved to `SAMPLES/` under its name when it's cut down to size, stopped (open handles on it are reopened at the same position). The name is kept in the header too (after the overview), so one cut short by power-off is named at the next boot. One no project uses any more (no track of any scene in any project, the clipboards and the dummy project included, nor the tracks now) can be deleted by `deleteUnusedSamples()` (stopped only), which runs whenever the menu opens only if the feature flag `DELETE_UNUSED_SAMPLES` (config.h) is on; it is off for now, so unused recordings stay on the card. 16-bit stereo WAV; a 512-byte header whose JUNK chunk holds `SampleInfo` {magic `M8SM`, version 3, frames, spb, loopBars, order, preroll}, the waveform overview (`OVERVIEW_COLS` = 286 peaks) the name (letter, track, 16-bit number; zeros: none) and, for a recording made while the tempo changed, its bar lines (`BR`, the count, a spare byte, then bars 1 … loopBars' frames as 32-bit numbers, the last its end; none: evenly at its tempo). Audio starts at byte 512 (`WAV_DATA_OFFSET`). After the loop's `frames` come `preroll` (128) frames: the input just before the recording's first frame, used to crossfade the loop's end into its start (version 3; trimming keeps them). |
| `DATA/TRACK1.DAT` … `TRACK7.DAT` | 512 bytes each: the track's `TrackUnit` {magic `M8TU`, version, sample, rangeStart, rangeLen, rangePhase, muted, filterLo, filterHi, volume, balance}. Overwritten in place. |
| `PROJECTS/A.L7P` … `L.L7P`, `DATA/CLIP.DAT`, `DATA/NONE.DAT` | The project slots (a file only while the slot holds a project), the project clipboard and the dummy project, each created once at full size: block 0 the scene list (with the selected row and cell), blocks 1… one scene record each (see scenes.md). Overwritten in place. |
| `DATA/SESSION.DAT` | The session: tempo, the tempo last set by hand (new projects start with it; 120 before any), master volume (one for all projects), loaded project, the menu (`bpm=…`, `newbpm=…`, `volume=…`, `project=…` 1-12, `project=0`: the dummy project; `menu=…` the menu's selected item or 100 + a status cell, + 1000 if open (the app starts in the menu then, else in the looper's view); `playing=1`: the clock was running (saved at power-off), started again at boot; `row=`/`cell=` the looper's selection, the app's: projects and scenes don't keep one; `metro=` the metronome, 0 off / 1 auto / 2 on). Rewritten only while stopped, ~2 s after a change, and at power-off. |

A sample's id (what scenes store, `TrackUnit.sample`) is its tag (project
letter and track) and number: `tag << 14 | number`; without a tag, the number
alone. The card's recordings are a sorted list of ids (`sampleIds`, up to 5000
in RAM2), so SHIFT+LEFT/RIGHT steps through them in the order of their names
(wrapping): from a track's sample, the next names are the same project and
track's, then the project's other tracks', then other projects' (older
`NNNN.wav` ones come first); an empty track starts from the first (or last)
of its own in the loaded project. (The `order` field, recording order, isn't
used for this any more.)

## The rule: no file-system changes while the transport runs

Creating, resizing, deleting or renaming files stalls the card for hundreds of
ms to seconds and starves the streams. So:

* **Pool**: `POOL_SIZE` = 8 empty sample files (each 22.6 MB = 32 bars at
  60 BPM, `MAX_REC_FRAMES`, ~180 MB), made ahead of time and refilled while
  stopped. They are named from the lowest free numbers (`DATA/0001.DAT` …
  `0008.DAT`, never ever higher ones: a recording's file is renamed into
  `SAMPLES` anyway, which frees its number). A card with more of them, or
  numbered higher, is brought down to those at boot (the extra ones removed).
  A recording takes the lowest-numbered one. The next pool file is kept
  **open** (`poolNext`) so starting a recording costs no SD access.
  Only if the pool runs out while playing is a file created on the spot
  (logged as `no recording space left: creating … while playing`; ~0.7 s
  stall — this was the cause of the big dropouts seen on hardware with a pool
  of 4; 8 is a compromise on card space).
* Finished samples are **trimmed** to their length the next time the
  transport stops.
* Unit files, sample headers and `scenes.dat` blocks are written in place
  (512 bytes), and only when the playback buffers are topped up
  (`storageMayWrite()` / `streamsTopped()`).
* Legacy per-track `trackN.wav` files are migrated at boot (recordings become
  samples, empty ones pool files).

## Recording

* `recordPress(t)` on an empty track: starts on the bar line just passed if
  pressed within `PREROLL_FRAMES` (~250 ms) of it (read back from the input
  history), otherwise waits for the next bar line (`WaitBar`). Stopped: the
  transport starts and recording begins on bar 1.
* Recordings are shifted by `LATENCY_FRAMES` (153, measured round trip).
* **The tempo may change while recording** (by hand, tap tempo): the
  interrupt keeps the clock's tempo marks from the recording's first bar line
  (`recording.marks`: at each change, the transport sample, the beat position
  there and the samples per beat; up to 32). The clip is written as it comes
  (input sample `startSample + k` is frame k, no stretching). The marks say
  where its bars fall: the bars it's stopped at count the clock's beats, its
  length is the input from its first bar line to its last, and a queued
  recording starts on the input sample of the end's bar line. It plays at its
  **average tempo** (its `spbNative` = its length / its beats), so it keeps in
  step with the clock over the whole loop, while its bars drift inside it.
  Each bar line's frame is kept in its header (see Files) and exported as
  Ableton warp markers (beat 4·b at that frame), so Live plays every bar at
  the tempo it was played at (`recBeatsAt()`, `recFramesAtBeats()`,
  `recBarFrames()` in storage.cpp; the simulator checks a 4-bar take with
  changes to 100 and 140 BPM). With no change, nothing differs from before
  and no bar lines are stored.
* Stop: the loop is whole bars. `targetBars()`: if stopped within
  `SNAP_THRESHOLD_BEATS` (1 beat) after a bar line it ends there (trimmed),
  otherwise it carries on to the end of the current bar (`Finishing`).
  Longest: 32 bars at 60 BPM and over, 16 bars below (`maxBarsNow()`: 16 bars at 30 BPM still fit a file).
* Playback of the new loop is gapless: the loop's beginning is copied into the
  track's ring while recording, so it can play the moment the loop wraps
  (`armRecordedLoop`).
* **Queued recording**: pressing record on another empty track while one
  records stops the current one and queues the new one; it starts exactly
  where the first ends (same sample, from the input history if needed —
  up to `INPUT_FRAMES - 2 * CHUNK_FRAMES` behind). Its file is taken at queue
  time.
* **Resampling** (`recordPress(t, true)`, B+A on an empty track, offered only
  while a clip is playing unmuted): records the
  app's output instead of the input — the engine writes each output frame
  into `inputRing` while it's on (`recording.resample`), no latency shift. The
  master volume isn't in it: neither the playing tracks (with their own
  volume, balance and filters) nor the audio input are scaled by it.
  Always starts on a bar line (the history before the press holds input). A
  recording queued behind one of the other kind starts on the next bar line
  instead of where it ends. Stopped: the other playing tracks go quiet where
  the loop ends (`silencedTracks` from `silenceFrom`, in the interrupt; each
  track's bit clears at its next clip switch, all on transport stop; a muted
  clip carried into the new scene is cleared by `hearAgain()`), so the
  new loop takes over from what it recorded. Saved: `takeResampled()`
  reports it once, and the app makes the new scene (scenes.md).
* **Cancel** (`cancelRecording(t)`, SHIFT+A): stops a recording at once,
  running or finishing, and throws it away: the track is empty again and its
  file goes back to the pool (no header was written, so it's an empty pool
  file again; nothing is deleted while the transport runs). A queued
  recording then starts from the next bar line.
* The pre-roll (the 128 input frames before the first one) is copied from
  the input history once recording is under way and written after the
  loop's frames when it finishes.
* Writes are 16 KB chunks aligned to 16 KB file offsets (the first one is
  shorter because audio starts at byte 512).
* **Overruns**: if the card falls so far behind that the input history
  overflows, the lost stretch is skipped and remembered as a gap; gaps are
  written as silence when the card has time, always before the recording
  finishes (never left as old card data, which played back as noise).
  Logged as `recording fell behind (...)`.
* While recording, the live waveform's scale shows the room as powers of two
  (1, 2, 4, 8, 16, 32 bars); once stopped it shows the real length.

## Track units, clipboard

* `unitOf(t)` gives a track's unit (a change on its way counts);
  `setTrackUnit(t, u)` applies one (scenes use it).
* Clipboard: SHIFT+A cuts a track's unit (the sample stays on the card) or
  pastes it onto an empty track, keeping its alignment.
* `trackChanged(t)` marks the unit file dirty and lets the current scene
  follow (not while a scene is being applied: `applyingUnit`).
