// input.cpp — see input.h. The key list is at the top of L7.ino.
#include "input.h"
#include "audio_follow.h"
#include "ui.h"
#include "ableton.h"
#include "usb_drive.h"
#include "midi_in.h"

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;   // held arrows
static constexpr uint32_t DOUBLE_TAP_MS = 200;      // two taps of a key this close: a double tap
static constexpr uint32_t TRIPLE_TAP_MS = 200;      // SHIFT+UP/DOWN's three quick taps: each this close to the last
static constexpr uint32_t EDGE_HOLD_MS = 350;       // SHIFT+UP/DOWN's second tap held this long: to the list's end

// The selected track's current cell back to its defaults: the whole loop as
// the range, the filter open, volume at unity and balance centred.
static void resetCell() {
  if (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == CLOCK_CELL) {   // the master volume: back to 100%
    stepMasterVolume(MASTER_VOLUME_UNITY - masterVolume());   // (and the balance centred)
    stepMasterBalance(-masterBalance());
    return;
  }
  if (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == TEMPO_CELL) {   // the tempo: the scene's saved one
    float b = sceneSavedTempo();
    if (b > 0) setTempo(b);
    return;
  }
  if (selected == STATUS_ROW || !hasClip(selected)) return;
  const Track &tr = tracks[selected];
  if (selectedCell == WAVE_CELL) {
    setRange(selected, 0, tr.loopBars * BEATS_PER_BAR);
    rotateTrack(selected, -(int32_t)unitOf(selected).rotate, -(int32_t)unitOf(selected).steps);   // (and its offsets: none)
  } else if (selectedCell == FILTER_CELL) {
    setTrackFilter(selected, 0, FILTER_MAX);
    trackChanged(selected);
  } else if (selectedCell == MIX_CELL) {
    setTrackMix(selected, VOLUME_UNITY, 0);
    trackChanged(selected);
  }
}

// Tap tempo (A on the status row): from the second tap on, the tempo is the
// least-squares fit of every tap in the run (tap n at beat n), so each tap
// refines it from the 3rd tap on (two intervals), a long series evening out the
// jitter of single taps. Taps further apart than TAP_TIMEOUT_MS start a new
// run, and so does any other key press in between. The 5th tap lands on a
// bar line: a stopped clock starts there, from 1.1.1 (running, it's left as it
// is: no snapping to the taps). That 5th tap isn't part of the estimate: a
// fresh one starts from it.
static constexpr uint32_t TAP_TIMEOUT_MS = 1100;     // a little over a beat at the slowest tapped tempo (60 BPM)

static int tapCount = 0;                            // taps in this run (0: the next is the first)


static uint32_t tapAt = 0;                          // millis() of the latest tap (taken)

// Tapping while playing also lines the clock up with the taps (the run's
// first tap its "one", a bar's first beat): each tap from the 3rd on measures
// how far the clock's bar is from the taps' and eases it there, the tempo
// PHASE_RATE faster or slower until it has caught up (phaseService()); the
// tempo set by the taps meanwhile, and again after.
static constexpr float PHASE_RATE = 0.06f;           // 6% faster / slower while catching up
static int phaseDir = 0;                            // 1 faster, -1 slower, 0 none
static uint32_t phaseEnd = 0;                       // millis() it's caught up by
static void phaseAlign(double msPerBeat, double firstMs) {
  if (!transport.running || syncSource() != SYNC_NONE) return;
  noInterrupts();
  double pos = (double)(transport.beat - transportOrigin()) + (double)transport.sampleInBeat / transport.spb;   // (the clock, in beats)
  interrupts();
  double want = (millis() - firstMs) / msPerBeat;   // (the taps', in beats from the run's first: its "one")
  double e = fmod(pos - want, BEATS_PER_BAR);       // (the clock ahead: positive; within half a bar either way)
  if (e < 0) e += BEATS_PER_BAR;
  if (e >= BEATS_PER_BAR / 2.0) e -= BEATS_PER_BAR;
  if (fabs(e) < 0.01) { phaseDir = 0; return; }
  phaseDir = e > 0 ? -1 : 1;                        // (ahead: slower)
  phaseEnd = millis() + (uint32_t)(fabs(e) * msPerBeat / PHASE_RATE);
}
static void phaseService() {
  if (!phaseDir) return;
  if (!transport.running || tempoNudging() || (int32_t)(millis() - phaseEnd) >= 0) {
    phaseDir = 0;
    if (transport.running && !tempoNudging()) transport.pendingSpb = spbForBpm(bpm);   // caught up: the tempo set
    return;
  }
  uint32_t spb = (uint32_t)lroundf(spbForBpm(bpm) / (1 + phaseDir * PHASE_RATE));
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  if (spb != now) transport.pendingSpb = spb;
}
uint32_t lastTapMs() { return tapAt; }

// Following audio (its tempo and beat heard, not tapped): taps say where its
// bar's first beat is instead, the run's first tap "one". From the 2nd tap,
// once two taps agree the clock's bar is whole beats off, audio follow eases
// it there (audioFollowShift()); the run starts again.
static void tapBar() {
  static uint32_t lastTap = 0;
  static int taps = 0, lastK = 0;
  uint32_t now = millis();
  if (taps > 0 && now - lastTap > TAP_TIMEOUT_MS) taps = 0;
  lastTap = now;
  if (!transport.running || audioFollowShifting()) { taps = 0; return; }
  noInterrupts();
  double pos = (double)(transport.beat - transportOrigin()) + (double)transport.sampleInBeat / transport.spb;
  interrupts();
  double e = fmod(pos - taps, BEATS_PER_BAR);       // (this tap: beat `taps` of the bar; the clock ahead: positive)
  if (e < 0) e += BEATS_PER_BAR;
  if (e >= BEATS_PER_BAR / 2.0) e -= BEATS_PER_BAR;
  int k = (int)lround(e);                           // (whole beats: within a beat, audio follow has it)
  if (taps >= 1 && k == lastK && k != 0) {
    audioFollowShift(k);
    taps = 0;
    return;
  }
  lastK = k;
  taps++;
}

static void tapTempo() {
  if (audioFollowOn()) {                            // (the tempo heard: the taps place its bar, and help find their beat)
    tapAt = millis();
    audioFollowTapHint();
    tapBar();
    return;
  }
  tapAt = millis();
  static uint32_t first, last;                      // this run's first and latest taps
  static double sumT, sumIT;                        // over the taps: t (ms after the first), i * t
  uint32_t now = millis();
  if (tapCount > 0 && now - last > TAP_TIMEOUT_MS) tapCount = 0;
  // Stopped, the 5th tap starts the clock: not part of the estimate so far
  // (left as it was); a fresh one starts from it (it the new run's first).
  const bool starts = tapCount == 4 && !transport.running;
  if (starts) {
    last = now;
    tapCount = 0;
    transportStart();                               // (a bar line now)
  }
  if (tapCount == 0) {
    first = now;
    sumT = sumIT = 0;
  }
  double i = tapCount, t = now - first;
  sumT += t;
  sumIT += i * t;
  last = now;
  int n = ++tapCount;
  if (n >= 3) {                                     // (the tempo from the 3rd tap on: two intervals at least)
    // The slope of t over i (ms per beat), with i = 0 .. n-1:
    // sum(i) = n(n-1)/2, n sum(i^2) - sum(i)^2 = n^2(n^2-1)/12.
    double sumI = n * (n - 1) / 2.0;
    double msPerBeat = (n * sumIT - sumI * sumT) / (n * (double)n * ((double)n * n - 1) / 12);
    if (msPerBeat > 0) setTempo(max(60.0f, (float)(60000.0 / msPerBeat)));   // (tapped: 60 BPM at least)
    if (msPerBeat > 0) {                            // playing: the clock eased onto the taps' bar
      double a = (sumT - msPerBeat * sumI) / n;     // (the fit's first tap, ms after the real first)
      phaseAlign(msPerBeat, first + a);
    }
  }
  // Stopped, the metronome on: each of the first four taps clicks (the
  // first as a bar's first beat); the 5th starts the clock, which clicks itself.
  if (!transport.running && metronomeOn && n < 5) metronomeClick(n == 1);
}

// The clock started by hand (START, A on the clock cell; SHIFT: from 1.1.1
// again): on the beat heard while audio follow has one (audio_follow.h).
static void startByHand() {
  if (!audioFollowStart()) transportStart();
}
static void restartByHand() {
  if (!audioFollowLocked()) return transportRestart();
  transportStop();
  startByHand();
}
// SHIFT+START: stopped, from where the clock last stopped (a project loaded
// since: 1.1.1); running, from 1.1.1 again.
static void resumeByHand() {
  if (transport.running) return restartByHand();
  const int32_t from = transportResumeBeat();
  if (!audioFollowStart(from)) transportStartAt(from);
}

