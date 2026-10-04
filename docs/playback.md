# Playback and clip switching

## Streams

Each track has a ring of `RING_FRAMES` stereo frames. `loop()` fills it
(`produce()`: one SD read of up to `CHUNK_FRAMES` = 16 KB) with the loop range's
frames in playback order; the interrupt plays it at `playSpb / spb`
(varispeed: tempo changes repitch; any ratio the 30–240 BPM range allows,
1/8x–8x — high ratios need proportionally more card bandwidth). Counters `head` / `tail` run freely; ring index = n % size.

`serviceStreams()` refills the emptiest track first. Writes (recording, unit
files, scenes) wait until every ring is topped up (`streamsTopped()`), so a
slow write starts with the most playback in reserve.

**Start handshake**: `startTrack(t, beat0, fillNow)` sets `s0` (stream frame
at `startBeat`) and arms the track; the interrupt goes live when the clock
reaches `startBeat`. Every beat, a live track's `tail` is snapped to
`s0 + (beat - startBeat) * playSpb`.

## Declicking

* **Jumps in the stream** (a range's end back to its start, the loop's end
  back to frame 0): the `XFADE_FRAMES` (128, ~3 ms) before the jump are
  crossfaded in `produce()` into the frames that precede the jump's target in
  the recording, so the target follows on smoothly and keeps its attack.
  Before frame 0 that's the recording's stored **pre-roll** (sample version
  3, see storage.md); older samples, or a recording not saved yet, fade into
  silence instead. The frames before each target are cached per track
  (`framesBefore`, two per track) so wraps cost no extra reads.
* **Switches** (`applyPlan`, `applyQueuedRange`): the ring's last 128 frames
  before the switch point crossfade into what precedes the new clip's entry
  frame (`crossfadeInto`; the originals are kept so a replanned switch starts
  over from them). A switch to "empty" fades out instead.
* **Levels** (engine): the four mix gains glide (one-pole, `GAIN_GLIDE_MS` =
  1 ms) — mute, unmute, volume/balance, a track joining (from silence).
  Stopping a playing track goes through `silenceTracks()`: it sets
  `stopping`, the interrupt glides it to silence and takes it off, and
  `loop()` waits for that (a few ms) — used by transport stop, snap to bar,
  clearing a track and restarting one.

## Loop ranges

* Any start beat, any length from 1 beat to the whole loop, may wrap past the
  loop's end. `rangeAnchorQ` = transport quarter beat at which the range is at
  its first beat (the alignment); stored in units as `rangePhase`.
* Changing a range while playing is queued (`rangePending`) and switches on
  the first quarter beat the stream hasn't buffered (`applyQueuedRange`).
* **Start position** (`startPosition()`): where the transport starting at
  1.1.1 plays each track from — a range of 1 beat: its start; shorter than a
  bar, or in a loop of up to 8 bars (`BEAT_STEP_MAX_BARS`, where the range
  moves a beat at a time): the first bar start or bar middle in it, whichever
  comes first; otherwise the first bar start in it.

## Gapless clip changes (`Change` → `Plan` → engine switch)

Used by scene changes and SHIFT+LEFT/RIGHT sample stepping while playing.

1. `setTrackUnit` / `stepSample` record a **Change** for the track: the target
   unit and the quarter beat `q` it's due on (sample step: first quarter beat
   ≥ `SWITCH_LEAD_MS` = 150 ms ahead; scenes: see scenes.md).
2. `serviceChange()` sets up the clip switch:
   * live track → `planSwitch()` makes a **Plan**: opens the new sample now,
     computes the switch frame `switchS` from the engine's current mapping.
     If the ring already holds old audio past `switchS`, `head` is rewound to
     it (old buffered rest dropped) and `applyPlan()` runs at once; otherwise
     it runs when the stream reaches `switchS`.
   * `applyPlan()` switches the producer to the new clip and tells the
     interrupt (`sampleSwitch`, `switchAt`, `nextS0`, `nextStartBeat`,
     `nextSpb`). The interrupt keeps playing the old clip up to `switchAt`,
     then takes the new mapping (different recording tempo included). A
     switch to "empty" uses `nextSpb = 0`: the track stops there.
   * track not live (empty or waiting) → loads now, starts on the beat of `q`.
3. When the clock passes `q` and the stream has switched, the change's
   settings (mute, filter, volume/balance) are applied, or the track is
   cleared.

Details:
* A planned switch can be retargeted to the same point up to `PLAN_LATE_MS`
  (50 ms) before it, or moved **earlier** (the engine hasn't reached it).
  Moving it later waits for the pending one first.
* **Playhead continuity**: if the new range is as long (in beats) as the one
  playing, the new clip takes over the old anchor, so it continues from the
  same spot. Different lengths use the unit's stored phase.
* Edits made while a change waits (filter, volume, mute via
  `setTrackMuted`) are copied into the change, so they survive the switch.
* B+arrow range edits are refused while a change is on its way.
* Transport stop / snap-to-bar settle all changes at once (`settleNow`).

Measured in the simulator (realistic card, 120 BPM, all 8 tracks switching):
new samples heard ~230 ms after the request (max ~280 ms) with no silence;
before this design a switch left 1–2 beats of silence (~770 ms average).
