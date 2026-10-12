# Testing checklist

Things built but not yet checked on the M8 (or in Live), and known open
problems. Look through it now and then: tick an item off (delete it) once it's
checked, or turn it into a fix.

## To check

- [ ] **Ableton export, the fine rotate's direction.** Export a project with a
  clip shifted (B then A + LEFT/RIGHT) and play it in Live next to the M8.
  The whole loop moves the clip's start marker; a range inside the loop moves
  every warp marker. Watch for: the beat landing where the M8 has it. Off by
  twice the offset = the sign the wrong way round (`ableton.cpp`, `r`).
  Also: a range inside the loop shifted past the file's start or end plays a
  silent or cut stretch in Live (no wrapping there).
- [ ] **Tap tempo while playing: the bar eased onto the taps** (`phaseAlign()`,
  6% faster / slower). Does it converge, and is 6% too slow or too audible?
- [ ] **Audio follow: taps place the bar** (`tapBar()`, `audioFollowShift()`,
  5% of a beat a beat). Two agreeing taps shift it; is it too slow?
- [ ] **Audio follow while recording / resampling** (no longer frozen): no
  drift, no tempo-mark trouble in the recording.
- [ ] **SELECT NEXT FREE TRACK**: on a recording's stop press; after a
  resampling, track 2 (or the next free one in the new scene).
- [ ] **Resampling onto track 1**, the muted clips on tracks 2, 3 ... in
  order; unmuting a moved one is heard.
- [ ] **Sample switch label** (SHIFT on a track) with real samples: name, X/N,
  the flash on a step.
- [ ] **Waveform heights** follow the track's volume (turn a track up and down).
- [ ] **Clock start with the range start a beat before the clip's end**: starts
  on 1.1.1's start position, not a bar late.
- [ ] **Recording start and stop grace (100 ms)** and the 16-bar limit below 60 BPM.

- [ ] **Resample pressed late** (within 0.1 s after a bar line): starts on
  that bar line, the mix copied in from the mix history; no click at its start
  (the 128-frame pre-roll from the mix too). No simulator test yet.
- [ ] **Recording on a slow card**: the input history is now ~560 ms (was
  ~740 ms), so a long card stall loses input sooner ("recording fell behind").
- [ ] **Volume, 20 steps**: saved projects keep their old step numbers (not
  converted), so a track that was set off unity now sits half as far from it
  (e.g. old "2 steps down", -7.5 dB, now plays -3.75 dB). Check older
  projects still sound right, or re-set their volumes.
- [ ] **Balance, ±20 steps**: saved balances not converted, so they now sit
  half as far off centre. Check older projects.
- [ ] **Volume cell while B is held**: meter in the accent colour, the level
  light grey; stopped or muted, the level all accent.
- [ ] **Volume display**: an odd step's last half-line dotted, the meter
  lighting it dotted too; nothing lit only when the track is off.
- [ ] **Resampled clip's loudness**: two steps up with the -3.75 dB headroom
  should match what was resampled exactly; compare by ear.
- [ ] **Ableton export, a range across the loop's end**: the doubled file
  (`A001x2.wav`), the clip's loop on the range inside it, warp markers through
  both halves; plays as on the M8. No simulator test yet.
- [ ] **Sample names `A001.wav`** (per project): new recordings named so;
  older `A30012.wav` ones still play; SHIFT+LEFT/RIGHT order.
- [ ] **Step and ms offsets apart**: taps move the range, start point,
  playhead and bar lines left over the waveform (not the waveform); holds turn
  the waveform; the status shows the ms part only. Export: steps move the
  clip's start and loop start back (wrapping; a range then across the end:
  doubled), the ms part as before. Older saved offsets load as ms.
- [ ] **Delete unused samples**: the progress bar under it, then the count.
- [ ] **Recording space on a card with scattered free space**: grown a
  chunk at a time while stopped (log: "grown in pieces"); the 24 come back;
  no freezes while it runs.

## Open problems

- [ ] **Snags on project load**: a capture of several loads (2026-10-12) heard
  nothing; the log had one tiny dropout on track 7 (112 + 256 samples, ~8 ms)
  during a scene write, then a stream read. Watch for it again.
- [ ] **A dropout while a recording is saved** (seen once: track 6, the loop
  drawing the screen).
- [ ] **Swipe**: a held arrow stops one track short of the start; undoing after
  a full lap skips a track.
- [ ] **Simulator: "12 switches at 120 BPM"** fails with dropouts since the
  200 ms grace change (state from earlier tests); look into or loosen.
- External tempo vs scene tempo: follow a MIDI clock (and audio follow) at a tempo other than the scene's; switch scenes and record while following, then stop the clock / turn audio follow off: the scene's own tempo comes back, and its saved tempo is unchanged. B+UP on the tempo cell while following, power cycle: still doubled; unplug the clock for 10 s (audio follow: turn it off): back to x1.
- On N+1* (and 0* holding clips), change the tempo by hand: it becomes scene N+1 (1) with that tempo.
- Scene switch between clips of different range lengths (e.g. 4 bars into 2, 1 into 4): the new clip plays as far past its start point as the old one was, wrapped to its length; same-length ranges unchanged; an empty track into a clip: from its start point.
- Change N+1* (it becomes scene N+1), then cut it right away: back on N+1* (a copy of N), playing on without a restart. Leave a made scene and come back, then cut: to the scene before.
- Filter level compensation: narrow the filter on a full mix loop (and on a hi-hat loop, a bass loop): the level stays about the same, a bass under a lowpass isn't boosted, a hi-hat under a lowpass is (up to +12 dB). Watch for clipping on big boosts, and jumps as the filter moves (the gain changes at once while the filter glides). Recordings from before: compensated once measured (the clock stopped a while). Export: the clip gain includes it.
- SHIFT+START while stopped: resumes from the beat it stopped in (clock shows it, loops in place, MIDI followers get Song Position + Continue; with audio follow locked, on their beat); playing: restarts from 1.1.1. Load another project, then SHIFT+START: from 1.1.1.
- Scene clipboard: cut on 0* (SHIFT, B, A): clipboard emptied. Then B, SHIFT: the button shows the empty checkered square (not greyed); A inserts an empty scene after the current one (while playing: tracks go quiet on the next beat).
- Scene preview: playing, hold SHIFT and step to another scene: its tracks are drawn at once (waveforms, ranges, filters, volumes; a clip whose file is still opening: shown as it was until it is), no playheads, the track numbers blink each half beat. Let go: until the bar line the same; A, B, SHIFT+LEFT/RIGHT do nothing, arrows still move a grey frame, SHIFT+UP/DOWN still steps scenes, START stops. Step back to the playing scene while held: its own looks again, playheads back. Same while a project load waits for its bar line.