// With SHIFT and B both held, which came first: A then pastes a scene (B
// first) or cuts it (SHIFT first).
// The loaded project exported as a Live Set (the menu's first list item),
// its progress and then where it went shown in the item's place.
static void exportToAbleton() {
  bool wasRunning = transport.running;
  if (wasRunning) transportStop();                   // (the export wants the clock stopped, and the card to itself)
  int p = projectCurrent();
  int n = exportProject(p, [](int done, int total) {
    char b[24];
    snprintf(b, sizeof b, "EXPORTING %d/%d", done, total);
    menuExportStatus(b);
  });
  char b[24];
  if (n) {
    int head = snprintf(b, sizeof b, "SAVED AS ");
    snprintf(b + head, sizeof b - head, "%s %c%02d", APP_NAME, 'A' + p, n);
    menuExportStatus(b, head);                       // (the name in the project's colour)
  } else {
    snprintf(b, sizeof b, "EXPORT FAILED");
    menuExportStatus(b);
  }
  if (wasRunning) transportStart();                  // (running before: again, from 1.1.1)
}

static bool optFirst = false;
bool optBeforeShift() { return optFirst; }
// A pressed while B was held (B then A: the clip's fine rotate, moving a track
// with a clip; A first: a swipe).
static bool editAfterOpt = false;
// The fine rotate while its keys are held: turned this far more (frames) on
// track rotateT, shown (the waveform drawn, the ms) but not heard until a key
// of the combo (B, A or the arrow) is let go: then applied (rotateTrack()).
// While held and playing, what's turned so far is heard at each quarter beat
// crossing (serviceRotate()).
static int32_t rotateBy = 0;
static int32_t rotateStepsBy = 0;                  // (of it, whole quarter beats: the step offset; taps)
static int rotateT = -1;
static uint8_t rotateKey = 0;
static bool rotateHeld = false;                    // (its keys still held)
static int32_t rotateQ = 0;                        // (the quarter beat last seen: a crossing, what's turned heard)
int32_t clipRotatePending(int t) { return t == rotateT ? rotateBy : 0; }
int32_t clipRotatePendingSteps(int t) { return t == rotateT ? rotateStepsBy : 0; }
static int32_t quarterNow() {
  noInterrupts();
  int32_t q = transport.beat * 4 + (int32_t)((uint64_t)transport.sampleInBeat * 4 / transport.spb);
  interrupts();
  return q;
}
// A turn as signed frames: past half the loop, as earlier.
static int32_t signedTurn(int64_t f, int64_t loop) {
  int64_t r = (f % loop + loop) % loop;
  return (int32_t)(r > loop / 2 ? r - loop : r);
}
// A nudge step on the track being turned: dir 1 later, -1 earlier, by
// frames (the clip's own); reset: back to none. Reaching none (or passing it,
// either way): there, the held arrow's steps stopped. A tap (let go before
// the arrows' repeats start): a quarter beat; held instead: a repeat five
// of 1 ms, five of 2, five of 4, then 10 ms each.
static bool rotateStepped = false;                 // (the press's steps begun, or reset: no tap's quarter)
static bool rotateStopped = false;                 // (at none: the held arrow's steps stopped)
static uint32_t stepAt = 0;                        // (millis() of the held arrow's next step; 0: none)
static int heldSteps = 0;                          // (the held arrow's steps so far: five of 1 ms, five of 2, five of 4, then 10 ms each)
// The tempo cell's ms flashed white a moment: as the held arrow's ms steps
// start, and as a tap's quarter beat is taken (let go).
static constexpr uint32_t ROTATE_FLASH_MS = 100;
static uint32_t rotateFlashAt = 0;
uint32_t sampleSteppedAt = 0;                       // (millis() of the last SHIFT+LEFT/RIGHT sample step: its label flashed)
uint32_t stepFlashAt = 0;                           // (millis() of the last step offset tap: its track's start point solid white a moment)
int stepFlashTrack = -1;
bool clipRotateFlash() { return rotateFlashAt && millis() - rotateFlashAt < ROTATE_FLASH_MS; }
static int32_t msFrames(const Track &tr, int ms) {
  return (int32_t)((uint64_t)tr.spbNative * (uint32_t)(AUDIO_SAMPLE_RATE_EXACT * ms / 1000) / (transport.spb ? transport.spb : 1));
}
// steps: whole quarter beats (a tap: the step offset, wrapping round the
// loop); frames: the ms offset (held), stopping at none; reset: both to none.
static void nudgeStep(int dir, bool reset, int32_t frames = 0, int32_t steps = 0) {
  if (rotateT < 0) {
    if (!reset) return;
    rotateT = selected;                            // (both arrows at once: the selected track's)
    rotateHeld = true;
    rotateQ = quarterNow();
  }
  const Track &tr = tracks[rotateT];
  const int64_t loop = tr.loopFrames();
  if (loop <= 0) return;
  const TrackUnit u = unitOf(rotateT);
  const int32_t qf = (int32_t)(tr.spbNative / 4), stepsNow = u.steps + rotateStepsBy;
  if (reset) {
    rotateBy -= signedTurn((int64_t)u.rotate + rotateBy, loop);
    rotateStepsBy -= stepsNow;
    rotateStepped = true;
  } else if (steps) {                              // (a tap: a quarter beat, the step offset)
    rotateStepsBy += dir * steps;
    rotateBy += dir * steps * qf;
  } else {                                         // (held: the ms offset, stopping at none)
    const int32_t now = signedTurn((int64_t)u.rotate + rotateBy - (int64_t)stepsNow * qf, loop);
    int32_t by = dir * frames;
    if (now != 0) {
      int32_t next = signedTurn((int64_t)now + by, loop);
      if (next == 0 || ((now < 0) != (next < 0) && llabs((int64_t)next - now) < loop / 2)) {
        by = -now;                                 // (to none: there)
        rotateStopped = true;
      }
    }
    rotateBy += by;
  }
  drawTrack(rotateT);
}

// What's turned goes to the track (rotateTrack(): queued as a range, switched
// to on a quarter beat with the new data buffered) at each quarter beat
// crossing while held and playing, and once let go; a change on its way
// meanwhile: again until taken.
static void serviceRotate() {                      // (every pass: pollKeys())
  if (rotateT < 0) return;
  if (rotateHeld && !rotateStopped && (int32_t)(millis() - stepAt) >= 0 && (mainKeys() & rotateKey)) {   // held: five 1 ms repeats, five 2, five 4, then 10 ms
    stepAt += REPEAT_PERIOD_MS;
    rotateStepped = true;
    if (++heldSteps == 1) rotateFlashAt = max(1u, millis());
    int ms = heldSteps <= 5 ? 1 : heldSteps <= 10 ? 2 : heldSteps <= 15 ? 4 : 10;
    nudgeStep(rotateKey & m8::BTN_RIGHT ? 1 : -1, false, msFrames(tracks[rotateT], ms));
  }
  int32_t q = quarterNow();
  bool crossing = q != rotateQ;
  rotateQ = q;
  if ((rotateBy || rotateStepsBy) && (!rotateHeld || (crossing && transport.running)) && rotateTrack(rotateT, rotateBy, rotateStepsBy)) rotateBy = rotateStepsBy = 0;
  if (!rotateHeld && !rotateBy && !rotateStepsBy) {
    drawTrack(rotateT);
    rotateT = -1;
    rotateKey = 0;
  }
}
// B then A held on a track (A pressed while B was held): its selection frame
// on its waveform cell, in black (ui.h cellFrame()).
bool optThenEdit() {
  uint8_t k = mainKeys();
  return editAfterOpt && (k & m8::BTN_OPT) && (k & m8::BTN_EDIT) && selected != STATUS_ROW && !menuOpen();
}
// B then A held on a track with a clip, any of its cells: its fine rotate (the arrows turn it).
bool clipRotating() {
  uint8_t k = mainKeys();
  return editAfterOpt && (k & m8::BTN_OPT) && (k & m8::BTN_EDIT) && selected != STATUS_ROW && !menuOpen() &&
         hasClip(selected);                         // (from any of its cells)
}

// The last arrow press moved the selection (not: already at the edge that way).
static bool moved = false;
bool arrowsMoved() { return moved; }

// A or B down on a menu item: its action waits for the keys to be let go.
static bool armed = false, shifted = false;          // SHIFT with it
static int armedSlot = -1;                           // the slot it was pressed on (-1: a list item)
static int armedItem = -1;                           // or the list item
static bool loadedOnPress = false;                   // its press loaded the card's project: held on, the menu closes
static bool openOnLetGo = false;                     // pressed on the loaded project's card or an empty slot: to the looper once let go or held
static uint32_t armedAt = 0;
static uint32_t cardPressAt = 0;                     // millis() of the latest A or B press on a card
uint32_t menuCardPressMs() { return cardPressAt; }
static constexpr uint32_t LONG_PRESS_MS = 600;

