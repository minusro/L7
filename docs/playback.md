# Playback and clip switching

## Streams

Each track has a ring of `RING_FRAMES` stereo frames. `loop()` fills it
(`produce()`: one SD read of up to `CHUNK_FRAMES` = 16 KB) with the loop range's
frames in playback order; the interrupt plays it at `playSpb / spb`
(varispeed: tempo changes repitch; any ratio the 30–300 BPM range allows,
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
  the first quarter beat the stream hasn't buffered (`applyQueuedRange`). Its
  start moved (its length as it was): the playhead moves with it, as far into
  the new range as it was into the old (crossfaded); else the playhead carries
  on from the same spot if the new range holds it.
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

## Time stretching

Every track keeps its own pitch whatever the tempo (`Track::stretch`, on for
new tracks: `STRETCH_DEFAULT`, config.h; off: resampled, its pitch with the
tempo, as before). The playhead (`tail`, `frac`) moves exactly as when
resampling (sync, starts, switches, ranges, recording all as ever); only the
sound is made differently (engine.cpp `stretchSample()`):
* WSOLA: grains of 1024 frames (23 ms), a new one every 512 output samples,
  Hann-windowed (two overlapping always sum to 1), each read at speed 1. A new
  grain comes from around where the playhead says (centred: its middle where
  the playhead will be by then), within ±256 frames, where it best continues
  the grain before (`placeGrain()`): the normalised cross-correlation of its
  first half with where the grain before would go on, on one signal (left +
  right, or left − right where that's louder: a mono sum loses what's
  opposite), every 8th frame (energies from running sums), then to the frame
  (±5); the nearest lag within 95 % of the best (a periodic sound matches as
  well a period further). At the tempo recorded that's where the grain before
  goes on: the stream as it is. Each track's grains are due at another point
  of the hop (their searches spread over the blocks).
* The streamer keeps `STRETCH_HISTORY` (1024) frames behind the playhead in
  each ring (`STREAM_FRAMES` ahead at most; `streamRoom()`): a grain reads up
  to ~515 frames back at half speed, ~710 at an eighth.
* A track's grains start afresh when it starts playing (not when the playhead
  snaps to its beat each beat: they go on through it).
