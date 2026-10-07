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

Measured in the simulator (realistic card, 120 BPM, all 8 tracks switching, before L7 went to 7):
new samples heard ~230 ms after the request (max ~280 ms) with no silence;
before this design a switch left 1–2 beats of silence (~770 ms average).

## Metronome

A setting (the session's `metro=`; SHIFT+A on the tempo cell): 0 (off), A
(auto: only in a silent scene, no unmuted clip playing; the default) or 1
(on). While the clock runs and it says so (never in the dummy project), the engine clicks on every beat: a
small wood block (two damped sines, ~1.15 kHz with a harder partial; 1.6 kHz
and louder on the bar's first beat). It's added after the master volume and
left out of what a resampling records. A recording started with it clicking keeps it
until it's completely recorded: on auto it stops on the bar line where that
recording ends and the clip starts playing (it goes on while the recording
finishes its bar; that bar line itself doesn't click). While a scene or project
switch waits for its switch point, nothing clicks, whatever the setting: from
the press until the switch (`clickFromBeat` = `sceneSwitchBeat()`, set by
`loop()`; the switch's own beat clicks if the new scene should).

## MIDI clock

**Out** (`engine.cpp`, `MIDI_OUT_ENABLED`): 24 ticks a beat from the audio
interrupt (each queued with the time it's due and sent by a 200 µs timer:
<0.2 ms of jitter), Start on `transportStart()` (started at another beat,
following an external clock's song position: Song Position and Continue
instead), Stop on `transportStop()`.
Port 2 on variant B (Serial3, TX pin 14), port 1 on variant A.

**In** (`midi_in.*`, `MIDI_IN_ENABLED`): the serial MIDI in (variant B: port 1,
Serial1 RX pin 0, opened receive-only, its TX pin 1 given back as the early
boot line, HIGH; variant A: Serial7 RX 28, unverified), read by the same 200 µs
timer, each clock tick stamped with `micros()`. In `loop()` (`midiInService()`):
* Following: their transport running, from their Start (or Continue) until
  their Stop or no tick for 0.5 s (`midiClockFollowing()`; their Start counts
  as a tick for that: a DAW over USB sends no clock while stopped, and its
  Start may be taken in before its first tick, which then cancelled the start
  about half the time). Their ticks while
  they're stopped (some send clock all the time) are timed but not followed:
  the tempo stays free to change by hand, and their Start takes theirs at
  once. Their tempo: two alpha-beta trackers on their ticks' times (each tick
  the next one predicted, its miss moving the prediction and the tick period
  a little): a slow one, whose tempo is used (steady through jitter), and a
  fast one; the fast one 0.6 % off the slow one for 4 ticks running means
  their tempo changed: the slow one takes its state and its gains, easing
  back to its own over a beat. After a pause tracked afresh from the tick
  after the first (often late after a Start), from the tempo known (played
  again at the same tempo, it doesn't move). Modelled with ±0.8 ms of jitter:
  steady within 0.054 %; a change of 1 to 10 BPM followed to within 0.2 % in
  ~11 ticks (the simulator: 120 to 123 while playing, the tempo shown right
  after 19 ticks; before, up to 4 beats). Applied every quarter beat; shown
  to a tenth of a BPM, changing only once clear of the last (no flicker). Measured in
  `micros()` turned into samples by the ratio measured from the audio blocks'
  start times (`blockStamp`, written by the interrupt each block). Shown in the
  tempo cell to a tenth of a BPM; not the project's tempo, not saved as set by
  hand; meanwhile `setTempo()` / a project's tempo do nothing (tap tempo,
  B + arrows: no effect).
* Running: each tick, how far our beat is from theirs at that moment (our
  position from `blockStamp` and the tick's time); once a beat the average,
  and the next beat runs at their tempo nudged by half of it (at most ±0.5 %,
  ~9 cents;
  nothing under 0.003 beats). For 4 beats after their Start or a tempo change
  (while the loops change pitch anyway) up to ±5 %, the beat caught up within
  about a beat, not over several each off pitch. A tick way off its predicted
  time (half a tick: their first after a Start often is) is taken as it is,
  the tempo left; three in a row: their tempo's another, measured afresh.
* Measured on a DAW's clock over USB (`tools/looper_sim/data/daw_clock_usb.txt`,
  captured with `LOG_TICKS`): ticks up to ±2 ms off, its first after a Start
  early; replayed by the simulator: the tempo read 72.0 / 83.0, the speed
  within 0.65 % of theirs while steady, up to ~4 % for a beat or two starting
  and around their change from 72 to 83, on their beat within 0.003 beats.
* Starting where they are, not catching up: our transport starts once their
  clock reaches the beat it starts on, and the tracks fill first; then
  `transportStartAt(beat, inBeat)` asks how far past that beat they are by now
  (from that tick's time, at their tempo) and starts that far into the beat
  (`transport.joinMidBeat`: the engine's next block joins the armed tracks at
  that point of their streams, gliding in, and the MIDI clock out from its
  next tick). The simulator: played again after a pause, the speed at most
  0.19 % off theirs and our position within 0.004 beats of theirs from the
  first beat. A tempo change under 0.1 % isn't made (each one
  re-pitches the loops a little and is a recording's tempo mark). The
  simulator holds a clock jittering by ±0.6 ms within 0.003 beats, with 2
  small changes in 16 beats.
* Their transport's messages are queued in order with the tick each comes
  before (a Stop, Song Position and Continue in one go keep their order).
  Start (0xFA): the transport from 1.1.1 on their next tick. Continue (0xFB):
  from their latest Song Position (0xF2, two data bytes, in 16ths; serial:
  put together from the bytes, realtime ones may come between; USB: as
  `usbMIDI` has it; none: 1.1.1), starting on their next whole beat
  (`transportStartAt(beat)`: the clock reads that beat, each track where it'd
  be then from its 1.1.1 alignment). A Song Position while they run: a jump
  there, the same way (stopped, then started on their next whole beat).
  Running already: stopped first. Stop (0xFC): the transport stops. Their
  position known, our beat is kept on theirs bars and all (the error's taken
  modulo a bar); started with START instead, only the beats are kept in step.
  The simulator: Continue from 8.5 beats starts on beat 9, then within 0.0003
  beats; a jump to beat 16 while running, within 0.0011.
* Logged on the serial port: "MIDI in: start" / "stop".

**USB** (`midi_usb.*`, `MIDI_USB_ENABLED`; the USB type "Serial + MIDI + Mass
Storage", tools/teensy_msc/patch.py: a MIDI interface as Teensyduino's Serial +
MIDI, endpoint 4, mass storage moved to endpoint 5 and interface 3): out, the
same clock, Start and Stop as the serial out (a second reader of the engine's
queues, `midiOutTakeUsb()`); in, clock / Start / Continue / Stop into the same
follower (`midiInByte(..., MIDI_USB)`). Its own 250 µs timer at priority 224,
under the audio interrupt's 208: the core's USB MIDI send waits (up to 40 ms,
once) while a computer isn't reading, which may delay MIDI and `loop()`, never
the audio. Two clocks at once (serial and USB): the one that ticked first is
followed until it stops; the other's ticks, starts and stops are ignored
meanwhile. A pause (no tick for 0.5 s) starts the tempo's measurement afresh.
In storage mode nothing is taken in (`midiInSuspend()`).

## USB audio

The patched USB type ("Serial + MIDI + Audio + Mass Storage",
tools/teensy_msc/patch.py) adds Teensyduino's USB audio (as its Serial + MIDI +
Audio type: stereo, 16-bit, 44.1 kHz both ways; interfaces 4-6, endpoints 6 and
7, after mass storage's 3 and 5). The M8 shows up on a computer as an audio
device, "L7", 2 in and 2 out (`AudioInputUSB` / `AudioOutputUSB`,
m8_looper.ino, only with that type).
* Out: the engine's output, as the line out and headphones get it (after the
  master volume).
* In: the engine's inputs 2-3, mixed into the input with the line input (what's
  monitored and recorded, `inputRing`; clipped to 16 bits). While USB audio
  comes in, the line input is left out once it has been quiet (block peaks
  under `LINE_GATE_LEVEL`, ~-56 dBFS) for 2 s, as with nothing plugged in (the
  M8 can't tell: no jack detection), and in again as soon as it isn't, each a
  fade over a block; with no USB audio, the line input as ever. The serial log
  shows the line input's quietest and loudest block every 5 s while USB audio
  comes in ("USB audio in; line input: ..."), to set the level from the M8's
  own noise floor. The simulator checks the mix and the gate.