static bool nextSceneArmed = false;               // A pressed on the scene cell: the next scene once it's let go
// UP / DOWN between the tracks and the status row: each keeps its own column,
// the one it was last left on (-1: none yet; the nearest then).
static int trackColumn = -1, statusColumn = -1;
// The selection just moved to another row (`selected`) from row `from`, on its
// cell `at`: to or from the status row, the column that kind of row was left on.
static void keepColumns(int from, int at) {
  if (selected == STATUS_ROW && from != STATUS_ROW) {
    trackColumn = selectedCell;                    // (the column kept, as on an empty track)
    selectedCell = statusColumn >= 0 ? statusColumn : min(selectedCell, CELLS - 1) + 1;
  } else if (from == STATUS_ROW && selected != STATUS_ROW) {
    statusColumn = at;
    selectedCell = trackColumn >= 0 ? trackColumn : max(at - 1, 0);
  }
}
// Swipe to mute / solo: A pressed on a track with a clip mutes or unmutes it;
// still held, UP / DOWN (each tap, and a held one's repeats) step a track up /
// down (wrapping from the top track to the bottom one and back, not onto the
// status row) and do the same to each track stepped onto: muting, each one
// heard is muted but the last one heard, where it stops (a solo); unmuting,
// each muted one is unmuted, and it stops on the track before the one it
// started from (round again).
// Empty tracks (and one recording) are stepped over as they are. Stopped,
// UP / DOWN do nothing more until A is let go.
static bool swipeOn = false, swipeMute = false;
static int swipeFrom = -1, swipeDir = 0, swipePos = 0;   // (the first step's way, in track indices; steps taken that way)
static bool swipeWas[NUM_TRACKS];                  // each track's mute when A was pressed (undoing goes back to it)

// An arrow held that switches screens when let go: into the menu, or out of
// it; or between the looper's two (main and notes, ui.h: UP off its top, DOWN
// off its bottom).
static uint8_t menuOnRelease = 0, leaveOnRelease = 0, flipOnRelease = 0;
static bool nextChosen = false;                    // (a recording's stop pressed: the next track chosen then, not again once it's saved)
static void selectAfterRecording(int t, bool fromScene = false);
static bool dupJustMade = false;                   // (SHIFT+A duplicated the scene, SHIFT still held: A again clears it)
bool sceneDupMade() { return dupJustMade && (mainKeys() & m8::BTN_SHIFT); }
static uint8_t helpOnRelease = 0;                  // (UP or DOWN in the help pages: back to the menu, the same way)
static bool stopRepeat = false;                    // (pollKeys(): that arrow's repeats off)
// Or held on SWITCH_HOLD_MS (from when it stopped at the edge): it switches
// then, still held (pollKeys()); held on, it does nothing more (no repeats)
// until let go.
static constexpr uint32_t SWITCH_HOLD_MS = 600;
static uint32_t switchWaitAt = 0;                        // (when the switch began waiting)
static bool edgeHeldLong = false;                  // (this pass: held that long, it switches now)
static bool edgeDue(uint8_t keys, uint8_t key) {
  if (!key || ((keys & key) && !edgeHeldLong)) return false;
  board.tft().setClipRect();                       // (the edge line's clip off: the other screen drawn whole, ui.cpp drawEdgeLine())
  return true;
}
// LEFT+RIGHT pressed together (the second within LR_WINDOW_MS of the first,
// nothing else held) and held LR_HOLD_MS: the other screen (switchHold()).
// From the second one's press until both are let go, they don't move the
// selection (the first one's step: undone by the second's, or it stopped at
// an edge).
static bool lrHeld = false, lrDone = false;
static uint32_t lrAt = 0, leftAt = 0, rightAt = 0;   // (the combo's start; each arrow's last fresh press)
static constexpr uint8_t LR = m8::BTN_LEFT | m8::BTN_RIGHT;
static constexpr uint32_t LR_WINDOW_MS = 200, LR_HOLD_MS = 300;
bool screenSwitchPending() { return menuOnRelease || leaveOnRelease || flipOnRelease || helpOnRelease || (lrHeld && !lrDone); }
uint8_t screenSwitchKey() { return menuOnRelease | leaveOnRelease | flipOnRelease | helpOnRelease | (lrHeld && !lrDone ? LR : 0); }

// Loads project slot p from the menu; a new project (an empty slot) opens
// with the selection on track 1 (the looper's, for when the menu closes);
// another keeps the selection as it is.
// A project with no clips (a new one too) comes in at once, not on the next
// bar line: the clock (running) stopped, the project loaded (at once, the
// clock stopped), and started again from 1.1.1 (as a load restarts it anyway).
static bool loadFromMenu(int p, bool keepSelection = false) {   // (keepSelection: a new one too keeps the looper's selection)
  bool fresh = !projectFilled(p);
  int from = projectCurrent();
  bool now = p != from && transport.running && recording.mode == Recording::Idle && !projectHasClipsIn(p);
  if (now) transportStop();
  if (!projectLoad(p)) {
    if (now) transportStart();
    return false;
  }
  if (now) startByHand();
  menuNoteLoad(from);                              // (its card and the old one's take turns until it comes in)
  if (fresh && !keepSelection) setMainSelection(NUM_TRACKS - 1, WAVE_CELL);
  metronomeOn = metronomeWanted();                 // (now: its first beat clicks too)
  clickFromBeat = sceneSwitchBeat();               // (playing: silent until the switch)
  return true;
}

// Out of the menu, back to the looper (as A on the loaded project's card);
// with no project loaded, a new one is made in the first empty slot (the
// menu's selection staying where it was), as A there. False: nowhere to go.
// The looper's selection: as it was left (its row and cell); off the menu's
// edge with arrow `key`, on the looper's far edge: UP off the menu's top, its
// bottom row (the status row), DOWN off its bottom, its top one (track 7), at
// the column that row was left on; RIGHT off the menu's right side, the
// looper's left edge (its row's first cell), LEFT, its right edge (its last),
// in the row it was left on.
static bool leaveMenu(uint8_t key = 0) {
  menuNoteLeave(false);                              // (back in: where the arrow lands)
  bool back = projectCurrent() >= 0;
  if (!back) {
    int slot = menuFirstEmptySlot();
    if (slot < 0) return false;
    if (loadFromMenu(slot, true)) back = true;       // (the menu's selection stays where it was: its column kept; the looper's too)
  }
  if (back) {
    closeMenu(false);                                // (drawn below, where it lands)
    if (key == m8::BTN_UP || key == m8::BTN_DOWN) {
      int from = selected, at = selectedCellIn(selected);
      selected = rowAtPos(key == m8::BTN_UP ? ROWS - 1 : 0);
      keepColumns(from, at);
    } else if (key == m8::BTN_RIGHT) {
      selectedCell = firstCellIn(selected);
    } else if (key == m8::BTN_LEFT) {
      selectedCell = maxCells(selected) - 1;        // (an empty track: the column kept as on a full one)
    }
    for (int i = 0; i < ROWS; i++) drawTrack(i);
  }
  return back;
}

// Back to the looper from the menu: at once, or, a project loaded from it
// still on its way (menuLoadPending(): playing, until its switch point),
// once it has come in (closeWhenIn, followed by menuHold()).
static bool closeWhenIn = false;
static void closeToLooper() {
  if (menuLoadPending()) {
    closeWhenIn = true;
    return;
  }
  closeWhenIn = false;
  closeMenu();
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}

// A or B let go (or held) on the loaded project's card: back to the looper
// (one on its way: once it has come in); on an empty slot: a new project there,
// opened (on track 1's waveform cell; the loaded one's: the clock cell). False: nothing done.
static bool openCard(int slot) {
  openOnLetGo = false;
  menuNoteLeave(true);                               // (back in: on the loaded project's card)
  if (slot == projectCurrent()) {
    setMainSelection(STATUS_ROW, CLOCK_CELL);      // (from a card: the clock cell selected)
    closeToLooper();
    return true;
  }
  if (projectFilled(slot) || !loadFromMenu(slot)) return false;
  setMainSelection(NUM_TRACKS - 1, WAVE_CELL);      // (a new, empty project: track 1's waveform cell, to record)
  statusColumn = CLOCK_CELL;                       // (and the status row's the clock cell: DOWN from track 1 lands on it)
  closeMenu();
  return true;
}

// A or B held on a card whose project its press loaded: after LONG_PRESS_MS,
// the menu closes too, and the clock starts if it was stopped (from 1.1.1,
// as START). Called every pass (pollKeys()).
// On the loaded project's card or an empty slot (openOnLetGo): nothing on the
// press; let go, to the looper (openCard()); held LONG_PRESS_MS, to the looper
// and the clock started the same way.
// SHIFT+A (or B) held on a card with a project: cut once held CUT_HOLD_MS
// (let go sooner: nothing); meanwhile its card wipes away from the left
// (menuCutProgress(), ui_menu.cpp).
static constexpr uint32_t CUT_HOLD_MS = 1000;
static bool justCut = false;                       // a project just cut by holding: its card's letter not in its colour until the keys are let go
bool menuJustCut() { return justCut; }
float menuCutProgress(int &slot) {
  slot = armed && shifted && armedSlot >= 0 && menuOpen() && projectFilled(armedSlot) && (board.keys() & m8::BTN_SHIFT) ? armedSlot : -1;
  return slot < 0 ? 0 : min(1.0f, (millis() - armedAt) / (float)CUT_HOLD_MS);
}

