# Scenes

A scene is a snapshot of the looper: the 7 track units (`SceneInfo` {magic
`M8SC`, version, float tempo, units[7]}; its tempo is only what it was when
saved: the tempo belongs to the project, the same in all its scenes).

## Projects

A project is a scene list with its current scene, the looper's selected
row and cell, and its tempo (block 0: `tempo10`, tenths of a BPM, in the
place of a 250th scene: room for 248 scenes, `LIST_SLOTS`; 200 at most for
now, `MAX_SCENES`; 0 in a project saved before, which
then keeps its current scene's tempo; `volume1` next to it is no longer used).
Changing the tempo, in any scene, changes the project's. The master volume is
the app's, one for all projects (in the session). `DATA/SESSION.DAT` names the
loaded project and keeps the tempo last set by hand (`newbpm=`; before any:
120 BPM): a new project starts with it. Loading a project doesn't change it. Twelve slots:
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
  loaded and no other copy starts. Not while recording. A project with no
  clips isn't copied: cutting it only empties its slot (as a delete), the
  clipboard keeping what it had, so the clipboard only ever holds clips and
  paste is off while it's empty. Cutting the loaded project empties its slot too and
  loads an empty dummy project (`DATA/NONE.DAT`, in no slot, emptied each time it's
  loaded; the accent then the splash's turquoise; for now), the tracks
  cleared.
* A project with no clips at all is deleted when the menu opens (its slot
  emptied, not to the clipboard; the dummy project loaded).
* The selection (row and cell) is the app's, not a project's or a scene's:
  loading a project or stepping to another scene leaves it where it is. It's
  kept in the session (`SESSION.DAT`). (`SceneInfo.row`/`cell` and
  `SceneList.row`/`cell`, where scenes and projects kept one before, are
  unused.)
* Each scene keeps a tempo of its own, its saved tempo (`SceneInfo.tempo`),
  played from its switch on (`apply()`; 0* and N+1* keep the tempo as it is): the tempo it was made at (0*
  or N+1* made a scene by a change there: the tempo then, or a recording's
  own; a duplicate: the tempo then; a resampling's scene: the resampling's; a
  pasted scene keeps its own), then its latest recording's (`sceneRecorded()`,
  as a recording is saved; the tempo set by hand changes it too, `projectTempoChanged()`; on 0* or N+1* a tempo set by hand is a change: it's made a scene with that tempo; an external tempo followed changes none of it). B twice on
  the tempo cell goes back to the current scene's (`sceneSavedTempo()`; N+1*:
  N's; 0*: none). Before, a scene's record held the tempo when it was last
  saved, and the list block (`recTempo10`, now unused) the project's last
  recording's.
* `projectEdits()` counts the loaded project's changes (the tracks edited by
  hand or recorded, scenes cut / pasted / duplicated, the tempo set by hand;
  not loading it or stepping through its scenes): the menu's "SAVED AS" label
  stays until it moves.

## The list

* Stored in the project's file: block 0 = `SceneList` {magic `M8SL`, version,
  count, current, row, cell, slot[248]}; scene records in blocks 1…, in slots reused as
  scenes come and go (inserting/removing only rewrites block 0).
  `LIST_SLOTS` = 248 (the file's room), `MAX_SCENES` = 200 (the most a project
  takes now: none added past it; a file with more loads as it is).
* Position **0\*** is the empty scene, never stored; changing anything there
  inserts a new scene 1 (the others move up).
* Position **N+1\*** (after the last scene N) is a copy of N, stored only once
  changed. With no scenes there is none: the list stays on 0\*.
* On a stored scene every change updates it (`sceneStateChanged()` → `commit()`;
  written one block at a time when the buffers allow).
* The label shows `SCN n/N` (light grey, white with the cell selected); the empty scene shows as `0/N` and the copy after
  the last as `*/N`. `0/N` grey (light grey while held, stepping); `*/N` its `*`
  in the accent colour, its `/N` as any position's (held: the `*` white, the
  `/N` in the accent colour).

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
* **Switching scenes** (`go()`): at the switch, a track whose range is as long
  as the one it had plays as far from its start position as that one was from
  its own (`keepLen`); any other from its start position (`beginSceneChange(...,
  restart, clockToo = false)`: as a project switch, but the clock runs on).
  Between N and N+1* (its copy) nothing starts over.
* **After a resampling** (`sceneFromResample(t)`, from `followRecording()`):
  a new scene is inserted after the current one, holding
  the resampled clip, on track 1 (the bottom row; another recording running there: on its own track; its volume a step over unity: the mix it recorded already had the tracks' headroom, `masterGain` ~-3.1 dB, in it, so it plays as loud as what it resampled), and the current scene's muted clips,
  still muted (copied: they stay in the current scene too), in their order on the tracks right after it (2, 3 ...: no empty tracks between; a track another recording runs on stepped over); the resampled
  clip leaves the current scene. The new scene is selected and goes on on the next beat (the other
  tracks were already silenced where the loop ended), with that track
  selected. A recording
  queued behind the resampling carries on and is left out of the switch.

A half-bar grid for normal changes was tried and reverted (commits 506c0d7 /
786f884): normal changes stay on bar lines.
