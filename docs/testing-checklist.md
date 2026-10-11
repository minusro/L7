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
- [ ] **Recording stop grace (200 ms)** and the 16-bar limit below 60 BPM.

## Open problems

- [ ] **Recording files on a card with scattered free space**: making one
  needs a 21 MB unbroken block; without it L7 thinks the card is full, stops
  refilling the 24, and later recordings make their own while playing
  (snags). Workaround: copy off, format, copy back. Fix to do: fall back to
  growing the file in small steps while stopped.
- [ ] **Snags on project load**: the dropout log now names the step that ran
  short (`underrunStage`); capture a log while loading projects.
- [ ] **A dropout while a recording is saved** (seen once: track 6, the loop
  drawing the screen).
- [ ] **Resampling sometimes starts a bar late**: it can't look back to the
  bar line just passed (no history of the mix). Decide: keep, show it, or
  add a mix history (RAM).
- [ ] **Swipe**: a held arrow stops one track short of the start; undoing after
  a full lap skips a track.
- [ ] **Simulator: "12 switches at 120 BPM"** fails with dropouts since the
  200 ms grace change (state from earlier tests); look into or loosen.
- [ ] **RAM1 headroom** is ~1.2 KB: move cold code to flash before the next
  bigger feature.
