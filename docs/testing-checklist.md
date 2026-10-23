# Testing checklist

Things built but not yet checked on the M8 (or in Live), and known open
problems. Look through it now and then: tick an item off (delete it) once it's
checked, or turn it into a fix.

## To check

- [ ] **K6: the SONG arranger (2026-10-18).** Make a few clips, then on
  SONG: A adds a row after the cursor (duplicating it), A+U/D sets the
  scene, A+L/R the repeats - the label counts BARS in scene-span steps (a
  4-bar scene reads X4 X8 X12...). PLAY on SONG starts the arrangement
  from row 1; B-tap jumps the transport to the cursor's row (starts the
  clock if stopped); rows advance seamlessly on bar lines (queued a bar
  ahead) and the song LOOPS at the end. The playing row highlights with a
  progress bar; the list scrolls past 8 rows (position top right). B+U/D
  reorders rows, SHIFT+A cuts one, SHIFT+B inserts the cut row after the
  cursor. Any manual launch on GRID (cell, scene column, A+B clip step)
  leaves song mode and the grid plays on. Editing the SOUNDING row's
  scene/repeats takes effect live (bars-left clamps down). Projects save
  the song (format K6P2; today's earlier K6P1 files still load, with an
  empty song). Check polymeter: a scene whose clips are 1/2/4 bars spans
  4 bars per repeat and clips keep their phase across row changes.
- [ ] **K6: INST editing + the full GRID/NOTE grammar (2026-10-18).** A is
  EDIT, B is OPTION. INST: the split selector is live (A+L/R engine, A+U/D
  jumps six; on the preset cell A+L/R walks the banked GM list 1..64, A+U/D
  jumps eight, DRUMS steps the kit), cells edit with A+L/R +-1 / A+U/D
  +-10, B+L/R turns the SYN/MOD/FX page, B double-tap resets a cell (FX
  TYPE included). NOTE: the roll folds to the scale (drum tracks one row
  per sound, named rows), velocity shows as fill brightness; A tap makes/
  selects a note (clean release deletes, the next clean tap restores);
  A+L/R length, A+U/D velocity (17 steps); B+L/R moves (wrapping the
  clip), B+U/D transposes in scale; B on empty drags a dotted marquee,
  released it selects (wrapped tails too); edits audition while held;
  the cursor skips over note extents and walks all bars. GRID: launches
  are INSTANT now (clips ride the clock, phase kept); A+U/D steps the bar
  ladder (Auto/1/2/4/8/16); B tap zooms into the clip's roll. Clipboards:
  SHIFT+A cut on full / paste on empty (clips, note selections, project
  cards; a cut playing clip resumes on paste), SHIFT+B copies. MIX: A+U/D
  volume, A+L/R pan, B tap SOLO (engine-side: soloed tracks silence the
  rest). PROJ: A on KEY/SCALE steps the scale, A+L/R key / A+U/D scale,
  METRONOME item toggles. Global: A+B+L/R track, A+B+U/D steps the playing
  clip, B+PLAY overdub (OD shows in the top bar). SONG is still mock.
