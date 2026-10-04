# Storage and recording

SdFat through `SD.sdfs` (SDIO, DMA mode). Everything lives in `/looper`.

## Files

| Path | Contents |
|---|---|
| `samples/0001.wav` … `9999.wav` | Every recording, kept when no track uses it. 16-bit stereo WAV; a 512-byte header whose JUNK chunk holds `SampleInfo` {magic `M8SM`, version 3, frames, spb, loopBars, order, preroll} and the waveform overview (`OVERVIEW_COLS` = 286 peaks). Audio starts at byte 512 (`WAV_DATA_OFFSET`). After the loop's `frames` come `preroll` (128) frames: the input just before the recording's first frame, used to crossfade the loop's end into its start (version 3; trimming keeps them). |
| `track1.unit` … `track8.unit` | 512 bytes each: the track's `TrackUnit` {magic `M8TU`, version, sample, rangeStart, rangeLen, rangePhase, muted, filterLo, filterHi, volume, balance}. Overwritten in place. |
| `scenes.dat` | Created once at full size: block 0 the scene list, blocks 1… one scene record each (see scenes.md). Overwritten in place. |
| `session.txt` | The session: tempo and selected row and cell (`bpm=…`, `row=…`, `cell=…`; none saved: track 1's waveform cell). Rewritten only while stopped, ~2 s after a change, and at power-off. |

Samples are ordered by recording order (the `order` field; version-1 samples
by number) — this is what SHIFT+LEFT/RIGHT steps through.

## The rule: no file-system changes while the transport runs

Creating, resizing, deleting or renaming files stalls the card for hundreds of
ms to seconds and starves the streams. So:

* **Pool**: `POOL_SIZE` = 16 empty sample files (each 22.6 MB = 32 bars at
  60 BPM, `MAX_REC_FRAMES`), made ahead of time and refilled while stopped.
  A recording takes the lowest-numbered one. The next pool file is kept
  **open** (`poolNext`) so starting a recording costs no SD access.
  Only if the pool runs out while playing is a file created on the spot
  (logged as `no recording space left: creating … while playing`; ~0.7 s
  stall — this was the cause of the big dropouts seen on hardware with a pool
  of 4).
* Finished samples are **trimmed** to their length the next time the
  transport stops.
* Unit files, sample headers and `scenes.dat` blocks are written in place
  (512 bytes), and only when the playback buffers are topped up
  (`storageMayWrite()` / `streamsTopped()`).
* `renumberPool()` at boot renames pool files numbered below existing
  recordings, so new recordings always sort after old ones.
* Legacy per-track `trackN.wav` files are migrated at boot (recordings become
  samples, empty ones pool files).

## Recording

* `recordPress(t)` on an empty track: starts on the bar line just passed if
  pressed within `PREROLL_FRAMES` (~250 ms) of it (read back from the input
  history), otherwise waits for the next bar line (`WaitBar`). Stopped: the
  transport starts and recording begins on bar 1.
* Recordings are shifted by `LATENCY_FRAMES` (153, measured round trip).
* Stop: the loop is whole bars. `targetBars()`: if stopped within
  `SNAP_THRESHOLD_BEATS` (1 beat) after a bar line it ends there (trimmed),
  otherwise it carries on to the end of the current bar (`Finishing`).
  Longest: 32 bars.
* Playback of the new loop is gapless: the loop's beginning is copied into the
  track's ring while recording, so it can play the moment the loop wraps
  (`armRecordedLoop`).
* **Queued recording**: pressing record on another empty track while one
  records stops the current one and queues the new one; it starts exactly
  where the first ends (same sample, from the input history if needed —
  up to `INPUT_FRAMES - 2 * CHUNK_FRAMES` behind). Its file is taken at queue
  time.
* **Resampling** (`recordPress(t, true)`, SHIFT twice + A): records the
  app's output instead of the input — the engine writes each output frame
  into `inputRing` while it's on (`recording.resample`), no latency shift.
  Always starts on a bar line (the history before the press holds input). A
  recording queued behind one of the other kind starts on the next bar line
  instead of where it ends. Stopped: the other playing tracks go quiet where
  the loop ends (`silencedTracks` from `silenceFrom`, in the interrupt; each
  track's bit clears at its next clip switch, all on transport stop), so the
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
