# Testing checklist

Things built but not yet checked on the M8 (or in Live), and known open
problems. Look through it now and then: tick an item off (delete it) once it's
checked, or turn it into a fix.

## To check

- [ ] **N9: cursors on every screen** (arrows, held = repeating): FX cells and
  send bars, mixer strips, mod sources + cells (the cells' header follows the
  source row), INST selector + cells, grid cursor (its track shows in the
  INST/MOD/EDIT titles), piano roll (a note under the cursor shows selected,
  an empty cell shows a white frame). Clamped at all edges, no wrap.
- [ ] **N9: first sound.** MIDI channel 1 (USB and serial in) plays the synth;
  B + LEFT/RIGHT on INST steps the 13 engines (takes effect for new notes).
  SAW, PULSE, SUPER, FM2, PLUCK and NOISE are real (simple) voices; FM4, WT
  and the emulations JUNO, PRO5, MWAVE, M1 are placeholder sounds.
- [ ] **N9: the two FX-slot screens and the bar strip.** The stack is eight
  now: MFX (two selectable master FX slots, topmost), sends (the old FX page,
  retagged SENDS), mixer, mod, TFX (the track's two slots, between mod and
  INST), instrument, grid, editor — eight dots in the status row. MFX/TFX:
  UP/DOWN over selector/cells rows, LEFT/RIGHT on the cell rows; mock types
  (COMP/TAPE, CHORUS/DRIVE). The editor's new bar strip: 16 slots, 4-bar mock
  clip, the accent loop brace wrapping bars 3-4-1, a white frame on the shown
  bar.
- [ ] **Charge guard (L7 + N9, M8Board::chargeGuard).** Every 5 s on variant
  B: pauses charging (CHG_CONFIG off) on a charger fault, thermal regulation,
  battery over 4.25 V, or measured charge current over 900 mA; retries after
  5 min, three strikes latch off until the next boot; serial logs each
  transition. N9 is on the device; L7 is build-verified only (flashing it
  would replace N9) - upload L7 whenever the device goes back to looping.
  Hard to provoke safely: at least confirm no spurious pauses while charging
  normally (no "charge guard:" lines on serial).
- [ ] **N9: the 12 effects.** TFX (track bus) and MFX (master) slots are
  live: the selector steps -, CHORUS, FLANGER, PHASER, TREM, DELAY, PINGPONG,
  REVERB, DRIVE, CRUSH, FILTER, COMP, WOW; a type change loads its defaults;
  the four cells edit with B+arrows. All parameters eased (~20 ms) against
  zipper noise - DELAY/PINGPONG time changes bend pitch tape-style on
  purpose. Listen per effect; watch CPU with reverb + delay + chorus + comp
  at once. Boot showed 6 voices active twice - if that wasn't someone
  playing, the serial MIDI in may be reading noise.
- [ ] **N9: the trimmed lineup and live cells.** 27 engines: PROPHET, 303,
  JUNO, then the 24 Mutable models (engine selector a third of the screen,
  M1/Microwave gone from it). INST cells edit for real (B+arrows, LEFT/RIGHT
  1, UP/DOWN 10, repeats): PROPHET detune/cutoff/res/env/ADSR; 303 incl.
  WAVE (square at 50+) and SLIDE time; JUNO's new DCO voice (saw + detune +
  sub); Mutable harmonics/timbre/morph/decay/LPG/level. Values apply to NEW
  notes (not live on held ones yet). Listen: plaits models at 4 voices (CPU
  log), speech model on the SPEECH engine, drum models trigger right.
- [ ] **N9: the Microwave.** Engine 12 plays the user's factory bank (256
  sounds, sounds/Microwave soundbank): preset cell shows "NNN NAME", B +
  LEFT/RIGHT or MIDI program change steps it. Parameter offsets were
  INFERRED from the data, not a spec (tools/mw_bank.py): VCA ADSR, resonant
  VCF + env sweep, wavetable stand-in = sine->saw->square morph swept from
  the start wave. Check: the three "303" sounds squelch, pads swell and
  sustain, "Noisefloor"-type sounds at least behave; if envelopes feel
  systematically wrong the offsets or the time scale (mwcoef) are off.
- [ ] **N9: the M1.** Engine 13 plays the factory bank's parameters (the
  user's sysex dump, sounds/M1 soundbank): MIDI program change on channel 1
  picks among the 100 programs (serial logs the name; boots on 00 Universe).
  The INST screen's preset cell (left of the engine selector, LEFT/RIGHT on
  the top row between the two) shows "NN NAME" on the M1, a dash elsewhere;
  B + LEFT/RIGHT on it steps the program, wrapping at 99.
  Real per program: osc pair (octave/interval/detune), the VDA EG segments,
  VDF cutoff + env sweep, osc level and velocity sens. Stand-ins: the PCM
  multisounds are approximated by category (saw/pulse/super/sine/organ/FM
  bell/EP/pluck/noise — tools/m1_bank.py). Checked finite and audible on the
  PC for all 100; judge by ear per family (pads, organs, basses, bells).
  Drum programs (09, 29, 49, 95) play as single-osc stand-ins, far from real.
- [ ] **N9: the Prophet-5.** Poly voice: two detuned polyBLEP saws into the
  ladder (mild resonance 1.3), filter easing to a sustain level over ~600 ms,
  velocity opens it. Numbers by ear: detune (0.4%), cutoff floor, env sweep,
  release 350 ms. Never heard yet.
