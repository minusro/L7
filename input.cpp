// input.cpp — see input.h. The key list is at the top of m8_looper.ino.
#include "input.h"
#include "ui.h"

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;   // held arrows
static constexpr uint32_t DOUBLE_TAP_MS = 200;      // two taps of a key this close: a double tap
static constexpr uint32_t EDGE_HOLD_MS = 350;       // SHIFT+UP/DOWN's second tap held this long: to the list's end

// The selected track's current cell back to its defaults: the whole loop as
// the range, the filter open, volume at unity and balance centred.
static void resetCell() {
  if (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == CLOCK_CELL) {   // the master volume: off, or back to 100% from off
    stepMasterVolume((masterVolume() == 0 ? MASTER_VOLUME_UNITY : 0) - masterVolume());
    return;
  }
  if (selected == STATUS_ROW || !hasClip(selected)) return;
  const Track &tr = tracks[selected];
  if (selectedCell == WAVE_CELL) {
    setRange(selected, 0, tr.loopBars * BEATS_PER_BAR);
  } else if (selectedCell == FILTER_CELL) {
    setTrackFilter(selected, 0, FILTER_MAX);
    trackChanged(selected);
  } else if (selectedCell == MIX_CELL) {
    setTrackMix(selected, VOLUME_UNITY, 0);
    trackChanged(selected);
  }
}

// Tap tempo (A on the status row): the tempo follows the average of the last
// TAP_AVERAGE intervals from the second tap on. Taps further apart than
// TAP_TIMEOUT_MS start a new count, and so does any other key press in between. Every 4th tap after the first (the 5th,
// 9th, 13th ...) lands on a bar line: a stopped transport starts there from
// 1.1.1; a running one snaps to the nearest bar start.
static constexpr uint32_t TAP_TIMEOUT_MS = 2100;     // a little over a beat at the slowest tempo (30 BPM)
static constexpr int TAP_AVERAGE = 4;

static int tapCount = 0;                            // taps in this run (0: the next is the first)

static void tapTempo() {
  static uint32_t times[TAP_AVERAGE + 1];           // the latest taps, oldest first
  int &count = tapCount;
  uint32_t now = millis();
  if (count > 0 && now - times[min(count, TAP_AVERAGE + 1) - 1] > TAP_TIMEOUT_MS) count = 0;
  if (count >= TAP_AVERAGE + 1) memmove(times, times + 1, TAP_AVERAGE * sizeof times[0]);
  times[min(count, TAP_AVERAGE)] = now;
  count++;
  int intervals = min(count, TAP_AVERAGE + 1) - 1;
  if (intervals > 0 && now > times[0]) setTempo(60000.0f * intervals / (now - times[0]));
  if (count > 1 && count % 4 == 1) {                // 5th, 9th, ...: a bar line now
    if (transport.running) transportSnapToBar();
    else transportRestart();
  }
}

// With SHIFT and B both held, which came first: A then pastes a scene (B
// first) or cuts it (SHIFT first).
static bool optFirst = false;
bool optBeforeShift() { return optFirst; }

// SHIFT tapped twice (within DOUBLE_TAP_MS), the second tap held: A records
// another way while it's held: an empty track resamples (records the app's
// output), one with a clip overdubs (not there yet).
static bool shiftTwice = false;
// The last arrow press moved the selection (not: already at the edge that way).
static bool moved = false;
bool arrowsMoved() { return moved; }
bool shiftTwiceHeld() { return shiftTwice && (board.keys() & m8::BTN_SHIFT); }