- [ ] **K6 (the NEW app in K6/): the engine layer is in (2026-10-18).**
  On the device. The N9-era engine ported whole (32-voice pool synth, FX,
  Mutable engines, GM region, projects, storage mode, the L7 switch), with
  K6's own card layout: /K6/PROJECTS/A.K6P (format K6P1, no N9P reading)
  and /K6/GM.FLB - COPY apps_sd/GM.FLB THERE via storage mode, or GM plays
  synth patches. Check: SHIFT+arrows still walk the 3x2 map; on GRID, A on
  an empty cell creates a clip, on a filled one launches/stops it on the
  bar line (the scene column launches whole rows), launching also starts
  the clock; on NOTE (below GRID), A toggles a step note at the cursor and
  the roll shows the grid cursor's clip with a playhead; PLAY toggles the
  transport, SHIFT+PLAY resume/reset, PLAY held + arrows set tempo
  anywhere (U/D 1, L/R 0.1 BPM); on MIX, A mutes and the strips show real
  volumes/meters; on PROJ, A loads/creates projects, STORAGE MODE and
  SWITCH TO L7 work, KEY/SCALE steps the scale; the top bar's clock /
  tempo / play / key are live. INST is read-only (real engine name,
  params) and SONG is still mock; A/B held combos, B-tap zoom and
  clipboards are not in yet. All tracks default to PRO5 (no engine
  selection yet). Power-off saves the project. Getting BACK from L7 needs
  a USB reflash (L7's switcher still targets N9.HEX).
- [ ] **K6: the rename (6 tracks x 6 scenes), the split selector, and the
  boot-loaded GM region.** The app shows a K6 mark (splash + menu corner -
  judge the K and 6 glyphs by eye); the grid is 6x6 (cells 48x30). INST's
  top row has two cells now: the engine selector (40% wide) and, for GM and
  DRUMS, a preset box (4 blocks apart; L/R moves between them, the preset
  cell steps with B+L/R, B+U/D jumps a GM family). GM samples moved to a
  BOOT-loaded flash region: copy apps_sd/GM.FLB to the card as /N9/GM.FLB
  (GM.BNK is obsolete, delete it). First boot after copying (or after an
  app switch) shows "GM SAMPLES n%" for ~8-10 s; afterwards boots are
  instant and PRESET/PROJECT SWITCHES NEVER INTERRUPT AUDIO. Quality ladder
  landed at 0.5 s accurate starts / 0.2 s match-cut loops / 0.55 s
  one-shots - listen for loop quality vs before. Infra names (N9/ folder,
  /N9 card dir, N9.HEX) unchanged - a full rename is a separate migration.
- [ ] **N4: the whole reorganised UI (2026-10-16).** Three wrapping screens
  (GRID / NOTE / INST), 4 tracks x 4 scenes, minimalist. Check against the
  spec: GRID - A plays/stops instantly (phase kept on switch), B tap opens
  the editor, SHIFT+A clip cut/paste, B+U/D the bar ladder (Auto/1/2/4/8/16),
  B+PLAY overdub (MIDI lands in the selected track's playing clip),
  B+A+L/R track select, B+A+U/D clip stepping, cells show mini notes +
  playhead. NOTE - B-drag selection box (dotted), cursor skips note extents,
  A creates/selects, A+arrows length/velocity (0-16 steps), B+arrows
  move/transpose in scale, SHIFT+A note cut/paste, SHIFT+L/R bars. INST -
  selector above SYN/MOD/FX tabs, B+arrows +-1/+-10, B double-tap resets,
  edge wrap between pages; SYN row 2 = the new per-voice SVF (LP/HP/BP +
  env), MOD = ADSR + LFO (6 shapes, 15 destinations incl FX X/Y/wet),
  FX = type/X/Y/wet per slot. Status bar (GRID top, INST bottom): TAP tempo,
  tempo, metronome, OD, play triangle, key/scale. PLAY stop/start-1.1.1,
  SHIFT+PLAY reset/resume. Splash: static N4. Menu exits on any edge.
  KNOWN GAPS: the MI (plaits) tracks bypass the new filter/ADSR layer; the
  old mixer/sends/master-FX screens are gone (values live on); projects are
  N9P5 (older files skipped). The N4 mark's 4 glyph: judge by eye.
- [ ] **N9: storage mode retry (DMA card mode) + the DMA screen send.**
  Storage mode now re-begins the card in DMA_SDIO for the MSC session (L7's
  proven mode; FIFO was the one untested difference) and back to FIFO on
  leave - retest mount/copy/eject on the computer; if it still fails, grab
  the serial log ("storage mode: ..." lines). The DMA screen send is DEAD
  ON THIS PANEL (second strike: even the watchdog fallback wedged the app -
  a stuck transfer poisons the synchronous path too; never retry async).
  Instead the SPI clock went 30 -> 60 MHz (stock firmware runs the panel at
  70): the frame send halves to ~18 ms. CHECK for visual artifacts
  (streaks, wrong pixels) at 60 MHz, and that the framerate under ~70%
  synth CPU feels fixed; if artifacts show, step down to 48 MHz. L7 still
  runs at 30 MHz - bump it after N9 proves the speed.
- [ ] **N9: GM full quality via the SD-to-flash sample cache.** Copy
  apps_sd/GM.BNK to the card as /N9/GM.BNK (storage mode works). The
  melodic bank left the firmware: a 960 KB flash region above the app image
  caches the full-quality records (whole loops AND whole pre-loop middles,
  2 s one-shots - nothing trimmed) for the up to nine GM programs in use.
  Check: a program plays the synth patch instantly while uncached, then
  "GM LOADING <name>" (audio mutes ~1-2 s) about 0.7 s after settling on
  it, and the samples take over for new notes; boot reloads the project's
  programs during the splash; slots survive a power cycle (no reload);
  switching apps wipes the cache (shared with the switcher's buffer) and
  the next N9 boot reloads; no GM.BNK on the card = synth patches + a
  serial line. Drums are still baked in flash, unaffected.
- [ ] **N9: clips hold 128 notes now (were 64).** The clips moved to RAM2
  and projects stream them after the header blob (format N9P4; older
  files skipped). Check: a dense clip (chords every step) past 64
  notes records/edits/plays; projects with such clips survive a power
  cycle; the editor stays responsive on a full clip.
- [ ] **N9: GM rebuilt from the Roland_SC-88.sf2 soundfont.** The bank now
  comes from the soundfont's real multisamples: 106 of 128 programs covered,
  up to 4 key zones each (the soundfont's roots, tunings and loop points),
  deep loops kept by splicing out the middle (attack + 0.19 s loop,
  crossfaded seams - LISTEN for splice/loop artefacts, esp. pianos and
  pads); the GM drum map stays on the freewavesamples one-shots (the
  soundfont kit has only 11 hits). Regenerate with
  `cd N9 && python3 tools/sf2_gm_bank.py` (the sf2 is gitignored too).
- [ ] **N9: GM samples + two drum engines (superseded bank, same engines).** ~80 GM
  programs play baked SC-88 one-notes (mu-law 22 kHz from flash, sustained
  ones auto-looped: LISTEN FOR LOOP CLICKS and bad loop points - each is
  fixable in tools/sc88_bank.py; the piano is a 3-zone multisample); the
  rest keep the synth patches. GM DRUM: the standard GM drum map on SC-88
  hits (35/36 kicks, 38 snare, 42/46 hats with choke, cymbals, toms, Latin
  percussion; some notes borrowed+pitched). DRUMS: synthesized 606/707/808/
  909 (preset cell picks the machine), 12 sounds every octave - C kick,
  C# CH, D snare, D# OH (choked by CH), E clap, F/G/A toms, F#/G#/A#
  crash/ride/cym, B rim; cells TUNE/DECAY/SNAP/TONE/LEVEL at note-on. Drum
  tracks skip the scale snap on MIDI in; the roll still navigates in-scale
  rows (annoying on drum tracks - fix later?). Projects: format now N9P3
  (older files skipped). NOTE: n9_gm_pcm.cpp/.h are generated and
  GITIGNORED (license: no re-hosting) - a fresh clone must run
  `cd N9 && python3 tools/sc88_bank.py` before building. Watch CPU with
  many sampled voices; flash is now ~86% full (352 KB free).
- [ ] **N9: the GM engine (all 128 melodic programs, synthesized).** The
  engine selector's last entry, after the Mutable models; the INST preset
  cell shows "001 ACOU GRAND" etc, B+LEFT/RIGHT steps programs, B+UP/DOWN a
  family of 8; MIDI program change sets the selected track's program. Each
  program is a two-oscillator patch (n9_gm_bank.h) - no PCM, so judge them
  as Sound Canvas impressions: sweep the 16 families and flag the worst.
  The cells (CUT/RES/ENV/ATK/DEC/REL/VIB at 50 = as authored, LEVEL) offset
  the patch live. Programs save with projects (format bumped to N9P2: any
  project saved before 2026-10-15 is skipped, delete the old files). No drum
  kits yet. Watch the synth CPU line with many GM voices held.
- [ ] **N9: clip launching + the stop row + the clip clipboard.** The grid's
  rows are tighter (17 px cells) with a stop-button row underneath. A on an
  empty slot makes a clip (a hollow light frame: a clip with no notes); A on
  a resting clip arms it (dark-accent fill) and it starts from its top on the
  next bar line (the clock starts if stopped); A on the running clip (accent
  fill) opens the editor; A on a stop cell arms that track's stop (the small
  square accented, dies on the next bar). SHIFT+A cuts a clip to the
  clipboard (silencing it if it played), SHIFT+A on an empty slot pastes.
  Track numbers light up per playing track. Polymeter: launch clips of
  different lengths at different bars; each loops from its own launch bar.
- [ ] **N9: projects (menu cards) + state persistence.** The live state (all
  clips, engines + cells, FX, mixer, key/scale, tempo, armed clips) saves to
  /N9/PROJECTS/A-L.N9P: on project switch, before storage mode, before the
  app switch, at power off; /N9/CURRENT.TXT remembers the card and boot
  reloads it. Menu cards: the loaded one filled (play icon while the clock
  runs; A on it saves + closes the menu), other projects a thick outline in
  their colour (A loads - while playing it commits on the NEXT BEAT and its
  armed clips come in on the next bar line), empty grey (A = new fresh
  project, saved at once). SHIFT+A cuts a card's file to CLIP.N9P / pastes
  onto an empty card (the current card can't be cut). After storage mode the
  card's version of the current project is reloaded. Check a power cycle
  brings everything back, and a round trip through L7 does too.
- [ ] **N9: the menu's L7 layout + storage mode.** The menu now has L7's
  twelve cards (A-L, all empty-looking, grey) and the list (EXPORT ABLETON
  SET / STORAGE MODE / SETTINGS / HELP); the cursor walks cards and list as
  L7's (LEFT/RIGHT along a card row, UP/DOWN rows then the list, UP from the
  list back to the remembered column), RIGHT off the right side arms the
  exit. Only STORAGE MODE acts: A on it hands the card to the computer
  (mount, copy both ways, eject or any key to exit, card in/out while in it,
  the battery icon and the state line as L7's). After leaving, check the
  menu comes back and the card still works (switch apps, say). Also: N9 now
  answers USB disk probes from boot (usbDriveBegin) - check normal USB
  serial/MIDI still behaves on macOS.
- [ ] **N9: edge navigation + status-row focus + the menu.** A SHIFT tap
  (pressed and let go alone) toggles the cursor between the content and the
  status row (a light frame on the stood-on cell: scene, clock, tempo, action);
  there LEFT/RIGHT move, B+LEFT/RIGHT step the scene on its cell, B+arrows
  set the tempo (40-240, 1/10 steps, live) on the BPM cell. An arrow pushed
  past a content edge arms an edge switch, L7 style: a 1-block line the
  edge's whole length (light grey 50 ms, then grey), the switch on letting
  go or after 600 ms held (held on: dead until let go). UP/DOWN walk the
  screen stack; LEFT opens the (empty) dark-grey menu with the N9 corner
  logo (L7's place/size — judge the logo's look); RIGHT off the menu's edge
  goes back. Check SHIFT+UP/DOWN still steps screens, SHIFT+B+A still
  switches apps, and that a SHIFT used in a chord doesn't toggle the focus.
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
- [ ] **App switcher (both apps) + N9's SD folders.** (2026-10-22: L7's side
  removed - no SHIFT+B+PLAY, and an L7.HEX built since is refused by the
  other app's switch, having no FLASH_ID; only N9/K6 matter here now.) Copy apps_sd/L7.HEX
  and N9.HEX to the card as /APPS/L7.HEX and /APPS/N9.HEX (card reader or
  L7 storage mode; tools/make_apps.sh rebuilds both). N9: SHIFT+B+A
  switches to L7; L7: SHIFT+B+PLAY switches to N9 (saves its session
  first). ~10-20 s with a progress count; "DON'T POWER OFF" marks the few
  unsafe seconds (power loss there = USB reflash). N9 also makes /N9,
  /N9/PROJECTS and /APPS on the card at boot. Check both directions, a
  missing-file failure message, and that L7 still boots its project after
  a round trip. L7's switcher is BUILD-VERIFIED only until L7.HEX is
  flashed via the switch itself. CAUTION (seen 2026-10-13): a stale
  /APPS/N9.HEX from before the boot-loop fix flashed a crash-looping N9
  (black screen, continuous resets; rescued over USB) — the switch
  mechanism itself worked. Always restage apps_sd/ via tools/make_apps.sh
  after any code change and re-copy BOTH hexes before testing a switch.
- [ ] **N9: the send/insert FX chains, 8 params, and the working mixer.**
  Effects have 8 cells, the last always DRY/WET. Each track: FX1+FX2 in
  series, each an insert (FLANGER/PHASER/TREM/DRIVE/CRUSH/FILTER/COMP/WOW,
  per-track) or a send (DELAY/REVERB/CHORUS, global stereo post-fader;
  cells 1-7 edit the SHARED effect, cell 8 is this track's send: to midway
  send rises to 100%, past midway the dry path to the mixer fades). Series
  rules per the spec (send+send taps full output; insert+send sends FX1's
  result). Master: one stereo effect (PINGPONG lives only there), then
  master volume, then the clipper. MIXER: 10 strips (9 + master), B+UP/DOWN
  volume, B+LEFT/RIGHT pan, A mutes (kills its post-fader sends too), live
  VU per track and stereo master pair (refresh every 80 ms on that screen).
  Listen hard to: send ramp behaviour around midway, two sends on one
  track, reverb/delay tails when muting, pan law at extremes.
- [ ] **N9: key/scale, roll pages and octaves.** B+LEFT/RIGHT on the grid
  steps the key, B+UP/DOWN the scale (overlay while B held; C chromatic =
  no restriction). The roll: cursor only lands on in-scale pitches (strip
  marks them; dark line above each C), UP/DOWN slides the window by octaves
  at the edges (24-96), LEFT/RIGHT runs across bar pages (16 max; the strip
  frames the shown page, fills the clip's bars, underlines the playing bar);
  placing/stretching notes extends the clip's bars, removing shrinks them;
  clips loop their own length (polymeter). MIDI in snaps to the scale (ties
  downward); stored out-of-scale notes play untouched.
- [ ] **N9: all tracks play at once; the decided FX topology.** PLAY now
  fires every track's clip at the current scene through its own engine and
  its own MONO insert (one per track, live simultaneously - light effects
  only; DELAY/PINGPONG/REVERB are master-only and the track selector skips
  them), summed to ONE stereo master effect, then a soft clipper into 16
  bit (no hard clipping however loud). MIDI still plays the selected track;
  a track's 303 is mono per track. Listen: two+ tracks with different
  engines and inserts at once; CPU log with a full grid; the clipper's
  behaviour when stacking loud tracks (should squash, not crack).
- [ ] **N9: per-track engines and FX.** Each track keeps its own engine, its
  per-engine cells, and its two TFX slot settings; the grid cursor's track is
  what plays, and switching tracks swaps its FX settings into the two track
  processors (tails clear - two "fx: slot" lines on serial). Check: set
  different engines/FX on two tracks, flip between them on the grid, and
  confirm INST/TFX follow and edits stick per track; held notes from the old
  track keep their own cells (no cross-bleed).
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
- Master volume: 22 lines, 100 % at line 16 (about three quarters lit), even ~2.0 dB steps (the meter's) up to 400 %; a session or project saved on the scales before comes back at the same level. Tempo 60-240 BPM (2026-10-22, was 30-300; tap, B+UP, MIDI clock, audio follow doubling refused past it): playback, recording, MIDI out, the tempo label (240.0, 60.0). A project with scenes saved outside it: loads them at 60 / 240.
- Master volume per project: set it in one project, load another (its own; a new one at 100 %), back again, power cycle. Projects from before take the volume playing when first loaded.
- Defrag samples (settings, after a delete's message goes): on a well-used card: the bar by bytes, then DEFRAGGED N (N, M NO ROOM); recordings play as before, the tracks on them too; pull the power during one: the recording back at the next start, no .NEW/.OLD left in SAMPLES. On exFAT, files a computer copied over are all rewritten the first time. Takes a while for many large files.
- Card hot swap (stopped): take the card out: INSERT AN SD CARD; put another in: its projects and samples load (a blank card: a fresh /L7). Swap during storage mode: the drive goes away, the new one mounts. Enter storage mode after a swap: the new card shows (the bug: connected, no drive; then a lock-up). Start with no card: the screen waits, then starts.
- Editing while playing at a high tempo (held arrows on filter / range, several tracks): no stutter (edits' writes now one every 2 s while playing; the serial log shows no 'slow SD read'). Stop: everything written at once.
- Defrag with little room: it drops the recording space first (the log says so), then copies; the recording space comes back while stopped (watch the log: 'recording space: making ...'), recording works right after.
- Switch off in the looper: it starts in the looper (the selection as it was); off in the menu: the menu. Key bus trouble: the serial log says 'keys: the key bus failed', the app keeps running (reads backed off 50 ms), no long freezes in 'keys (I2C)'.
- High tempo, many stretched tracks, editing cells: no 'slow SD read … card error 0x0B/0x0C data 0xB' on the serial log any more (the SDIO patch), no 'slow pass' lines over ~60 ms, no stutter.
- High tempo (more than twice the clips' own), a long resampling (10+ bars): the new scene switches in by itself (no SHIFT+START needed); if a track runs dry it comes back in step.
- Stop, start and press record at once: the recording starts on the start's bar line, its length right.
- Export while playing, right after recording (the export stops the clock itself): the Live set's samples are the clips' own size (not 22 MB each, the recording space's) and named (A30012.wav ...); the export no slower than from stopped. The set opens in Live, the new clips play.
- README logo (2026-10-23): check on github.com in both themes that the logo over the README shows (the `<picture>`, light and dark, and the relative SVG paths through their proxy) and that 120 px wide looks right beside the text; nothing of it could be rendered locally.
- Model:02 (2026-10-23, none of it on hardware - there is no Model:02 here): `-DM8_MODEL=2` lays the screen out for 480 x 320 (rows 40 px, waveform 328, filter 80, volume 48 unchanged). The Model:01 build renders byte-identically (all 81 UI preview screens diffed). Check on a device when there is one: the waveform centred and scaled in the taller cell (it sits 1 px above centre, the cell being an odd number of block rows), the filter curve's shape at 80 x 42, the volume cell's 20 steps still reading right. Check too: the status row (34 px tall, 6 px of space above it; scene 172, clock 94 as the Model:01's, tempo 188) - the scene's and tempo's contents 8 px in from the row's ends (not 4), the clock centred between its neighbours, each cell's frame when selected holding a similar amount of space, the scene's widest content (B held: arrows + SCN + "250*/250") still fitting, and the master meter (B held on the clock cell) wrapped as closely as the Model:01's. No board layer yet: the display driver (480 x 320 is not the ILI9341), its pins, keys and backlight. Still laid out for 320 x 240 at the bigger size: the help pages (`help_slides.h`, generated artwork with absolute coordinates), the menu's vertical placement, the splash logo's scale, the storage-mode screen.
