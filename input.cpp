// input.cpp — see input.h. The key list is at the top of m8_looper.ino.
#include "input.h"
#include "ui.h"

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;   // held arrows
static constexpr uint32_t DOUBLE_TAP_MS = 200;      // two taps of a key this close: a double tap

// The selected track's current cell back to its defaults: the whole loop as
// the range, the filter open, volume at unity and balance centred.
static void resetCell() {
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
// TAP_TIMEOUT_MS start a new count. Every 4th tap after the first (the 5th,
// 9th, 13th ...) lands on a bar line: a stopped transport starts there from
// 1.1.1; a running one snaps to the nearest bar start.
static constexpr uint32_t TAP_TIMEOUT_MS = 2100;     // a little over a beat at the slowest tempo (30 BPM)
static constexpr int TAP_AVERAGE = 4;

static void tapTempo() {
  static uint32_t times[TAP_AVERAGE + 1];           // the latest taps, oldest first
  static int count = 0;                             // taps in this run
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

static void onKeys(uint8_t keys, uint8_t pressed, bool repeat) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  if (pressed & m8::BTN_OPT) optFirst = !(shift && !(pressed & m8::BTN_SHIFT));
  if (pressed & m8::BTN_SHIFT) optFirst = opt && !(pressed & m8::BTN_OPT);
  if (panelOpen()) {                                 // the panel: only A, to close it
    if (pressed & m8::BTN_EDIT) {
      noteActionPress();
      closePanel();
    }
    return;
  }
  bool onStatus = selected == STATUS_ROW;
  int prevSel = selected;
  bool sceneChanged = false;                         // every row to redraw

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

  if (opt) {
    // OPT (B) + arrows: the current cell's parameters (none on empty tracks).
    if (onStatus) {
      if (selectedCell == TEMPO_CELL) {              // tempo: 0.1 BPM steps, or 1 BPM
        if (pressed & m8::BTN_LEFT) setTempo(bpm - 0.1f);
        if (pressed & m8::BTN_RIGHT) setTempo(bpm + 0.1f);
        // UP/DOWN: to the next whole BPM that way (120.5 up: 121; 135 down: 134).
        long tenths = lroundf(bpm * 10);
        if (pressed & m8::BTN_UP) setTempo(tenths / 10 + 1);
        if (pressed & m8::BTN_DOWN) setTempo((tenths + 9) / 10 - 1);
      }
    } else if (selectedCell == WAVE_CELL) {
      // Loop range in beats, length a power of two. LEFT/RIGHT move the start
      // by one beat, wrapping around the loop (a range may run past the
      // loop's end); UP/DOWN double/halve the length.
      Track &tr = tracks[selected];
      uint32_t rs, rb, loopBeats = tr.loopBars * BEATS_PER_BAR;
      trackRange(selected, rs, rb);                  // includes a queued change
      if (pressed & m8::BTN_LEFT) setRange(selected, (rs + loopBeats - 1) % loopBeats, rb);
      if (pressed & m8::BTN_RIGHT) setRange(selected, (rs + 1) % loopBeats, rb);
      if (pressed & m8::BTN_UP && rb * 2 <= loopBeats) setRange(selected, rs, rb * 2);
      if (pressed & m8::BTN_DOWN && rb > 1) setRange(selected, rs, rb / 2);
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
    // SHIFT+UP/DOWN: newer / older scene; twice quickly: straight to that
    // end (UP: N+1*, DOWN: the empty scene 0*).
    static uint32_t lastUpTap = 0, lastDownTap = 0;
    if (pressed & m8::BTN_UP) {
      bool twice = !repeat && millis() - lastUpTap < DOUBLE_TAP_MS;
      sceneChanged |= twice ? sceneGoEnd() : sceneStep(1);
      lastUpTap = repeat || twice ? 0 : millis();
    }
    if (pressed & m8::BTN_DOWN) {
      bool twice = !repeat && millis() - lastDownTap < DOUBLE_TAP_MS;
      sceneChanged |= twice ? sceneGoEmpty() : sceneStep(-1);
      lastDownTap = repeat || twice ? 0 : millis();
    }
    // SHIFT+LEFT/RIGHT on a track: its sample, the previous / next on the card.
    if (!onStatus && !recordingOn(selected)) {
      if (pressed & m8::BTN_LEFT) stepSample(selected, -1);
      if (pressed & m8::BTN_RIGHT) stepSample(selected, 1);
    }
  } else {
    if (pressed & m8::BTN_UP && selected > 0) selected--;              // no wrapping
    if (pressed & m8::BTN_DOWN && selected < ROWS - 1) selected++;
    if (pressed & m8::BTN_LEFT && selectedCell > 0) selectedCell--;
    if (pressed & m8::BTN_RIGHT && selectedCell < CELLS - 1) selectedCell++;
  }

  // START: start from 1.1.1 / stop. With SHIFT: start from 1.1.1 on every
  // press, running or not. A on the status row: tap tempo (with SHIFT: the
  // same as SHIFT+START).
  // B+A: open the panel. With SHIFT too, the order they were pressed in:
  // B then SHIFT, +A: paste the scene clipboard after the scene; SHIFT then
  // B, +A: cut the scene to it.
  if (pressed & m8::BTN_EDIT) noteActionPress();     // before it acts: the icon shows what it does
  bool edit = (pressed & m8::BTN_EDIT) && !opt;
  if ((pressed & m8::BTN_EDIT) && opt) {
    if (!shift) openPanel();
    else sceneChanged = optFirst ? scenePaste() : sceneCut();
  }
  bool transportKey = pressed & m8::BTN_PLAY;
  bool toggleTransport = transportKey && !shift;
  if ((transportKey || (edit && onStatus)) && shift) transportRestart();
  if (edit && onStatus && !shift) tapTempo();
  if (edit && !onStatus) {
    Track &tr = tracks[selected];
    if (shift) {
      // SHIFT+A: cut the clip to the clipboard, or paste it onto an empty
      // track (nothing while recording).
      if (hasClip(selected)) cutTrack(selected);
      else if (!recordingOn(selected)) pasteTrack(selected);
    } else if (tr.state == TrackState::Empty || recordingOn(selected)) {
      recordPress(selected);
    } else {
      tr.state = tr.state == TrackState::Playing ? TrackState::Muted : TrackState::Playing;
      trackChanged(selected);
    }
  }
  if (toggleTransport) {
    if (transport.running) transportStop();
    else transportStart();
  }
  // Show the result right away rather than on the rows' next redraw: the new
  // selection first, then the old one, so highlight and icon move together.
  if (sceneChanged)
    for (int i = 0; i < ROWS; i++) drawTrack(i);
  drawTrack(selected);
  if (prevSel != selected) drawTrack(prevSel);
}

// After a recording finishes on the selected track, move on: to the next
// empty track (in order, wrapping), else the next muted one, else stay.
static void selectAfterRecording(int t) {
  if (selected != t) return;                       // the user has moved on already
  for (TrackState want : {TrackState::Empty, TrackState::Muted}) {
    for (int k = 1; k < NUM_TRACKS; k++) {
      int i = (t + k) % NUM_TRACKS;
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
  // A recording that ended with a clip (not cancelled): select the next track.
  static int recTrack = -1;
  if (recording.mode != Recording::Idle) {
    recTrack = recording.track;
  } else if (recTrack >= 0) {
    if (tracks[recTrack].state != TrackState::Empty) selectAfterRecording(recTrack);
    recTrack = -1;
  }
}