static void onKeys(uint8_t keys, uint8_t pressed, bool repeat) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  if (pressed & m8::BTN_OPT) optFirst = !(shift && !(pressed & m8::BTN_SHIFT));
  if (pressed & m8::BTN_SHIFT) optFirst = opt && !(pressed & m8::BTN_OPT);
  {
    static uint32_t lastShiftTap = 0;
    if (!shift) shiftTwice = false;
    if (pressed & m8::BTN_SHIFT) {
      shiftTwice = millis() - lastShiftTap < DOUBLE_TAP_MS;
      lastShiftTap = shiftTwice ? 0 : millis();
    }
  }
  // The menu's items: the arrows move its selection; A or B (either, or both
  // at once) runs the action button's action.
  if (menuOpen() && !menuOnStatus()) {
    uint8_t act = m8::BTN_EDIT | m8::BTN_OPT;
    if ((pressed & act) && !((keys & ~pressed) & act)) {   // (the second of the two: nothing more)
      noteActionPress();
      int slot = menuSlot();
      if (slot < 0) {                                // the list's items: nothing yet
      } else if (shift) {                            // SHIFT+A on a slot: cut its project, or paste one into it
        projectFilled(slot) ? projectCut(slot) : projectPaste(slot);   // (SHIFT+A or SHIFT+B)
        drawMenu();
        for (int i = 0; i < ROWS; i++) drawTrack(i);   // (the loaded one cut: the looper emptied)
      } else {                                       // A: load it (an empty slot: a new project), back to the looper
        int row, cell;
        mainSelection(row, cell);
        projectSetSelection(row, cell);              // (the project left keeps its selection)
        if (projectLoad(slot)) {
          closeMenu();
          takeProjectSelection();
          for (int i = 0; i < ROWS; i++) drawTrack(i);
        }
      }
    } else if (!(pressed & act)) {
      bool went = menuKeys(pressed);
      if (pressed & (m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN)) moved = went;
    }
    return;
  }
  // The menu's selection on the status row: as in the looper's view, but UP
  // (or LEFT from the scene cell) goes back to the menu's items.
  if (menuOpen() && !shift && !opt &&
      ((pressed & m8::BTN_UP) || ((pressed & m8::BTN_LEFT) && selectedCellIn(STATUS_ROW) == SCENE_CELL))) {
    menuLeaveStatus();                               // (LEFT from its first cell too: the list's last item)
    moved = true;
    return;
  }
  bool onStatus = selected == STATUS_ROW;
  int prevSel = selected;
  // A on the status row's scene cell: as B+A (the panel); on the clock cell:
  // as START; on the tempo cell: tap tempo.
  bool onScene = onStatus && selectedCellIn(STATUS_ROW) == SCENE_CELL;
  bool onClock = onStatus && selectedCellIn(STATUS_ROW) == CLOCK_CELL;
  bool onTempo = onStatus && selectedCellIn(STATUS_ROW) == TEMPO_CELL;
  // Any press but a lone A on the tempo cell (a tap): the next tap starts a new count.
  if (pressed && !(onTempo && pressed == m8::BTN_EDIT && keys == m8::BTN_EDIT)) tapCount = 0;
  bool sceneChanged = false;                         // every row to redraw
  // The scene keys' modifier: SHIFT, or B on the status row's scene cell.
  bool sceneMod = shift || (opt && onStatus && selectedCellIn(STATUS_ROW) == SCENE_CELL);
  // Held while playing: scene steps only move the label; the scene changes
  // once it's let go (see the end).
  if (sceneMod && transport.running) sceneHold(true);

  // SHIFT+UP/DOWN (or B+UP/DOWN on the scene cell) tapped twice: the second
  // tap waits. Held EDGE_HOLD_MS, it goes to that end of the scene list; let
  // go sooner, it's a step after all.
  static uint8_t edgeKey = 0;
  static uint32_t edgeAt = 0;
  uint8_t edgeDone = 0;                              // the arrow this call settled
  if (edgeKey) {
    int dir = edgeKey == m8::BTN_UP ? 1 : -1;
    if (!(keys & edgeKey) || !sceneMod) {
      sceneChanged |= sceneStep(dir);
      edgeDone = edgeKey;
      edgeKey = 0;
    } else if ((int32_t)(millis() - edgeAt) >= 0) {
      sceneChanged |= dir < 0 ? sceneGoEmpty() : sceneGoEnd();
      edgeDone = edgeKey;
      edgeKey = 0;
    } else {
      pressed &= ~edgeKey;                           // still waiting: no repeats
    }
  }

  // OPT (B) tapped twice within DOUBLE_TAP_MS (with no arrows or A in
  // between): the current cell's parameters back to their defaults; with
  // SHIFT, a copy of the scene inserted after it.
  static uint32_t lastOptTap = 0;
  static bool optTapArmed = false;
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  if (pressed & (arrows | m8::BTN_EDIT)) optTapArmed = false;   // B was used as a modifier
  if (pressed & m8::BTN_OPT) {
    if (optTapArmed && millis() - lastOptTap < DOUBLE_TAP_MS) {
      if (shift) sceneChanged = sceneDuplicate();
      else resetCell();
      optTapArmed = false;
    } else {
      optTapArmed = true;
      lastOptTap = millis();
    }
  }

  // SHIFT+UP/DOWN: newer / older scene; tapped twice, the second tap held
  // (see above): straight to that end (UP: N+1*, DOWN: the empty scene 0*).
  auto sceneKeys = [&] {
    static uint32_t lastTap[2] = {0, 0};
    const uint8_t keys2[2] = {m8::BTN_UP, m8::BTN_DOWN};
    for (int k = 0; k < 2; k++) {
      if (!(pressed & keys2[k] & ~edgeDone)) continue;
      bool twice = !repeat && millis() - lastTap[k] < DOUBLE_TAP_MS;
      if (twice) {
        edgeKey = keys2[k];
        edgeAt = millis() + EDGE_HOLD_MS;
      } else {
        sceneChanged |= sceneStep(k == 0 ? 1 : -1);
      }
      lastTap[k] = repeat || twice ? 0 : millis();
    }
  };

  if (opt) {
    // OPT (B) + arrows: the current cell's parameters (none on empty tracks).
    if (onStatus) {
      if (selectedCellIn(STATUS_ROW) == SCENE_CELL) {   // scene: UP/DOWN as SHIFT+UP/DOWN
        sceneKeys();
      } else if (onTempo) {                          // tempo: 0.1 BPM steps, or 1 BPM
        if (pressed & m8::BTN_LEFT) setTempo(bpm - 0.1f);
        if (pressed & m8::BTN_RIGHT) setTempo(bpm + 0.1f);
        // UP/DOWN: to the next whole BPM that way (120.5 up: 121; 135 down: 134).
        long tenths = lroundf(bpm * 10);
        if (pressed & m8::BTN_UP) setTempo(tenths / 10 + 1);
        if (pressed & m8::BTN_DOWN) setTempo((tenths + 9) / 10 - 1);
      } else if (onClock) {                          // the master volume, a step either way
        if (pressed & (m8::BTN_LEFT | m8::BTN_DOWN)) stepMasterVolume(-1);
        if (pressed & (m8::BTN_RIGHT | m8::BTN_UP)) stepMasterVolume(1);
      }
    } else if (selectedCell == WAVE_CELL) {
      // Loop range in beats: any whole number of beats, up to the whole loop.
      //   LEFT/RIGHT: move its start a step, wrapping around the loop (a range
      //     may run past the loop's end): a beat for loops up to 8 bars (BEAT_STEP_MAX_BARS), half
      //     a bar up to 16 bars, a bar beyond. Starts off that grid go to the
      //     next grid point that way.
      //   UP/DOWN: to the next / previous of 1/4, 1/2, 3/4, 1, 1 1/2, 2, 3, 4,
      //     5, 6 ... bars (up to the whole loop).
      //   Tapped twice, LEFT/RIGHT: by its own length. A range inside the loop
      //     stops at the loop's edge rather than crossing it; one already at
      //     that edge jumps to the other. A range across the edge moves freely.
      //   Tapped twice, UP/DOWN: to the next / previous power of two: 1/4,
      //     1/2, 1, 2, 4, 8, 16, 32 bars (up to the whole loop). Three times a
      //     power of two (3/4, 1 1/2, 3, 6 ... bars) doubles / halves instead
      //     (3/4 bar halves to 1/2: no half beats).
      // The start stays when the length changes. A double tap counts from the
      // range before the first tap (which already moved it a step).
      Track &tr = tracks[selected];
      uint32_t rs, rb, loopBeats = tr.loopBars * BEATS_PER_BAR;
      trackRange(selected, rs, rb);                  // includes a queued change
      static int tapTrack = -1;
      static uint32_t tapAt[4], tapStart[4], tapLen[4];   // per arrow: the last tap and the range before it
      const uint8_t keys4[4] = {m8::BTN_LEFT, m8::BTN_RIGHT, m8::BTN_UP, m8::BTN_DOWN};
      // Lengths in beats: the steps (1, 2, 3, 4, 6, 8, then every 4) and the
      // jumps (powers of two, or doubling / halving three times one). Next
      // above / below l (at least 1 beat).
      auto stepAbove = [](uint32_t l) { return l < 4 ? l + 1 : l < 8 ? (l / 2 + 1) * 2 : (l / 4 + 1) * 4; };
      auto stepBelow = [](uint32_t l) {
        return l <= 1 ? 1 : l <= 4 ? l - 1 : l <= 8 ? (l - 1) / 2 * 2 : (l - 1) / 4 * 4;
      };
      auto threeTimesPow2 = [](uint32_t l) { return l % 3 == 0 && ((l / 3) & (l / 3 - 1)) == 0; };
      auto jumpAbove = [&](uint32_t l) {
        if (threeTimesPow2(l)) return l * 2;
        uint32_t j = 1; while (j <= l) j *= 2; return j;
      };
      auto jumpBelow = [&](uint32_t l) {
        if (threeTimesPow2(l) && l >= 6) return l / 2;
        uint32_t j = 1; while (j * 2 < l) j *= 2; return l <= 1 ? 1 : j;
      };
      uint32_t step = tr.loopBars <= BEAT_STEP_MAX_BARS ? 1 : tr.loopBars <= 16 ? 2 : 4;   // the start's grid
      for (int k = 0; k < 4; k++) {
        if (!(pressed & keys4[k])) continue;
        bool twice = !repeat && tapTrack == selected && tapAt[k] && millis() - tapAt[k] < DOUBLE_TAP_MS;
        uint32_t s0 = twice ? tapStart[k] : rs, l0 = twice ? tapLen[k] : rb, s = s0, l = l0;
        if (!twice && !repeat) { tapTrack = selected; tapAt[k] = millis(); tapStart[k] = rs; tapLen[k] = rb; }
        if (twice) tapAt[k] = 0;
        bool inside = s0 + l0 <= loopBeats;          // not across the loop's edge
        switch (k) {
          case 0:                                    // LEFT
            if (!twice) s = s0 % step ? s0 / step * step : (s0 + loopBeats - step) % loopBeats;
            else if (!inside) s = (s0 + loopBeats - l0 % loopBeats) % loopBeats;
            else s = s0 == 0 ? loopBeats - l0 : s0 < l0 ? 0 : s0 - l0;
            break;
          case 1:                                    // RIGHT
            if (!twice) s = (s0 / step + 1) * step % loopBeats;
            else if (!inside) s = (s0 + l0) % loopBeats;
            else s = s0 + l0 == loopBeats ? 0 : s0 + 2 * l0 > loopBeats ? loopBeats - l0 : s0 + l0;
            break;
          case 2:                                    // UP
            l = min(twice ? jumpAbove(l0) : stepAbove(l0), loopBeats);
            break;
          case 3:                                    // DOWN
            l = twice ? jumpBelow(l0) : stepBelow(l0);
            break;
        }
        if (s != rs || l != rb) setRange(selected, s, l);
        rs = s;
        rb = l;
      }
    } else if (selectedCell == FILTER_CELL && hasClip(selected)) {
      // Filter: LEFT/RIGHT move the passband a step; moving stops where both
      // points would leave the range. UP/DOWN widen/narrow it: a step on each
      // side around its centre, or, while one point sits at its edge (the
      // highpass at the bottom, the lowpass at the top; off), a step of the
      // other point only, from that edge. With both at their edges (flat),
      // around the centre again. Narrowing near an edge slides the band back
      // in (setTrackFilter).
      const Track &tr = tracks[selected];
      int lo = tr.filterLo, hi = tr.filterHi;
      if (pressed & m8::BTN_LEFT && hi > 0) { lo--; hi--; }
      if (pressed & m8::BTN_RIGHT && lo < FILTER_MAX) { lo++; hi++; }
      int grow = (pressed & m8::BTN_UP ? 1 : 0) - (pressed & m8::BTN_DOWN ? 1 : 0);
      bool loEdge = lo <= 0, hiEdge = hi >= FILTER_MAX;
      if (grow && loEdge != hiEdge) {
        if (loEdge) {                                  // from the bottom: the lowpass moves
          lo = 0;
          hi = constrain(hi + grow, lo, lo + FILTER_MAX);
        } else {                                       // from the top: the highpass moves
          hi = FILTER_MAX;
          lo = constrain(lo - grow, hi - FILTER_MAX, hi);
        }
      } else if (grow > 0 && hi - lo + 2 <= FILTER_MAX) {
        lo--; hi++;
      } else if (grow < 0 && hi - lo >= 2) {
        lo++; hi--;
      }
      if (lo != tr.filterLo || hi != tr.filterHi) {
        setTrackFilter(selected, lo, hi);
        trackChanged(selected);
      }
    } else if (selectedCell == MIX_CELL && hasClip(selected)) {
      // Volume and balance: UP/DOWN louder/quieter, RIGHT/LEFT towards the
      // right/left channel.
      const Track &tr = tracks[selected];
      int volume = tr.volume, balance = tr.balance;
      if (pressed & m8::BTN_UP) volume++;
      if (pressed & m8::BTN_DOWN) volume--;
      if (pressed & m8::BTN_RIGHT) balance++;
      if (pressed & m8::BTN_LEFT) balance--;
      volume = constrain(volume, 0, VOLUME_MAX);
      balance = constrain(balance, -BALANCE_MAX, BALANCE_MAX);
      if (volume != tr.volume || balance != tr.balance) {
        setTrackMix(selected, volume, balance);
        trackChanged(selected);
      }
    }
  } else if (shift) {
    sceneKeys();
    // SHIFT+LEFT/RIGHT on a track: its sample, the previous / next on the card.
    if (!onStatus && !recordingOn(selected)) {
      if (pressed & m8::BTN_LEFT) stepSample(selected, -1);
      if (pressed & m8::BTN_RIGHT) stepSample(selected, 1);
    }
  } else {
    int row = selected, at = selectedCellIn(selected);
    if (pressed & m8::BTN_UP && selected > 0) selected--;              // no wrapping
    if (pressed & m8::BTN_DOWN && selected < ROWS - 1) selected++;
    if (pressed & m8::BTN_LEFT && selectedCell > 0) selectedCell = selectedCellIn(selected) - 1;
    if (pressed & m8::BTN_RIGHT && selectedCell < cellCount(selected) - 1) selectedCell++;
    if (pressed & arrows) moved = selected != row || selectedCellIn(selected) != at;
  }

  // START: start from 1.1.1 / stop. With SHIFT: start from 1.1.1 on every
  // press, running or not. A on the status row: the menu (scene cell), START
  // (clock cell), tap tempo (tempo cell); with SHIFT, on any of them: the same
  // as SHIFT+START.
  // B+A: open the menu. With SHIFT too, the order they were pressed in:
  // B then SHIFT, +A: paste the scene clipboard after the scene; SHIFT then
  // B, +A: cut the scene to it.
  if (pressed & m8::BTN_EDIT) noteActionPress();     // before it acts: the icon shows what it does
  bool edit = (pressed & m8::BTN_EDIT) && !opt;
  if ((pressed & m8::BTN_EDIT) && opt) {
    if (!shift) menuOpen() ? closeMenu() : openMenu();
    else sceneChanged = optFirst ? scenePaste() : sceneCut();
  }
  bool transportKey = pressed & m8::BTN_PLAY;
  bool toggleTransport = (transportKey || (edit && onClock)) && !shift;
  if ((transportKey || (edit && onStatus)) && shift) transportRestart();
  if (edit && onStatus && !shift) {
    if (onScene) menuOpen() ? closeMenu() : openMenu();   // as B+A
    else if (onTempo) tapTempo();                  // (the clock cell: as START, below)
  }
  if (edit && !onStatus) {
    Track &tr = tracks[selected];
    if (shiftTwice && !recordingOn(selected) && recordQueued() != selected) {
      // SHIFT tapped twice and held, A: an empty track resamples (queued
      // behind a recording, as A records); one with a clip overdubs (to come).
      if (tracks[selected].state == TrackState::Empty) recordPress(selected, true);
    } else if (shift) {
      // SHIFT+A: cut the clip to the clipboard, or paste it onto an empty
      // track; recording (or queued): cancel it, the recording thrown away.
      if (hasClip(selected)) cutTrack(selected);
      else if (recordingOn(selected) || recordQueued() == selected) cancelRecording(selected);
      else pasteTrack(selected);
    } else if (tr.state == TrackState::Empty || recordingOn(selected)) {
      recordPress(selected);
    } else {
      setTrackMuted(selected, !trackMuted(selected));
    }
  }
  if (toggleTransport) {
    if (transport.running) transportStop();
    else transportStart();
  }
  if (!sceneMod) sceneChanged |= sceneHold(false);  // SHIFT (or B) let go: to the scene stepped to
  // Show the result right away rather than on the rows' next redraw: the new
  // selection first, then the old one, so highlight and icon move together.
  if (sceneChanged)
    for (int i = 0; i < ROWS; i++) drawTrack(i);
  drawTrack(selected);
  if (prevSel != selected) drawTrack(prevSel);
}