- [ ] **N9: the 303.** Mono with slide (overlapping notes glide, ~35 ms, no
  retrigger) and accent (velocity >= 100: louder, more filter env, faster
  "wow" decay). PolyBLEP saw into a resonant 4-pole ladder (feedback 3.3,
  cubic squash), filter env 300 ms / 130 ms accented, cutoff ~265 Hz-5 kHz.
  The numbers are first guesses — judge by ear: squelch, slide feel, accent
  pop, resonance short of self-oscillation, level against the other engines
  (makeup x2.4). Voice math checked stable on the PC, never heard yet. 32 voices, stealing beyond that. Headphones mute the speaker. Serial
  logs `synth: N voices, max block ...` once a second while playing — note the
  % with ~8 and ~32 voices (the engine-budget question). Redraws are DMA'd:
  listen for clicks or stuck keys while switching screens with notes held.
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
- Filter level compensation: narrow the filter on a full mix loop (and on a hi-hat loop, a bass loop): the level stays about the same, a bass under a lowpass isn't boosted, a hi-hat under a lowpass is (up to +12 dB). Half strength (at most +6 dB). The master limiter (one, on the summed tracks, its ceiling lowered by a master volume over 100 %): push several loud tracks (and the master volume up to 400 %): no clipping on the line out or in a resampling; listen for pumping; the click not ducking the mix. Also jumps as the filter moves (the gain changes at once while the filter glides). Recordings from before: compensated once measured (the clock stopped a while). Export: the clip gain includes it.
- SHIFT+START while stopped: resumes from the beat it stopped in (clock shows it, loops in place, MIDI followers get Song Position + Continue; with audio follow locked, on their beat); playing: restarts from 1.1.1. Load another project, then SHIFT+START: from 1.1.1.
- Scene clipboard: cut on 0* (SHIFT, B, A): clipboard emptied. Then B, SHIFT: the button shows the empty checkered square (not greyed); A inserts an empty scene after the current one (while playing: tracks go quiet on the next beat).
- Scene preview: playing, hold SHIFT and step to another scene: its tracks are drawn at once (waveforms, ranges, filters, volumes; a clip whose file is still opening: shown as it was until it is), no playheads, the track numbers blink each half beat. Let go: until the bar line the same; A, B, SHIFT+LEFT/RIGHT do nothing, arrows still move the selection (unseen), no meters, no selected row's ground, no selection frame while held, a grey one once let go (waiting), SHIFT+UP/DOWN still steps scenes, START stops. Step back to the playing scene while held: its own looks again, playheads back. Same while a project load waits for its bar line.
- SHIFT held: SHIFT+UP to N+1* (or triple tap to 0* / N+1*): only the label moves until SHIFT is let go, then it switches on the next beat.
- Master meter (B held on the clock cell): drive the mix into the limiter: grey lines (the unlimited level, through the master volume) past the white ones (the limited output), falling back as it lets go; turning the master volume down moves both.
- Scene changes now switch on the next beat (≥150 ms ahead), not the bar line; project loads while playing still on the bar line. Listen for clean switches mid-bar (loops keep their distance from their start points, the clock runs on).
- Master volume: 22 lines, 100 % at line 16 (about three quarters lit), even ~2.0 dB steps (the meter's) up to 400 %; a session or project saved on the scales before comes back at the same level. Tempo up to 300 BPM (tap, B+UP, MIDI clock, audio follow doubling): playback, recording, MIDI out, the tempo label (300.0).
- Master volume per project: set it in one project, load another (its own; a new one at 100 %), back again, power cycle. Projects from before take the volume playing when first loaded.
- Defrag samples (settings, after a delete's message goes): on a well-used card: the bar by bytes, then DEFRAGGED N (N, M NO ROOM); recordings play as before, the tracks on them too; pull the power during one: the recording back at the next start, no .NEW/.OLD left in SAMPLES. On exFAT, files a computer copied over are all rewritten the first time. Takes a while for many large files.
- Card hot swap (stopped): take the card out: INSERT AN SD CARD; put another in: its projects and samples load (a blank card: a fresh /L7). Swap during storage mode: the drive goes away, the new one mounts. Enter storage mode after a swap: the new card shows (the bug: connected, no drive; then a lock-up). Start with no card: the screen waits, then starts.
- Editing while playing at a high tempo (held arrows on filter / range, several tracks): no stutter (edits' writes now one every 2 s while playing; the serial log shows no 'slow SD read'). Stop: everything written at once.
- Defrag with little room: it drops the recording space first (the log says so), then copies; the recording space comes back while stopped (watch the log: 'recording space: making ...'), recording works right after.
- Switch off in the looper: it starts in the looper (the selection as it was); off in the menu: the menu. Key bus trouble: the serial log says 'keys: the key bus failed', the app keeps running (reads backed off 50 ms), no long freezes in 'keys (I2C)'.
- High tempo, many stretched tracks, editing cells: no 'slow SD read … card error 0x0B/0x0C data 0xB' on the serial log any more (the SDIO patch), no 'slow pass' lines over ~60 ms, no stutter.
- High tempo (more than twice the clips' own), a long resampling (10+ bars): the new scene switches in by itself (no SHIFT+START needed); if a track runs dry it comes back in step.
- Stop, start and press record at once: the recording starts on the start's bar line, its length right.
