# Scenes

A scene is a snapshot of the looper: the 8 track units and the tempo
(`SceneInfo` {magic `M8SC`, version, float tempo (0: keep), units[8]}).

## Projects

A project is a scene list with its current scene and the looper's selected
row and cell (the master volume and the tempo outside scenes are global, in
`session.txt`, which also names the loaded project). Eight slots:
`/looper/projects/1.dat` … `8.dat`, and `clip.dat` for the project clipboard,
all made once at full size; an empty slot's block 0 is zeros. Project files
hold no audio: scenes name their samples. The first version's
`/looper/scenes.dat` becomes project 1.

* Loading (A on a slot in the menu): the loaded project's changes are written
  out, the other file opened and its current scene applied like a scene
  change (playing: on a bar line). An empty slot gets a new project (no
  scenes: the empty scene, the tempo kept). Not while recording.
* Cut / paste (SHIFT+A): copies the used blocks (the records, then the list
  last) to or from `clip.dat`; cut then zeroes the slot's block 0. Only while
  stopped (many writes). Cutting the loaded project puts a new empty one in
  its place (the tracks cleared).
* The selection is saved in the list block (`row`, `cell`, each + 1; 0:
  none) a couple of seconds after it changes, before loading another
  project, and at power off.

## The list

* Stored in the project's file: block 0 = `SceneList` {magic `M8SL`, version,
  count, current, row, cell, slot[250]}; scene records in blocks 1…, in slots reused as
  scenes come and go (inserting/removing only rewrites block 0).
  `MAX_SCENES` = 250.
* Position **0\*** is the empty scene, never stored; changing anything there
  inserts a new scene 1 (the others move up).
* Position **N+1\*** (after the last scene N) is a copy of N, stored only once
  changed.
* On a stored scene every change updates it (`sceneStateChanged()` → `commit()`;
  written one block at a time when the buffers allow).
* The label shows `SCN n/N`; the empty scene shows as `0/N` and the copy after
  the last as `*/N` (both grey).

## Switching while playing

* B + UP/DOWN on the status row's scene cell works exactly like SHIFT + UP/DOWN
  (B held counts as SHIFT held below).
* **SHIFT held** (clock running): SHIFT+UP/DOWN moves the target shown in the
  label (`sceneHold`, `sceneHeld()`), and the target is **prepared**: its
  samples' files are opened in the background, one per loop pass
  (`prepareUnits()`, `unitsPrepared()`). Letting go of SHIFT launches the
  change at once if it's prepared, otherwise as soon as it is; the label
  flashes until the change happens. A prepared change needs only ~60 ms of
  lead before its switch point instead of 150 ms.
* A normal change then takes effect on the **first bar line at least 150 ms
  ahead** (`beginSceneChange()`), all of it together: samples (gapless, see
  playback.md), mutes, filters, volumes, tracks starting or stopping, and the
  tempo (engine applies `pendingSpb` exactly at `spbAtBeat`). The tempo label
  shows the playing tempo until then.
* Stepping on before the bar reuses the same bar line (unless < 50 ms away;
  then the next one, with no gap).
* **Quick operations** — switch on the **next beat** (≥ 150 ms ahead), ignore
  the SHIFT hold, and pull any waiting change forward to that beat:
  * steps to and from N+1\*,
  * the double-tap jumps to 0\* / N+1\* (second tap held `EDGE_HOLD_MS`),
  * scene cut, paste, duplicate (these first take a held target).
* **Label**: flashes white/dim with the beat (first half of each beat white)
  while the selected scene isn't the one playing — SHIFT still held on
  another scene, or a change waiting for its switch point.
* Stopped: scene changes apply at once.
* No scene changes while recording.
* **After a resampling** (`sceneFromResample(t)`, from `followRecording()`):
  a new scene with the same tempo is inserted after the current one, holding
  just the resampled clip, on the same track; the clip leaves the current
  scene. The new scene is selected and goes on on the next beat (the other
  tracks were already silenced where the loop ended), with that track
  selected. A recording
  queued behind the resampling carries on and is left out of the switch.

A half-bar grid for normal changes was tried and reverted (commits 506c0d7 /
786f884): normal changes stay on bar lines.