// After a recording finishes on the selected track, move on: to the next
// empty track (in their numbering, from the bottom up, wrapping), else the
// next muted one, else stay.
static void selectAfterRecording(int t) {
  if (selected != t) return;                       // the user has moved on already
  for (TrackState want : {TrackState::Empty, TrackState::Muted}) {
    for (int k = 1; k < NUM_TRACKS; k++) {
      int i = (t - k + NUM_TRACKS) % NUM_TRACKS;   // (the track numbered next: the row above)
      if (tracks[i].state == want) {
        selected = i;
        drawTrack(i);
        drawTrack(t);
        return;
      }
    }
  }
}

void pollKeys() {
  // Arrows repeat while held: the press acts at once, then again after
  // REPEAT_DELAY_MS and every REPEAT_PERIOD_MS (the most recently pressed one).
  uint8_t keys;
  static uint8_t prev = 0, repeatKey = 0;
  static uint32_t repeatAt = 0;
  const uint8_t ARROWS = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  if (board.keysChanged(keys)) {
    debugStage = "key action";
    uint8_t pressed = keys & ~prev;
    if (pressed & ARROWS) {
      repeatKey = pressed & ARROWS & -(pressed & ARROWS);   // one of them, if several
      repeatAt = millis() + REPEAT_DELAY_MS;
    } else if (!(keys & repeatKey)) {
      repeatKey = 0;                                 // released
    }
    onKeys(keys, pressed, false);
    prev = keys;
  } else if (repeatKey && (int32_t)(millis() - repeatAt) >= 0) {
    debugStage = "key repeat";
    repeatAt = millis() + REPEAT_PERIOD_MS;          // after a slow pass: no burst of catch-up repeats
    onKeys(prev, repeatKey, true);
  }
}

void followRecording() {
  // A resampling saved: a new scene holding just it (scenes.h), its track
  // selected.
  int resampled = takeResampled();
  if (resampled >= 0) {
    if (sceneFromResample(resampled)) {
      selected = resampled;
      for (int i = 0; i < ROWS; i++) drawTrack(i);
    } else {
      endSilencing();                              // (scene list full: it stays where it is)
    }
  }
  // A recording that ended with a clip (not cancelled): select the next track
  // (if SELECT_NEXT_AFTER_RECORDING).
  static int recTrack = -1;
  if (recording.mode != Recording::Idle) {
    recTrack = recording.track;
  } else if (recTrack >= 0) {
    if (SELECT_NEXT_AFTER_RECORDING && tracks[recTrack].state != TrackState::Empty) selectAfterRecording(recTrack);
    recTrack = -1;
  }
}