* Transients (`findTransients()`, loop(), as the streamer writes each chunk;
  from a recording's own frames when it starts playing): each window of 64
  frames' energy against the windows' average before it (exponential, 0.1 a
  window): 8 times that (~9 dB) and over ~-45 dBFS, 50 ms after the last at
  least, is one, marked 32 frames before its window. A queue per track (32),
  emptied as the playhead passes, cleared when its stream starts afresh,
  trimmed when a clip switch drops what's buffered.
* Transient locking (`transientLock`): grains fade out over the 32 frames
  before the next transient and play nothing past it (no attack early, or
  twice); when the playhead reaches it, the grains start afresh there (one at
  full level from it, its partner rising: the attack as recorded, on time),
  the ones before fading out over 64 samples.
* The simulator: a 440 Hz loop recorded at 120 BPM stays 440 Hz at 96 and 150,
  its level within 2 %, no clicks; drums on our beats; clicks recorded at 120
  played at 96, 80 and 150: locked, each as recorded (correlation 0.995, as
  at the tempo recorded) and on time (within 0.8 ms of where resampling puts
  it); not locked, 0.97-0.99 and up to 4 ms off; a fuzz session all stretched.
* On the M8 (`LOG_ENGINE_LOAD`: the engine's CPU use every 5 s): 7 stretched
  tracks ~24 % on average, ~31 % at most (the first version, with a search
  30 times dearer, ~44 %, single blocks over 100 %: the card timing out).
* The Live Set export warps the clips in Complex mode (`WarpMode 4`), the
  same: their own pitch at Live's tempo.

## Fine rotate

A clip can be turned later or earlier against the clock without touching
its audio (B + A + arrows on its waveform cell, controls-and-ui.md): its
unit keeps `rotate`, the frames it's turned later (0 up to its loop's
length; in the upper 24 bits of the old 32-bit mute, so older units read 0).
`Track::rangeStartFrame()` takes it off (every place played is that much
earlier in the file), so the range's start, its wraps, the planned switches
and the stream all follow; the waveform's overview is drawn turned as far
right (ui.cpp `takeLooks()`). A change while playing (`rotateTrack()`) is queued as a moved
range is (`queueRange()`, with the range: one doesn't undo the other): the
switch goes on the first quarter beat the stream hadn't buffered yet, the old
place playing on from the buffer up to it and the new one read after it, so
it's heard only once its data is in, a whole buffer (~0.35 s) ahead;
crossfaded, the clip in step with the clock. (A scene's units differing in it
switch as clips do: `sameClip()` counts it.) The simulator checks the output
at each beat turned a quarter beat, a millisecond more, and back.

## Metronome

A setting (the session's `metro=`; B+A on the tempo cell): off or on (an
older session's auto, 1: off). Off, a project with no clips (none on its
tracks or in any of its scenes, `projectHasClips()`) still clicks while the
EMPTY PROJECT CLICK setting is on (the default; the menu's settings). While the clock runs and it says so (never in the dummy project, nor while the menu is open: the clock not shown), the engine clicks on every beat: a
small wood block (two damped sines, ~1.15 kHz with a harder partial; 1.6 kHz
and louder on the bar's first beat). Its level follows the mix: its peak
before the master volume (the tracks and the monitored input, the click left
out), followed
rising at once and falling 6 dB a second (`REF_FALL_DB`), the bar's first beat 2 dB over that
peak (`CLICK_OVER_DB`; at most full scale) and the others at 3/4 of it, never under -16 dBFS;
then the master volume on it as on the mix, at once (all the way down: silent) (`clickLevel()`, engine.cpp). It's added after the master volume and
left out of what a resampling records. Stopped, it clicks on each of tap tempo's first four taps too (the first as a bar's first beat), the 5th starting the clock on its own click (`metronomeClick()`). A recording started with it clicking keeps it
until it's completely recorded: on auto it stops on the bar line where that
recording ends and the clip starts playing (it goes on while the recording
finishes its bar; that bar line itself doesn't click). A scene switch waiting
for its switch point leaves the click as it is (clicking on until it). While a
project switch waits, nothing clicks, whatever the setting: from the press
until the switch (`clickFromBeat` = `sceneSwitchBeat()` while
`menuLoadPending()`, set by `loop()`; the switch's own beat clicks if the new
project should).

## MIDI clock

While an external MIDI or USB clock is followed, the tempo cell's label is grey
(light grey selected) instead of the usual light grey (white selected).

**Out** (`engine.cpp`, `MIDI_OUT_ENABLED`): 24 ticks a beat from the audio
interrupt (each queued with the time it's due and sent by a 200 µs timer:
<0.2 ms of jitter), Stop on `transportStop()`, and a Song Position wherever
L7's position starts or jumps, so whatever follows it stays on its bar:
started, Song Position 0 then Start (started at another beat, following an
external clock's song position: Song Position then Continue); a project
switching while playing (the clock back to 1.1.1 at its bar line):
Stop, Song Position 0, Continue on that beat before its first tick (the
engine, `transport.midiRestartBeat`); tap tempo's snap to a bar: Stop, Song
Position, Continue. Sent from `loop()` and the audio interrupt both (the queue
guarded); over USB MIDI too (midi_usb.cpp puts a Song Position's three bytes
together). Checked on the M8 with tools/latency/midimon.c.
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
* Their tempo remembered: their ticks timed over the stretch we've followed
  them steadily (from the end of the catch-up after their start or a tempo
  change to the latest tick, 48 ticks at least: their jitter only at its two
  ends; a tempo change: afresh). On their Stop, or their clock gone, our tempo
  becomes it (exactly: no beat nudge left in it; the tempo shown too: START
  here then plays at it), and their next Start (or Continue) begins from it,
  trackers included, unless their clock meanwhile is clearly at another
  (0.6 % off: theirs, at once). The simulator: after their Stop, ours within
  0.013 % of theirs; started again, within 0.01 % through the first beat; a
  clock now at 130 instead of 120, taken at once.
* Played ahead (`AHEAD_US`, per source: serial 0, USB 2.0 ms): our beat kept
  that much ahead of theirs, their clock's way in and our audio's way out, so
  L7's audio reaches the computer on their beat as a DAW's recording places
  it. Measured with tools/latency/clocksync.c (a scheduled 120 BPM clock to
  "L7", its metronome recorded over USB audio with `USB_OUT_CLICK`, placed
  less CoreAudio's input latency): 1.7-2.1 ms late without it; with it the
  clicks within about ±1 ms of the beat (100-140 BPM), the beat correction's
  dead zone.
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

## Audio follow

`audio_follow.cpp` (its header has the outline). SHIFT+A on the tempo cell turns it
on / off (the session's `follow=`); the button, SHIFT held there: a jack plug
coming in from the right under an arrow pointing in, crossed
out while it's on. On, the tempo can't be changed by hand or by a project
(`changeTempo()`, tap tempo: nothing); B+UP/DOWN on the tempo cell only halve
(DOWN) or double (UP) it (B+LEFT/RIGHT: the offset from the external clock), fresh presses only, again and again
within the clock's range (`audioFollowScale()`: the clock at their tempo times
2^n, kept in the session (`octaves=`, with the MIDI clocks'), back to 1 when turned off; halved, their beat lands on every other half of
ours and every other one on ours; the estimate leans towards the tempo shown
divided back). Nothing found yet: the tempo shown is halved or doubled
instead (the clock too), and the estimate, leaning towards it, finds that
octave. Following any external tempo (a MIDI clock, serial or USB, or audio follow) the
scene's saved tempo isn't touched (`projectTempoChanged()`, `sceneRecorded()`: nothing; a scene
switch doesn't set its tempo); back on the internal clock (the clock stopped, audio follow off),
the current scene's saved tempo comes back (`loop()`, `sceneSavedTempo()`). A MIDI clock's
multiplier (B+UP/DOWN) is kept in the session too, back to 1 after 10 s without its clock (unplugged).
An external MIDI clock being followed
comes first (audio follow listens on, but doesn't touch the clock meanwhile).
* What's listened to: the input as recorded (monitored or not) (the line input and
  USB audio mixed, before resampling's swap). The interrupt (only while on),
  each block: the mono input through three bands (two one-poles under
  ~150 Hz; above ~2.5 kHz; all), each band's energy smoothed over ~10 ms, its
  log at the block's end; the rises (half-wave: only louder) summed, into
  `onsetRing` (2048 blocks, ~5.9 s, 4 KB, RAM2).
* The tempo, every 0.5 s once ~5.2 s are in: the autocorrelation of the onsets
  (frames of two blocks, mean removed, peaks widened a frame either side),
  every lag of 60-200 BPM tried in quarter frames, each scored by the mean
  of its lag and three multiples (a pattern repeats every beat, two, three,
  four; a whole-frame lag's multiples would miss narrow peaks: 174 BPM was
  taken for 116), weighted towards the tempo shown (a log-normal, 0.6 of an
  octave): a beat's half or double is a matter of taste, so the tempo set
  nearer decides (drums at 174 with 150 set: 174; with 100 set: 87). Not
  clear enough (silence): none.
* The beat: from a tempo, the phase whose beats rose most over the last few
  seconds; then each beat looked for within a quarter period of where it's
  due (weighted by closeness), found if its rise stands out (2.5 times the
  last 2 s's average); found, the beat moves by alpha of its miss and the
  period by beta (0.2 / 0.03; 0.35 / 0.06 for the first 8 beats and for any
  miss of 3 frames or more: a tempo changing). Found often enough: locked
  (the serial log: "audio follow: locked (126.00 BPM)"); a new tempo estimate
  4 % off (twice in a row unlocked, 6 times locked): afresh. Every 0.5 s,
  locked, the strongest phase at its period checked: 0.2 of a beat off three
  times in a row (an off-beat taken), moved there.
* The clock: stopped, the tempo theirs as soon as one is found, then each
  beat as the tracker refines it, locked or not (`followTempo()`, as the MIDI
  clock's; shown to a tenth); running, once locked, each beat also nudged: their beat as
  heard (its rise found, else as expected), less the input's latency
  (`inputLatency()`: the line input's or USB's, as recordings), is where our
  beat should be; our position then from `beatLog` (the engine: each beat's
  first sample, a start part way into a beat included), and the next beat at
  their tempo nudged by half the difference (at most 0.5 %; nothing under
  0.006 beats; for 8 beats after a lock or a start, or 0.025 beats off or
  more, all of it, up to 5 %). Beats only: the bars stay where START put
  them.
* START (or A on the clock cell, SHIFT+START) while locked: on their beat
  (`audioFollowStart()`): in the first half of theirs, at once, that far into
  the beat (`transportStartAt(0, inBeat)`); in the second half, on their next.
* Silence (the input, as analysed, under -50 dBFS for 1.5 s: `onsetLevel`,
  the interrupt's energy over ~10 ms; sparse music's gaps are shorter):
  nothing changes. No tempo estimated, the clock left as it is (stopped or
  running), their beat counted as lost (the tempo flashes black, START not
  on it; the serial log: "audio follow: silence (the tempo kept)"). Sound again:
  after 3 beats of it, their beat found afresh at the tempo kept (the
  strongest phase since; the tempo still kept meanwhile), then tracked as ever.
* Recording or resampling: followed as ever, the tempo changed and the clock
  nudged onto their beat (`follow()`; a recording keeps the tempo changes, its
  bar lines where they fell). Like MIDI clock, this lines up beats, not bars:
  audio has no bar line to follow. Each tap on the tempo cell also counts as one of
  their beats heard (`audioFollowTapHint()`: a hit in that beat's window, averaged
  with the detector's own if both), helping it find and keep the beat. Tapping A on the tempo cell meanwhile places
  it (`tapBar()`): the taps don't change the tempo; the run's first tap is beat
  one, and once two taps agree the clock's bar is whole beats off, it's eased
  there on top of the beat's nudge (`audioFollowShift()`, SHIFT_NUDGE: 5% of a
  beat a beat).
* The tempo cell's number flashes with the beat (its first half: black until
  their beat is locked, then the accent colour), whenever audio follow is on: the clock's while it runs; stopped,
  theirs as the tracker has it (locked or not yet), or, nothing found yet, at
  the tempo shown.
* The simulator (drums: a kick every beat, a snare on 2 and 4, hats on the
  eighths, a pad): 126 BPM found within 10 s (126.0 shown); START on their
  beat from the first (within 0.3 ms); running, within 0.2 ms of their kicks;
  their tempo rising to 132 over 8 s, followed (within ~16 ms meanwhile, a few
  ms after); silence (12 s of the line input's noise floor, stopped): the
  tempo kept exactly, the beat lost after 1.5 s, found again 3.3 s after the
  drums are back (0.4 of a beat later), the tempo steady meanwhile; 90 / 70 / 174 BPM found
  from the tempo set; the drums over USB audio alone (the line input silent):
  found, and running on their beat less USB's input latency (within 4 ms).
  Real music, on the M8: to try.

## USB audio

The patched USB type ("Serial + MIDI + Audio + Mass Storage",
tools/teensy_msc/patch.py) has L7's own USB audio, USB Audio Class 2.0
(usb_audio_l7.cpp, copied into the core; Teensyduino's UAC1 audio left out with
it). The M8 shows up on a computer as an audio device, "L7", with 16 channels
out and 2 in: 16-bit, 44.1 kHz, both streams asynchronous on L7's own clock.
macOS and Windows 10 (1703) and later have the class driver built in.
* Interfaces: CDC 0-1, mass storage 2, audio 3-5 (control, to the computer,
  from it), MIDI 6 (numbered after the audio function's, as UAC2 hosts want;
  its descriptor still comes first). Endpoints: audio 6 (both ways), the
  feedback endpoint 7. One clock source (ID 9, fixed 44.1 kHz), no controls:
  no volume on the computer's side.
* Channels to the computer (named in the descriptors, "Master L" ... "Ch 7 R",
  string descriptors 0x10-0x1F): 1-2 the master, then the tracks from the top
  row down: 3-4 Ch 1 (the top row, track index 6) to 15-16 Ch 7 (the bottom
  row, index 0; `usbTrackChannel()`, engine.h). At full speed (through a USB
  1.1 hub) only the master (2 channels).
* Packets every microframe (125 us) both ways, 5-6 frames each (7 at most); the
  feedback (frames a microframe, 16.16) every 1 ms. With data every microframe,
  "frames a microframe" and "frames a packet" agree, whichever a host takes.
* Rings: to the computer, 2 blocks of 16-channel frames (8 KB, RAM2): the
  engine writes a whole block straight into it (`usbAudioWrite`, engine.h);
  the USB interrupt sends from it, as many frames a packet as L7's rate makes,
  trimmed by the ring's fill after each block written against its mark (a
  block + 40 frames; a servo: 0.5 frames a second per frame off, at most
  200 Hz either way). From the computer, 4 blocks of stereo frames (2 KB): the
  engine reads a block (`usbAudioRead`) once the ring has a block + 40 frames;
  the rate asked for in the feedback is trimmed by the fill before each read
  the same way. A ring that runs dry: silence (to the computer) / no USB input
  (from it) until it's at its mark again. Every 5 s while streaming, the
  serial log shows the rings ("USB audio: out on (fill ..., ... Hz, ... short,
  ... dropped); in ...") when anything ran short since.
* Windows remembers a device's descriptors: a PC that saw an older L7 (with
  Teensyduino's stereo audio) may need it removed once in Device Manager
  (View > Show hidden devices, the "L7" entries: Uninstall device) before the
  16 channels show up.
* The master limiter (engine.cpp `Limiter`, config.h `LIMIT_*`): the mix is
  float all the way (tracks, filters, their make-up, the sum: no clipping
  inside); one limiter, on the tracks summed (and the monitored input: what a
  resampling records, `mixRing`), before the master volume: each peak over
  its ceiling turned down at once (no look-ahead), the gain back up over
  `LIMIT_RELEASE_MS`; the ceiling `LIMIT_CEILING` (~-0.2 dBFS), lowered by the
  master volume's gain over 100 % (its most on either side, balance counted),
  so the output stays under full scale too (a resampling with the master over
  100 % comes out that much lower). The click is added after it (it would duck the mix on
  every beat). Only the per-track USB channels (3-16) can still clip, a track
  alone over full scale. Buffers stay 16-bit (the rings hold 16-bit
  recordings; the codec and USB are 16-bit at full scale, so wider buffers
  wouldn't add headroom where it's needed, and RAM2 has no room for them).
* Out, channels 1-2: the mix as the line out has it (the tracks and the
  monitored input, while MONITOR AUDIO IN is on: `inputMonitored`; the master volume in it) without the metronome; channels
  3-16: each track as heard in the mix before the master volume (its volume,
  balance, filters and mute, the mix's headroom `masterGain`; at the master
  volume's 100 % the tracks add up to channels 1-2, without the monitored
  input; a track not playing: silent); the line out and headphones have the
  metronome too. `USB_OUT_CLICK` (engine.cpp) puts the metronome on
  USB too, for measuring (tools/latency/clocksync.c). The speaker stays on
  while a computer records it (only headphones turn it off).
* In: mixed into the input with the line input (what's
  monitored and recorded, `inputRing`; clipped to 16 bits). While USB audio
  comes in, the line input is left out once it has been quiet (block peaks
  under `LINE_GATE_LEVEL`, ~-56 dBFS) for 2 s, as with nothing plugged in (the
  M8 can't tell: no jack detection), and in again as soon as it isn't, each a
  fade over a block; with no USB audio, the line input as ever. The serial log
  shows the line input's quietest and loudest block every 5 s while USB audio
  comes in ("USB audio in; line input: ..."), to set the level from the M8's
  own noise floor. The simulator checks the mix and the gate.
* Recording what comes in over USB: on the beat. What's on a computer's beat
  arrives late (its output buffer, e.g. ~23 ms at 1024 samples, its sync
  delay, the USB buffers: a DAW's metronome recorded into L7 came back 20 ms
  late, docs/design/ableton/L7 Latency Test 1 Project), and by how much
  depends on the computer's settings, so it's measured: while USB audio comes
  in and the clock runs, the first sharp sound after each of our beat lines
  (well above the input's average level: a metronome, a kick; within the
  first third of the beat: not offbeat content) is timed in its beat by the
  interrupt, and `serviceUsbLatency()` (loop()) takes the middle of the last 8
  beats in a row when they agree within ~2 ms as USB's input latency (the
  serial log: "USB input latency: ... samples"; before any, the line input's
  plus a block). A recording takes the input's latency when it's pressed
  (`recording.latency`: USB's while USB audio comes in, else the line
  input's `LATENCY_FRAMES`) and keeps it to its end (a queued one that carries
  on from it: the same). The line input mixed in meanwhile lands late by the
  difference (a delay line to match them: too much RAM). The simulator: a
  click 900 samples after each beat, measured 900, a loop of it with the
  click on frame 0.
* Latency (tools/latency), measured with Teensyduino's UAC1 audio (to measure
  again with UAC2's rings): a round trip through the M8 over USB 10.9 ms, of
  which CoreAudio reported 5.2 ms; the M8's own ~5.7 ms wasn't reported, and
  couldn't be: macOS ignored the streams' bDelay (UAC2 has no such field). A
  DAW's offset for the device takes the rest. `AHEAD_US` (midi_in.cpp, 2 ms
  over USB) was measured then too.