static void menuHold() {
  if (justCut && !(board.keys() & (m8::BTN_EDIT | m8::BTN_OPT | m8::BTN_SHIFT))) {
    justCut = false;                               // (all let go: its letter as an empty slot's again)
    drawMenu();
  }
  if (armed && shifted && armedSlot >= 0 && menuOpen() && projectFilled(armedSlot) && !(board.keys() & m8::BTN_SHIFT)) {
    armed = false;                                 // SHIFT let go: the cut's countdown cancelled (A let go: as ever)
    drawMenu();
    return;
  }
  int cutSlot;
  if (menuCutProgress(cutSlot) >= 1) {             // held long enough: cut
    armed = false;                                   // (letting go: nothing more)
    justCut = true;
    menuCutFlash(cutSlot);                         // (its flash, before the cut: as long as it should be)
    projectCut(cutSlot);
    drawMenu();
    for (int i = 0; i < ROWS; i++) drawTrack(i);    // (the loaded one cut: the looper emptied)
    return;
  }
  if (armed && openOnLetGo && menuOpen() && millis() - armedAt >= LONG_PRESS_MS) {
    armed = false;                                   // (letting go: nothing more)
    if (openCard(armedSlot) && !transport.running) startByHand();
    return;
  }
  if (!menuOpen()) closeWhenIn = false;
  if (closeWhenIn && !menuLoadPending()) closeToLooper();   // (come in: now)
  if (!armed || !loadedOnPress || !menuOpen() || millis() - armedAt < LONG_PRESS_MS) return;
  loadedOnPress = false;
  armed = false;                                     // (letting go: nothing more)
  menuNoteLeave(true);                               // (back in: on the loaded project's card)
  setMainSelection(STATUS_ROW, CLOCK_CELL);        // (from a card: the clock cell selected)
  closeToLooper();
  if (!transport.running) startByHand();
}

// The selected row and the rows sharing its borders redrawn (the frame hidden
// or back, a screen switch pending or not); in the menu, the menu.
static void drawPending() {
  if (menuOpen()) {
    drawMenu();
    return;
  }
  drawTrack(selected);
  for (int r : {rowAbove(selected), rowBelow(selected)})
    if (r >= 0 && r < NUM_TRACKS) drawTrack(r);
}

// LEFT+RIGHT held long enough: the menu opens, or closes (leaveMenu(), the
// looper's selection as it was; a project loaded from it still on its way:
// once it has come in). Called every pass (pollKeys()).
static void switchHold() {
  if (!lrHeld || lrDone || millis() - lrAt < LR_HOLD_MS) return;
  lrDone = true;
  if (!menuOpen()) {
    openMenu();
  } else if (menuLoadPending()) {
    menuNoteLeave(false);
    closeWhenIn = true;
    drawPending();
  } else if (!leaveMenu()) {
    drawPending();
  }
}

