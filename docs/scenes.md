# Scenes

A scene is a snapshot of the looper: the 7 track units (`SceneInfo` {magic
`M8SC`, version, float tempo, units[7]}; its tempo is only what it was when
saved: the tempo belongs to the project, the same in all its scenes).

## Projects

A project is a scene list with its current scene, the looper's selected
row and cell, its tempo and its master volume (block 0: `volume1`, the
level + 1, and `tempo10`, tenths of a BPM, in the places of a 249th and 250th
scene: 248 scenes at most; 0 in a project saved before, which then keeps the
level playing / its current scene's tempo). Changing either, in any scene,
changes the project's. `DATA/SESSION.DAT` names the loaded project and keeps the
tempo and master volume last set by hand (`newbpm=`, `newvolume=`; before
any: 120 BPM, 100%): a new project starts with them. Loading a project
doesn't change them. Twelve slots:
`/L7/PROJECTS/A.L7P` … `L.L7P`, and `/L7/DATA/CLIP.DAT` for the project
clipboard, each made once at full size; an empty slot has no file (made when
the slot is first loaded or pasted to, removed when it's cut or deleted).
Project files hold no audio: scenes name their samples. An older card's
`/L7/projects/1.dat` … `12.dat`, `clip.dat`, `none.dat` and the first
version's `/L7/scenes.dat` are moved to these names at boot.

* Loading (A on a slot in the menu): the loaded project's changes are written
  out, the other file opened and its current scene applied. Playing: on the
  next bar line, as a scene change, but a restarting one
  (`beginSceneChange(…, restart)`): every track, even one keeping its clip,
  is placed as `transportStart()` would at 1.1.1, and the clock counts from
  there (`transportOrigin()`); the transport itself runs on. Its tempo comes in with it, on
  that bar line. An empty slot gets a new project (no scenes: the empty
  scene, the tempo last set by hand). Not while recording.
* Cut / paste (SHIFT+A): copies the used blocks (the records, then the list
  last) to or from `CLIP.DAT`; cut then removes the slot's file. Stopped: at
  once. Playing: in the background, a block per `serviceScenes()` pass when
  the buffers allow (as scene saves); meanwhile the slots involved can't be
  loaded and no other copy starts. Not while recording. Cutting the loaded project empties its slot too and
  loads an empty dummy project (`DATA/NONE.DAT`, in no slot, emptied each time it's
  loaded; the accent then the splash's turquoise; for now), the tracks
  cleared. A new project (or the dummy) starts on track 1's waveform cell.
* A project with no clips at all is deleted when the menu opens (its slot
  emptied, not to the clipboard; the dummy project loaded).
* Each scene keeps the looper's selection (`SceneInfo.row`, `cell`, + 1):
  saved when leaving it (and with every save of the scene), restored when
  arriving; loading a project takes its current scene's. The list block's
  selection is the fallback (scenes saved before this).
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
  changed. With no scenes there is none: the list stays on 0\*.
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
  playback.md), mutes, filters, volumes, tracks starting or stopping; a
  project loading, its tempo too (engine applies `pendingSpb` exactly at
  `spbAtBeat`; the tempo label shows the playing tempo until then). Scenes
  keep the tempo.
* Stepping on before the bar reuses the same bar line (unless < 50 ms away;
  then the next one, with no gap).
* **Quick operations** — switch on the **next beat** (≥ 150 ms ahead), ignore
  the SHIFT hold, and pull any waiting change forward to that beat:
  * steps to and from N+1\*,
  * a step to the empty scene 0\* — but SHIFT held still holds it back:
    on the next beat once SHIFT is let go (leaving 0\* waits as usual;
    changing it makes it scene 1, as before),
  * the double-tap jumps to 0\* / N+1\* (second tap held `EDGE_HOLD_MS`),
  * scene cut, paste, duplicate (these first take a held target).
* **Label**: flashes white/dim with the beat (first half of each beat white)
  while the selected scene isn't the one playing — SHIFT still held on
  another scene, or a change waiting for its switch point.
* Stopped: scene changes apply at once.
* No scene changes while recording.
* **After a resampling** (`sceneFromResample(t)`, from `followRecording()`):
  a new scene is inserted after the current one, holding
  the resampled clip, on the same track, and the current scene's muted clips,
  still muted (copied: they stay in the current scene too); the resampled
  clip leaves the current scene. The new scene is selected and goes on on the next beat (the other
  tracks were already silenced where the loop ended), with that track
  selected. A recording
  queued behind the resampling carries on and is left out of the switch.

A half-bar grid for normal changes was tried and reverted (commits 506c0d7 /
786f884): normal changes stay on bar lines.