FLASHMEM static void onKeys(uint8_t keys, uint8_t pressed, bool repeat) {   // (flash: on key presses only; RAM1 is full)
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  // The fine rotate's keys: one let go, what was shown is heard.
  if (rotateHeld && (!(keys & m8::BTN_EDIT) || !(keys & m8::BTN_OPT) || !(keys & rotateKey))) {
    if (!rotateStepped && rotateKey) { stepFlashAt = max(1u, millis()); stepFlashTrack = rotateT; }   // (a tap: its start point solid white a moment, not the ms)
    if (!rotateStepped && rotateKey) nudgeStep(rotateKey & m8::BTN_RIGHT ? -1 : 1, false, 0, 1);   // (a tap: a quarter beat, the step offset; RIGHT earlier: the grid moves right)
    rotateHeld = false;
    serviceRotate();
  }
  // The arrows' modifier (exclusive: the one pressed last; ui.h arrowModifier()).
  const bool shiftA = arrowModifier() == m8::BTN_SHIFT, optA = arrowModifier() == m8::BTN_OPT;
  if (pressed & m8::BTN_OPT) optFirst = !(shift && !(pressed & m8::BTN_SHIFT));
  if (pressed & m8::BTN_SHIFT) optFirst = opt && !(pressed & m8::BTN_OPT);
  if (pressed & m8::BTN_EDIT) editAfterOpt = opt && !(pressed & m8::BTN_OPT);
  // LEFT+RIGHT (switchHold()): the second one pressed within LR_WINDOW_MS of
  // the first, with only it down; any other key meanwhile, or one let go: off
  // (once done, the arrows still do nothing until both are let go).
  if (!repeat && (pressed & m8::BTN_LEFT)) leftAt = millis();
  if (!repeat && (pressed & m8::BTN_RIGHT)) rightAt = millis();
  if (lrHeld && ((keys & ~LR) || (keys & LR) != LR)) {
    lrHeld = false;
    if (!lrDone) drawPending();
  }
  if (!lrHeld && (pressed & LR) && keys == LR && !repeat &&
      (leftAt > rightAt ? leftAt - rightAt : rightAt - leftAt) <= LR_WINDOW_MS) {
    lrHeld = true;
    lrDone = false;
    lrAt = millis();
    bool stepped = moved;                            // (the first one moved the selection: this one steps back)
    if (menuOnRelease || leaveOnRelease || flipOnRelease) stepped = false;   // (it stopped at an edge, a switch waiting: that's off)
    menuOnRelease = leaveOnRelease = flipOnRelease = 0;
    if (!stepped) pressed &= ~LR;
    else repeat = true;                              // (stepping back: no switch off this edge)
    drawPending();
  } else if (lrHeld || (lrDone && (keys & LR))) {
    pressed &= ~LR;
  }
  if (lrDone && !(keys & LR)) lrDone = false;
  // UP off the looper's top row, DOWN off its bottom one (a fresh press, not
  // a held arrow repeating): its other screen (ui.h) once that key is let go,
  // on its far edge's row (UP: its bottom one, DOWN: its top; from a status
  // row, the track next to the other's), the column kept
  // (the tracks' and the status rows' each their own, both screens' status
  // rows the same).
  if (edgeDue(keys, flipOnRelease)) {
    uint8_t key = flipOnRelease;
    flipOnRelease = 0;
    int from = selected, at = selectedCellIn(selected);
    notesScreen = !notesScreen;
    notesLayout = notesScreen;
    // (From a status row, the two status rows meeting: the track next to it,
    // DOWN into notes its first, UP into main its last.)
    int edge = from == STATUS_ROW ? 1 : 0;
    selected = rowAtPos(key == m8::BTN_UP ? ROWS - 1 - edge : edge);   // (in its layout)
    keepColumns(from, at);
    board.tft().fillScreen(BG);
    uiGeneration++;                                  // (everything in full)
    for (int i = 0; i < ROWS; i++) drawTrack(i);
    return;
  }
  // Any arrow off the looper's edge (LEFT off a row's first cell, RIGHT off
  // its last, UP off its top row, DOWN off its bottom one; a fresh press, not a
  // held arrow repeating): the menu opens once that key is let go (the clock
  // goes on).
  // (menuOnRelease: the key to wait for, file scope: screenSwitchPending())
  if (edgeDue(keys, menuOnRelease)) {
    uint8_t key = menuOnRelease;
    menuOnRelease = 0;
    openMenu(key);                                   // (on its far edge: its own column or row)
    return;
  }
  // Any arrow off the menu's edge (menuLeavesOn()):
  // back to the looper (the screen it came from) once that key is let go, as A on the
  // loaded project's card; with no project loaded, a new one is made in the
  // first empty slot (the menu's selection staying where it was), as A there.
  // (leaveOnRelease: likewise)
  // UP or DOWN in the help pages: back to the menu, likewise (the edge line meanwhile).
  if (edgeDue(keys, helpOnRelease)) {
    helpOnRelease = 0;
    menuHelp(false);
    return;
  }
  if (edgeDue(keys, leaveOnRelease)) {
    uint8_t key = leaveOnRelease;
    leaveOnRelease = 0;
    if (menuLeavesOn(key)) leaveMenu(key);
    return;
  }
  for (uint8_t key : {m8::BTN_RIGHT, m8::BTN_UP, m8::BTN_DOWN, m8::BTN_LEFT})
    if (menuLeavesOn(key) && (pressed & key) && !repeat && !(keys & (m8::BTN_SHIFT | m8::BTN_OPT | m8::BTN_EDIT | m8::BTN_PLAY))) {
      leaveOnRelease = key;                          // (once let go, or held SWITCH_HOLD_MS: pollKeys())
      return;
    }
  // The menu's items: the arrows move its selection; A or B runs the action
  // button's action once let go (on the item it was pressed on); START starts /
  // stops the clock, as in the looper's view. The menu doesn't
  // start or stop the clock.
  // SHIFT+A on a project: cuts it once held CUT_HOLD_MS (its card wiping away meanwhile; let go sooner: nothing). A on a card,
  // at once on the press: its project loaded, the menu staying open (held
  // LONG_PRESS_MS: it closes too, menuHold(); let go, the two cards take turns
  // as the loaded one until it comes in: menuNoteLoad()); on an empty slot (once
  // let go, or held LONG_PRESS_MS: the clock started too): a new project made
  // and opened, the menu closing; on the loaded project's card (likewise):
  // the menu closes. While a project loaded here is on its way (playing:
  // until its switch point) the menu stays: closing waits for it
  // (closeToLooper()), and no edge leads out (menuLeavesOn()). The list's
  // items act once let go. A and B do the same
  // everywhere in the menu.
  if (!menuOpen()) armed = false;
  if (menuOpen() && !menuOnStatus()) {
    if (pressed & m8::BTN_PLAY) {                    // START: the clock, as in the looper's view (SHIFT: resume, or from 1.1.1)
      if (shift) resumeByHand();
      else if (transport.running) transportStop();
      else startByHand();
    }
    const uint8_t act = m8::BTN_EDIT | m8::BTN_OPT;
    if (pressed & act) noteActionPress();            // (the icon: what letting go will do)
    if (menuHelpOpen() && !armed) {                  // the help pages: LEFT / RIGHT turn them, A or B back to the menu; UP or
      if (pressed & act) menuHelp(false);          // DOWN too, once let go (or held SWITCH_HOLD_MS), its edge line meanwhile
      else if ((pressed & (m8::BTN_UP | m8::BTN_DOWN)) && !repeat) helpOnRelease = pressed & m8::BTN_UP ? m8::BTN_UP : m8::BTN_DOWN;
      else if ((pressed & m8::BTN_LEFT) && helpPage() == 0) { if (!repeat) helpOnRelease = m8::BTN_LEFT; }   // (off the ends: likewise)
      else if ((pressed & m8::BTN_RIGHT) && helpPage() == helpPages() - 1) { if (!repeat) helpOnRelease = m8::BTN_RIGHT; }
      else if (pressed & (m8::BTN_LEFT | m8::BTN_RIGHT)) moved = menuHelpKeys(pressed, repeat);
      return;
    }
    if (menuSettingsOpen() && !armed) {              // the settings popup: A or B turns its item on / off (on the
      if (pressed & act) menuSettingsAct();          // press); the arrows go through it (DOWN off its bottom, LEFT / RIGHT: closed)
      else if (pressed & (m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN)) { moved = menuSettingsKeys(pressed, repeat); if (!menuSettingsOpen()) stopRepeat = true; }   // (closed: the held arrow's repeats not on into the menu)
      return;
    }
    if ((pressed & act) && !armed) {
      armed = true;
      armedSlot = menuSlot();
      armedItem = menuListItem();
      shifted = shift;
      armedAt = millis();
      if (armedSlot >= 0) cardPressAt = armedAt;      // (its frame white a moment: ui_menu.cpp)
      loadedOnPress = false;
      openOnLetGo = false;
      int slot = armedSlot;
      if (slot >= 0 && !shift) {                     // a card: at once, on the press
        openOnLetGo = slot == projectCurrent() || !projectFilled(slot);
        if (openOnLetGo) return;                     // the loaded one, an empty slot: once let go, or held (menuHold())
        closeWhenIn = false;                         // (another one now: the menu stays)
        if (loadFromMenu(slot)) {                    // another: loaded (the looper's selection stays as it is)
          loadedOnPress = true;                      // (held LONG_PRESS_MS: the menu closes too, menuHold())
          drawMenu();
        }
      }
    }
    if (armed && !(keys & (m8::BTN_EDIT | m8::BTN_OPT))) {   // all let go: act
      armed = false;
      int slot = armedSlot;
      if (slot < 0) {                                // the list's items: export, storage mode, the settings, help
        if (armedItem == MENU_EXPORT && !shifted && projectCurrent() >= 0 && projectHasClips() && !menuExportShown()) exportToAbleton();
        if (armedItem == MENU_SETTINGS && !shifted) menuSettings(true);
        if (armedItem == MENU_HELP && !shifted) menuHelp(true);
        if (armedItem == MENU_STORAGE && !shifted) {
          bool wasRunning = transport.running;
          midiInSuspend(true);                       // (no MIDI taken in meanwhile)
          bool back = usbDriveMode();
          midiInSuspend(false);
          if (back) {                                // (back from it: everything read from the card again)
            drawMenu();
            if (wasRunning) transportStart();        // (running before: again, from 1.1.1)
          }
        }
      } else if (openOnLetGo && !shifted) {          // the loaded one, an empty slot: to the looper
        openOnLetGo = false;
        openCard(slot);
        return;
      } else if (shifted) {                          // SHIFT: cut its project, or paste one into it
        if (!projectFilled(slot)) projectPaste(slot);   // (a project: cut only once held, menuHold())
        else drawMenu();                             // (let go too soon: its card whole again)
        drawMenu();
        for (int i = 0; i < ROWS; i++) drawTrack(i);   // (the loaded one cut: the looper emptied)
      }                                              // (a card without SHIFT: done on the press)
      loadedOnPress = false;
      return;
    }
    if (!(pressed & (act | m8::BTN_PLAY))) {
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
  // A on the scene cell, once let go (nothing else pressed meanwhile): the
  // next scene (as SHIFT+UP: playing, on the next bar line; the clock as it is).
  if (nextSceneArmed && (pressed & ~m8::BTN_EDIT)) nextSceneArmed = false;   // (another key: not that)
  if (nextSceneArmed && !(keys & m8::BTN_EDIT)) {
    nextSceneArmed = false;
    if (onStatus && selectedCellIn(STATUS_ROW) == SCENE_CELL && sceneCount() >= 2) {   // (fewer than two scenes: nothing, the button grey)
      if (!transport.running) startByHand();     // (stopped: the clock started, the scene as it is)
      else if (sceneNext())
        for (int i = 0; i < ROWS; i++) drawTrack(i);
    }
  }
  // Swipe (A pressed on a track with a clip: it muted / unmuted): A still
  // held, UP / DOWN step on a track at a time, doing the same to each (with a
  // clip), wrapping round; back on the track it started on, it stops there
  // (held: no more repeats). The other way from the first step's: each step
  // undoes one (that track's mute as it was when A was pressed), stopping
  // back on the start (its own press kept: nothing further that way).
  if (swipeOn && !(keys & m8::BTN_EDIT)) swipeOn = false;
  if (swipeOn && (pressed & (m8::BTN_UP | m8::BTN_DOWN))) {
    if (!shift && !opt) {
      int dir = (pressed & m8::BTN_UP) ? NUM_TRACKS - 1 : 1;   // (UP: a row up; the top track's index is 0)
      if (!swipeDir) swipeDir = dir;
      if (dir == swipeDir) {
        if (swipePos < NUM_TRACKS - 1) {
          swipePos++;
          int t = (swipeFrom + swipePos * swipeDir) % NUM_TRACKS;
          selected = t;
          if (hasClip(t)) setTrackMuted(t, swipeMute);
          if (swipePos == NUM_TRACKS - 1) stopRepeat = true;   // (the next: the start again)
        } else {
          selected = swipeFrom;                    // round: stopped on the start
          stopRepeat = true;
        }
      } else if (swipePos > 0) {                   // back: undone
        int t = (swipeFrom + swipePos * swipeDir) % NUM_TRACKS;
        if (hasClip(t)) setTrackMuted(t, swipeWas[t]);
        swipePos--;
        selected = (swipeFrom + swipePos * swipeDir) % NUM_TRACKS;
        if (!swipePos) stopRepeat = true;          // back on the start: stopped there (the press's own kept)
      }
    }
    pressed &= ~(m8::BTN_UP | m8::BTN_DOWN);         // (nothing else: not the menu's edges either)
  }
  // A scene shown before it plays (held, or waiting for its switch point,
  // scenes.h scenePreviewing()): the selection still moves, the scene keys
  // (SHIFT+UP/DOWN, B+UP/DOWN on the scene cell) and START still work;
  // nothing else does (A, B's taps and edits, SHIFT+LEFT/RIGHT).
  const bool previewing = scenePreviewing();
  if (previewing) pressed &= ~(m8::BTN_EDIT | m8::BTN_OPT);
  // A on the status row's scene cell: as B+A (the panel); on the clock cell:
  // as START; on the tempo cell: tap tempo.
  bool onScene = onStatus && selectedCellIn(STATUS_ROW) == SCENE_CELL;
  bool onClock = onStatus && selectedCellIn(STATUS_ROW) == CLOCK_CELL;
  bool onTempo = onStatus && selectedCellIn(STATUS_ROW) == TEMPO_CELL;
  // Any press but a lone A on the tempo cell (a tap): the next tap starts a new count.
  if (pressed && !(onTempo && pressed == m8::BTN_EDIT && keys == m8::BTN_EDIT)) tapCount = 0;
  bool sceneChanged = false;                         // every row to redraw
  // The scene keys' modifier: SHIFT, or B on the status row's scene cell.
  bool sceneMod = shiftA || (optA && onStatus && selectedCellIn(STATUS_ROW) == SCENE_CELL);
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

  // SHIFT+UP/DOWN: newer / older scene, a step a tap; tapped three times
  // quickly (each within TRIPLE_TAP_MS of the last; no hold), the third goes
  // straight to that end (UP: N+1*, DOWN: the empty scene 0*).
  auto sceneKeys = [&] {
    static uint32_t lastTap[2] = {0, 0};
    static int taps[2] = {0, 0};
    const uint8_t keys2[2] = {m8::BTN_UP, m8::BTN_DOWN};
    for (int k = 0; k < 2; k++) {
      if (!(pressed & keys2[k] & ~edgeDone)) continue;
      if (repeat) { taps[k] = 0; sceneChanged |= sceneStep(k == 0 ? 1 : -1); continue; }
      taps[k] = millis() - lastTap[k] < TRIPLE_TAP_MS ? taps[k] + 1 : 1;
      lastTap[k] = millis();
      if (taps[k] >= 3) {
        sceneChanged |= k == 0 ? sceneGoEnd() : sceneGoEmpty();
        taps[k] = 0;
        lastTap[k] = 0;
      } else {
        sceneChanged |= sceneStep(k == 0 ? 1 : -1);
      }
    }
  };

  if (optA && (!previewing || onScene)) {
    // OPT (B) + arrows: the current cell's parameters (none on empty tracks).
    if (onStatus) {
      if (selectedCellIn(STATUS_ROW) == SCENE_CELL) {   // scene: UP/DOWN as SHIFT+UP/DOWN
        sceneKeys();
      } else if (onTempo && (audioFollowOn() || syncSource() != SYNC_NONE)) {
        // Following: UP/DOWN double / halve the tempo (fresh presses only;
        // a MIDI clock's: ours theirs times that, audio_follow.h, midi_in.h);
        // LEFT/RIGHT the clock's offset from the external one, 10 ms a
        // press (both: back to 0).
        int scaleBy = !repeat && (pressed & m8::BTN_UP) ? 1 : !repeat && (pressed & m8::BTN_DOWN) ? -1 : 0;
        if (scaleBy && (syncSource() == SYNC_SERIAL || syncSource() == SYNC_USB)) midiScale(scaleBy);
        else if (scaleBy && audioFollowOn()) audioFollowScale(scaleBy);
        const uint8_t LRK = m8::BTN_LEFT | m8::BTN_RIGHT;
        int src = syncSource();
        if (src != SYNC_NONE && (pressed & LRK)) {
          int16_t &o = syncOffsetMs[src];
          if ((keys & LRK) == LRK) o = 0;
          else o = constrain(o + ((pressed & m8::BTN_RIGHT) ? SYNC_OFFSET_STEP_MS : -SYNC_OFFSET_STEP_MS), -SYNC_OFFSET_MAX_MS, SYNC_OFFSET_MAX_MS);
          syncOffsetChanged(src);
        }
      } else if (onTempo) {                          // tempo: 0.1 BPM steps, or 1 BPM
        if (pressed & m8::BTN_LEFT) setTempo(bpm - 0.1f);
        if (pressed & m8::BTN_RIGHT) setTempo(bpm + 0.1f);
        // UP/DOWN: to the next whole BPM that way (120.5 up: 121; 135 down: 134).
        long tenths = lroundf(bpm * 10);
        if (pressed & m8::BTN_UP) setTempo(tenths / 10 + 1);
        if (pressed & m8::BTN_DOWN) setTempo((tenths + 9) / 10 - 1);
      } else if (onClock) {                          // the master volume (UP/DOWN) and balance (LEFT/RIGHT), a step at a time
        if (pressed & m8::BTN_DOWN) stepMasterVolume(-1);
        // (UP at full volume, panned: the balance a step back to the centre, the far side a line more.)
        if (pressed & m8::BTN_UP) masterVolume() >= MASTER_VOLUME_MAX && masterBalance() ? stepMasterBalance(masterBalance() > 0 ? -1 : 1) : stepMasterVolume(1);
        // (Balance all the way one side, that way again: the volume up a step.)
        if (pressed & m8::BTN_LEFT) masterBalance() <= -MASTER_BALANCE_MAX ? stepMasterVolume(1) : stepMasterBalance(-1);
        if (pressed & m8::BTN_RIGHT) masterBalance() >= MASTER_BALANCE_MAX ? stepMasterVolume(1) : stepMasterBalance(1);
      }
    } else if ((pressed & (m8::BTN_UP | m8::BTN_DOWN)) && !recordingOn(selected) && recordQueued() != selected &&
               (keys & m8::BTN_EDIT) && editAfterOpt && hasClip(selected)) {
      // A track with a clip, A pressed while B is held (B + A + UP/DOWN, from
      // any cell): moves it up / down, the track in its way taking its place
      // (wrapping round the 7); the selection goes with it.
      int to = (pressed & m8::BTN_UP) ? (selected + NUM_TRACKS - 1) % NUM_TRACKS : (selected + 1) % NUM_TRACKS;   // (row 0: track 7, at the top)
      if (swapTracks(selected, to)) {
        drawTrack(selected);
        selected = to;
      }
    } else if (clipRotating() && (pressed & (m8::BTN_LEFT | m8::BTN_RIGHT))) {
      // B then A on a clip (any of its cells): its fine rotate (the clip turned, its
      // audio as it was): RIGHT/LEFT tapped a quarter beat later / earlier;
      // held, a repeat five of 1 ms, five of 2, five of 4, then 10 ms each, instead (serviceRotate()); while held only the waveform drawn (and the ms
      // shown) turn, heard at each quarter beat crossing and once a key of the
      // combo is let go (serviceRotate(), above). Both held: back to none.
      // Reaching none (or passing it): there, the held arrow's steps stopped
      // (a fresh press goes on).
      const uint8_t LR = m8::BTN_LEFT | m8::BTN_RIGHT;
      if ((keys & LR) == LR) {
        nudgeStep(0, true);
      } else if (!repeat) {                        // (its steps while held: timed, not the arrows' repeats)
        if (rotateT != selected) {
          rotateHeld = false;                      // (another track's, let go of: to it now)
          serviceRotate();
          rotateBy = rotateStepsBy = 0;
          rotateQ = quarterNow();                  // (heard from the next crossing on)
        }
        rotateT = selected;
        rotateHeld = true;
        rotateKey = pressed & LR;
        rotateStepped = rotateStopped = false;
        heldSteps = 0;
        stepAt = millis() + REPEAT_DELAY_MS;       // (a tap's quarter beat on letting go, serviceRotate()'s ms steps once held)
      }
    } else if (selectedCell == WAVE_CELL) {
      // Loop range in beats: any whole number of beats, up to the whole loop.
      //   LEFT/RIGHT: move its start a step, wrapping around the loop (a range
      //     may run past the loop's end): a beat for loops up to 8 bars (BEAT_STEP_MAX_BARS), half
      //     a bar up to 16 bars, a bar beyond; a range of the whole loop, the
      //     start position's grid (startGrid(): each step moves the start
      //     position). Starts off that grid go to the next grid point that way.
      //   UP/DOWN: to the next / previous of 1/4, 1/2, 3/4, 1, 2, 3, 4,
      //     5, 6 ... bars (up to the whole loop).
      //   (No double taps: held, a step a repeat, on past the clip's edges.) A range inside the loop
      //     stops at the loop's edge rather than crossing it; one already at
      //     that edge jumps to the other. A range across the edge moves freely.
      //   Tapped twice, UP/DOWN: to the next / previous power of two: 1/4,
      //     1/2, 1, 2, 4, 8, 16, 32 bars (up to the whole loop). Three times a
      //     power of two (3, 6 ... bars) doubles / halves instead (3/4 bar:
      //     to 1 / 1/2; 3 bars halves to 2: no 1 1/2 bars).
      // The start stays when the length changes. A double tap counts from the
      // range before the first tap (which already moved it a step).
      Track &tr = tracks[selected];
      uint32_t rs, rb, loopBeats = tr.loopBars * BEATS_PER_BAR;
      trackRange(selected, rs, rb);                  // includes a queued change
      static int tapTrack = -1;
      static uint32_t tapAt[4], tapStart[4], tapLen[4];   // per arrow: the last tap and the range before it
      const uint8_t keys4[4] = {m8::BTN_LEFT, m8::BTN_RIGHT, m8::BTN_UP, m8::BTN_DOWN};
      // Lengths in beats: the steps (1, 2, 3, 4, then every 4) and the
      // jumps (powers of two, or doubling / halving three times one). Next
      // above / below l (at least 1 beat).
      auto stepAbove = [](uint32_t l) { return l < 4 ? l + 1 : (l / 4 + 1) * 4; };
      auto stepBelow = [](uint32_t l) { return l <= 1 ? 1 : l <= 4 ? l - 1 : (l - 1) / 4 * 4; };
      auto threeTimesPow2 = [](uint32_t l) { return l % 3 == 0 && ((l / 3) & (l / 3 - 1)) == 0; };
      auto jumpAbove = [&](uint32_t l) {
        if (threeTimesPow2(l) && l * 2 != 6) return l * 2;   // (no 1 1/2 bars)
        uint32_t j = 1; while (j <= l) j *= 2; return j;
      };
      auto jumpBelow = [&](uint32_t l) {
        if (threeTimesPow2(l) && l >= 6 && l / 2 != 6) return l / 2;
        uint32_t j = 1; while (j * 2 < l) j *= 2; return l <= 1 ? 1 : j;
      };
      uint32_t step = tr.loopBars <= BEAT_STEP_MAX_BARS ? 1 : tr.loopBars <= 16 ? 2 : 4;   // the start's grid
      // The whole loop: the start position's own grid (startGrid()), so every
      // step moves it (a finer step would leave it where it was).
      if (rb >= loopBeats) step = max(step, startGrid(loopBeats, rb));
      for (int k = 0; k < 4; k++) {
        if (!(pressed & keys4[k])) continue;
        bool twice = false;                          // (double taps: none on the arrows any more; held, a step a repeat, on past the clip's edges)
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
      // (A step on the clip's scale, engine.h: the track keeps steps from unity.)
      const Track &tr = tracks[selected];
      float d = headroomDbOf(tr.overview);
      int step = volumeStepOf(tr.volume, d), balance = tr.balance;
      // (UP at the top step, panned: the balance a step back to the centre, the far side a half more.)
      if (pressed & m8::BTN_UP) step >= VOLUME_MAX && balance ? balance += balance > 0 ? -1 : 1 : step++;
      if (pressed & m8::BTN_DOWN) step--;
      // (Balance all the way one side, that way again: the volume up a step.)
      if (pressed & m8::BTN_RIGHT) balance >= BALANCE_MAX ? step++ : balance++;
      if (pressed & m8::BTN_LEFT) balance <= -BALANCE_MAX ? step++ : balance--;
      int volume = volumeStoredOf(constrain(step, 0, VOLUME_MAX), d);
      balance = constrain(balance, -BALANCE_MAX, BALANCE_MAX);
      if (volume != tr.volume || balance != tr.balance) {
        setTrackMix(selected, volume, balance);
        trackChanged(selected);
      }
    }
  } else if (shiftA) {
    sceneKeys();
    // (SHIFT+LEFT/RIGHT on the status row: the tempo nudged while held, nudgeService().)
    // SHIFT+LEFT/RIGHT on a track (any of its cells; its waveform cell framed
    // meanwhile, ui.h frameCell()): its sample, the previous / next on the card.
    if (!onStatus && !recordingOn(selected) && !previewing) {
      // (another sample: the waveform cell selected, to see it)
      // Held: the repeats stop before the letter changes (that way, the next
      // sample's name starts with another letter: right, A009 before B001;
      // left, A001 before the letter before).
      const int dir = (pressed & m8::BTN_LEFT) ? -1 : (pressed & m8::BTN_RIGHT) ? 1 : 0;
      if (dir && stepSample(selected, dir)) {
        sampleSteppedAt = max(1u, millis());
        if (sampleLetterEndsAt(unitOf(selected).sample, dir)) stopRepeat = true;   // (a press landing there: no repeats past it either; another press crosses)
      }
    }
  } else {
    int row = selected, at = selectedCellIn(selected);
    // (By place on the screen: main, track 7 at the top, the status row at the
    // bottom; notes, the status row at the top.) Off its top or bottom row (a
    // fresh press; a held arrow stops there): the menu, once let go. (The notes
    // screen: not reached for now, flipOnRelease unused.)
    int pos = rowPos(selected);
    if (pressed & m8::BTN_UP && pos > 0) selected = rowAtPos(pos - 1);
    else if ((pressed & m8::BTN_UP) && !repeat) menuOnRelease = m8::BTN_UP;
    if (pressed & m8::BTN_DOWN && pos < ROWS - 1) selected = rowAtPos(pos + 1);
    else if ((pressed & m8::BTN_DOWN) && pos == ROWS - 1 && !repeat) menuOnRelease = m8::BTN_DOWN;
    // To or from the status row: the column that row was last left on (the
    // tracks' and the status row's each their own); the first time, the cell
    // below / above, the status row's scene cell under the tracks' waveform cell.
    // (From an empty track, its one cell: the column kept for the rows with three, not that cell's.)
    keepColumns(row, at);
    // LEFT off a row's first cell (a fresh press): the menu, once let go (it's to the left).
    if (pressed & m8::BTN_LEFT && selectedCellIn(selected) > firstCellIn(selected)) selectedCell = selectedCellIn(selected) - 1;
    else if ((pressed & m8::BTN_LEFT) && !repeat) menuOnRelease = m8::BTN_LEFT;
    if (pressed & m8::BTN_RIGHT && selectedCellIn(selected) < cellCount(selected) - 1) selectedCell = selectedCellIn(selected) + 1;
    else if ((pressed & m8::BTN_RIGHT) && !repeat) menuOnRelease = m8::BTN_RIGHT;   // RIGHT off a row's last cell: the menu too
    if (pressed & arrows) moved = selected != row || selectedCellIn(selected) != at;
  }

  // START: start from 1.1.1 / stop. With SHIFT: start from 1.1.1 on every
  // press, running or not. A on the status row: duplicate the scene (scene
  // cell), play / stop (clock cell), tap tempo (tempo cell); with SHIFT, on any of them: the same
  // as SHIFT+START.
  // B+A on the tempo cell: the metronome's next setting; on an empty track: resample (B otherwise adjusts cells). With SHIFT too, the order they were pressed in:
  // B then SHIFT, +A: paste the scene clipboard after the scene; SHIFT then
  // B, +A: cut the scene to it.
  if (pressed & m8::BTN_EDIT) noteActionPress();     // before it acts: the icon shows what it does
  bool edit = (pressed & m8::BTN_EDIT) && !opt;
  if ((pressed & m8::BTN_EDIT) && opt) {
    if (shift) {
      sceneChanged = optFirst ? scenePaste() : sceneCut();
    } else if (onStatus) {
      setMetronomeMode(nextMetronomeMode());       // B+A on the status row (any cell): the metronome on / off
      metronomeOn = metronomeWanted();             // (at once: the next beat too)
    } else if (!onStatus && tracks[selected].state == TrackState::Empty && !recordingOn(selected) &&
               recordQueued() != selected) {
      // An empty track resamples: records the app's output (queued behind a
      // recording, as A records), whatever is heard (no clip unmuted too: the
      // input). At once, on the press.
      if (recordPress(selected, true)) selectedCell = WAVE_CELL;
    }
  }
  bool transportKey = pressed & m8::BTN_PLAY;
  bool toggleTransport = transportKey && !shift;
  if ((transportKey || (edit && onStatus && !onScene && !onTempo)) && shift) resumeByHand();
  if (edit && onStatus && shift && onTempo) audioFollowToggle();   // SHIFT+A on the tempo cell: audio follow on / off
  // SHIFT+A on the scene cell: a copy of the scene after it, as SHIFT+B
  // twice; A again with SHIFT still held: that copy's tracks all cleared.
  if (!shift) dupJustMade = false;
  if (edit && onStatus && shift && onScene) {
    if (dupJustMade) {
      sceneChanged = sceneClear();
      dupJustMade = false;
    } else {
      sceneChanged = dupJustMade = sceneDuplicate();
    }
  }
  if (edit && onStatus && !shift) {
    if (onScene) nextSceneArmed = true;            // (acts once A is let go, below)
    else if (onClock) {                            // play / stop, as START
      if (transport.running) transportStop();
      else startByHand();
    } else if (onTempo) tapTempo();
  }
  if (edit && !onStatus) {
    Track &tr = tracks[selected];
    if (shift) {
      // SHIFT+A: cut the clip to the clipboard, or paste it onto an empty
      // track; recording (or queued): cancel it, the recording thrown away.
      if (hasClip(selected)) cutTrack(selected);
      else if (recordingOn(selected) || recordQueued() == selected) cancelRecording(selected);
      else pasteTrack(selected);
    } else if (tr.state == TrackState::Empty || recordingOn(selected)) {
      // (A recording told to stop, not a resampling: on to the next free track
      // now, as the setting says, not once it has finished.)
      const bool stopping = recording.mode == Recording::Running && recording.track == selected && !recording.resample;
      if (recordPress(selected)) {                   // a recording (or its stop, its cancel, out of the queue): at once, on the press
        selectedCell = WAVE_CELL;
        if (stopping) {
          nextChosen = true;
          selectAfterRecording(selected);
        }
      }
    } else {
      for (int k = 0; k < NUM_TRACKS; k++) swipeWas[k] = trackMuted(k);   // (to undo to)
      setTrackMuted(selected, !trackMuted(selected));
      swipeOn = true;                                // (held: UP / DOWN carry it on, above)
      swipeMute = trackMuted(selected);
      swipeFrom = selected;
      swipeDir = swipePos = 0;
    }
  }
  if (toggleTransport) {
    if (transport.running) {
      transportStop();
    } else {
      startByHand();
    }
  }
  if (!sceneMod) sceneChanged |= sceneHold(false);  // SHIFT (or B) let go: to the scene stepped to
  // Show the result right away rather than on the rows' next redraw: the new
  // selection first, then the old one, so highlight and icon move together.
  if (sceneChanged)
    for (int i = 0; i < ROWS; i++) drawTrack(i);
  drawTrack(selected);
  static bool wasPending = false;                  // (the frame hidden or back, or moved by SHIFT: the rows sharing its borders too)
  static uint8_t wasModifier = 0;
  if (screenSwitchPending() != wasPending || arrowModifier() != wasModifier) {
    wasPending = screenSwitchPending();
    wasModifier = arrowModifier();
    if (!menuOpen())
      for (int r : {rowAbove(selected), rowBelow(selected)})
        if (r >= 0 && r < NUM_TRACKS) drawTrack(r);
  }
  if (prevSel != selected) {
    drawTrack(prevSel);
    for (int r : {rowAbove(selected), rowBelow(selected), rowAbove(prevSel), rowBelow(prevSel)})   // (sharing a border row with them: the ground there)
      if (r >= 0 && r < NUM_TRACKS) drawTrack(r);
  }
  drawEdgeLine();                                  // (a switch waiting: its edge lit; let go without one: gone)
}

// After a recording finishes on the selected track, move on: to the next
// empty track (in their numbering, from the bottom up, wrapping), else the
// next muted one, else stay.
// fromScene: as the new scene has them (a resampling's: its switch still to
// come on the next beat), not as the tracks play now.
static void selectAfterRecording(int t, bool fromScene) {
  if (selected != t || !setting(SET_NEXT_FREE)) return;   // the user has moved on already (the setting off: it stays)
  for (TrackState want : {TrackState::Empty, TrackState::Muted}) {
    for (int k = 1; k < NUM_TRACKS; k++) {
      int i = (t - k + NUM_TRACKS) % NUM_TRACKS;   // (the track numbered next: the row above)
      int st = fromScene ? sceneUnitState(i) : -1;
      bool match = st >= 0 ? st == (want == TrackState::Empty ? 0 : 1) : tracks[i].state == want;
      if (match) {
        selected = i;
        drawTrack(i);
        drawTrack(t);
        return;
      }
    }
  }
}

// SHIFT+LEFT or RIGHT held on the status row, playing: the clock nudged
// faster (RIGHT) or slower (LEFT), as Live's nudge buttons, more the longer
// it's held (NUDGE_START, then NUDGE_RATE a second, at most NUDGE_MAX; in
// NUDGE_STEP steps); let go, the tempo set again. The tempo shown stays the
// one set (its number in the accent colour meanwhile). Following an external
// clock (whose tempo it is): its offset moved instead, as far as the nudge
// would have moved the clock (earlier: RIGHT, the clock ahead), the follower
// taking ours there; let go, it stays (as a nudge's shift does).
static constexpr float NUDGE_START = 0.01f, NUDGE_RATE = 0.03f, NUDGE_MAX = 0.08f, NUDGE_STEP = 0.0025f;
static bool nudging = false;
static int nudgeDir = 0;
static uint32_t nudgeAt = 0;
bool tempoNudging() { return nudging; }
static void nudgeService() {
  uint8_t k = board.keys();
  int dir = ((k & m8::BTN_RIGHT) ? 1 : 0) - ((k & m8::BTN_LEFT) ? 1 : 0);
  int src = syncSource();
  bool want = arrowModifier() == m8::BTN_SHIFT && dir && !scenePreviewing() && !(k & (m8::BTN_EDIT | m8::BTN_PLAY)) && selected == STATUS_ROW &&
              !menuOpen() && transport.running;
  static int nudgeSrc = SYNC_NONE;
  static uint32_t lastMs = 0;
  static float offsetMs = 0;                        // (following: the offset, nudged, unrounded)
  if (!want || (nudging && src != nudgeSrc)) {
    if (nudging && nudgeSrc == SYNC_NONE) transport.pendingSpb = spbForBpm(bpm);   // let go: the tempo set
    nudging = false;
    if (!want) return;
  }
  if (!nudging || dir != nudgeDir) {
    nudging = true;
    nudgeDir = dir;
    nudgeAt = lastMs = millis();
    nudgeSrc = src;
    if (src != SYNC_NONE) offsetMs = syncOffsetMs[src];
  }
  float amt = min(NUDGE_MAX, NUDGE_START + NUDGE_RATE * (millis() - nudgeAt) / 1000.0f);
  if (src != SYNC_NONE) {                            // following: the offset (ms) moved as the clock would have been
    offsetMs = constrain(offsetMs - dir * amt * (millis() - lastMs), (float)-SYNC_OFFSET_MAX_MS, (float)SYNC_OFFSET_MAX_MS);
    lastMs = millis();
    int16_t o = (int16_t)lroundf(offsetMs);
    if (o != syncOffsetMs[src]) {
      syncOffsetMs[src] = o;
      syncOffsetChanged(src);
    }
    return;
  }
  amt = roundf(amt / NUDGE_STEP) * NUDGE_STEP;       // (steps: few tempo changes, each a recording's tempo mark)
  uint32_t spb = (uint32_t)lroundf(spbForBpm(bpm) / (1 + dir * amt));
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  if (spb != now) transport.pendingSpb = spb;
}

// A, B and SHIFT pressed while the menu was open: in the looper's view,
// left out of the keys its looks follow (mainKeys()) until let go.
static uint8_t heldFromMenu = 0;
static constexpr uint8_t MODS = m8::BTN_EDIT | m8::BTN_OPT | m8::BTN_SHIFT;
uint8_t mainKeys() {
  uint8_t k = board.keys();
  return menuOpen() ? k : k & ~(heldFromMenu & k);
}

static uint8_t lastModifier = 0;                   // SHIFT or B, the latest pressed
uint8_t arrowModifier() {
  uint8_t k = mainKeys();
  if (lastModifier & k) return lastModifier;
  return k & m8::BTN_SHIFT ? m8::BTN_SHIFT : k & m8::BTN_OPT ? m8::BTN_OPT : 0;   // (the other one, still held)
}

void pollKeys() {
  // Arrows repeat while held: the press acts at once, then again after
  // REPEAT_DELAY_MS and every REPEAT_PERIOD_MS (the most recently pressed one).
  uint8_t keys;
  static uint8_t prev = 0, repeatKey = 0;
  static uint32_t repeatAt = 0;
  const uint8_t ARROWS = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  debugStage = "keys (I2C)";
  if (board.keysChanged(keys)) {
    debugStage = "key action";
    uint8_t pressed = keys & ~prev;
    heldFromMenu = (heldFromMenu & keys & ~pressed) | (menuOpen() ? pressed & MODS : 0);
    if (pressed & m8::BTN_SHIFT) lastModifier = m8::BTN_SHIFT;
    if (pressed & m8::BTN_OPT) lastModifier = m8::BTN_OPT;
    if (pressed & ARROWS) {
      repeatKey = pressed & ARROWS & -(pressed & ARROWS);   // one of them, if several
      repeatAt = millis() + REPEAT_DELAY_MS;
    } else if (!(keys & repeatKey)) {
      repeatKey = 0;                                 // released
    }
    onKeys(keys, pressed, false);
    if (stopRepeat) { repeatKey = 0; stopRepeat = false; }
    prev = keys;
  } else if (repeatKey && (int32_t)(millis() - repeatAt) >= 0) {
    debugStage = "key repeat";
    repeatAt = millis() + REPEAT_PERIOD_MS;          // after a slow pass: no burst of catch-up repeats
    onKeys(prev, repeatKey, true);
    if (stopRepeat) { repeatKey = 0; stopRepeat = false; }   // (a repeat asked to stop: none more)
  }
  // An arrow held off an edge (a switch waiting): SWITCH_HOLD_MS on, the switch
  // now; it doesn't repeat on the other screen (until pressed again).
  static bool edgeWaiting = false;
  bool waiting = menuOnRelease || leaveOnRelease || flipOnRelease || helpOnRelease;
  if (waiting && !edgeWaiting) switchWaitAt = millis();
  edgeWaiting = waiting;
  if (waiting && millis() - switchWaitAt >= SWITCH_HOLD_MS) {
    debugStage = "key held at an edge";
    edgeHeldLong = true;
    onKeys(prev, 0, true);
    edgeHeldLong = false;
    edgeWaiting = false;
    repeatKey = 0;
  }
  debugStage = "keys held (menu)";
  menuHold();
  switchHold();
  debugStage = "keys (rotate, nudge, tap)";
  serviceRotate();
  nudgeService();
  phaseService();
}

void followRecording() {
  // A resampling saved: a new scene holding just it (scenes.h), its track
  // selected.
  int resampled = takeResampled();
  if (resampled >= 0) {
    noteTouched(tracks[resampled].sample);         // (the project's last touched sample: an empty track's SHIFT+LEFT)
    int on = sceneFromResample(resampled);         // (on track 1)
    if (on >= 0) {
      selected = on;
      for (int i = 0; i < ROWS; i++) drawTrack(i);
      selectAfterRecording(on, true);       // (the setting on: on to the next free track there, as the new scene has them: track 2)
    } else {
      endSilencing();                              // (scene list full: it stays where it is)
    }
  }
  // A recording that ended with a clip (not cancelled): select the next track
  // (the SELECT NEXT FREE TRACK setting; a resampling: above, once in its scene).
  static int recTrack = -1;
  static bool recResample = false;
  if (recording.mode != Recording::Idle) {
    recTrack = recording.track;
    recResample = recording.resample;
  } else if (recTrack >= 0) {
    if (!recResample && tracks[recTrack].state != TrackState::Empty) noteTouched(tracks[recTrack].sample);
    if (!recResample && !nextChosen && tracks[recTrack].state != TrackState::Empty) selectAfterRecording(recTrack);   // (ended by itself)
    nextChosen = false;
    recTrack = -1;
  }
}
