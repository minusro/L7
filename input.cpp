// input.cpp — see input.h. The key list is at the top of m8_looper.ino.
#include "input.h"
#include "ui.h"
#include "ableton.h"
#include "usb_drive.h"

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;   // held arrows
static constexpr uint32_t DOUBLE_TAP_MS = 200;      // two taps of a key this close: a double tap
static constexpr uint32_t EDGE_HOLD_MS = 350;       // SHIFT+UP/DOWN's second tap held this long: to the list's end

// The selected track's current cell back to its defaults: the whole loop as
// the range, the filter open, volume at unity and balance centred.
static void resetCell() {
  if (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == CLOCK_CELL) {   // the master volume: back to 100%
    stepMasterVolume(MASTER_VOLUME_UNITY - masterVolume());
    return;
  }
  if (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == TEMPO_CELL) {   // the tempo: the project's last recording's
    float b = projectRecordingTempo();
    if (b > 0) setTempo(b);
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

// Tap tempo (A on the status row): from the second tap on, the tempo is the
// least-squares fit of every tap in the run (tap n at beat n), so each tap
// refines it: two taps give their interval, a long series evens out the
// jitter of single taps. Taps further apart than TAP_TIMEOUT_MS start a new
// run, and so does any other key press in between. Every 4th tap after the
// first (the 5th, 9th, 13th ...) lands on a bar line: a stopped transport
// starts there from 1.1.1; a running one snaps to the nearest bar start.
static constexpr uint32_t TAP_TIMEOUT_MS = 2100;     // a little over a beat at the slowest tempo (30 BPM)

static int tapCount = 0;                            // taps in this run (0: the next is the first)


static void tapTempo() {
  static uint32_t first, last;                      // this run's first and latest taps
  static double sumT, sumIT;                        // over the taps: t (ms after the first), i * t
  uint32_t now = millis();
  if (tapCount > 0 && now - last > TAP_TIMEOUT_MS) tapCount = 0;
  if (tapCount == 0) {
    first = now;
    sumT = sumIT = 0;
  }
  double i = tapCount, t = now - first;
  sumT += t;
  sumIT += i * t;
  last = now;
  int n = ++tapCount;
  if (n >= 2) {
    // The slope of t over i (ms per beat), with i = 0 .. n-1:
    // sum(i) = n(n-1)/2, n sum(i^2) - sum(i)^2 = n^2(n^2-1)/12.
    double sumI = n * (n - 1) / 2.0;
    double msPerBeat = (n * sumIT - sumI * sumT) / (n * (double)n * ((double)n * n - 1) / 12);
    if (msPerBeat > 0) setTempo(60000.0f / msPerBeat);
  }
  if (n > 1 && n % 4 == 1) {                        // 5th, 9th, ...: a bar line now
    if (transport.running) transportSnapToBar();
    else transportRestart();
  }
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
    snprintf(b + head, sizeof b - head, "%s %c%03d", APP_NAME, 'A' + p, n);
    menuExportStatus(b, head);                       // (the name in the project's colour)
  } else {
    snprintf(b, sizeof b, "EXPORT FAILED");
    menuExportStatus(b);
  }
  if (wasRunning) transportStart();                  // (running before: again, from 1.1.1)
}

static bool optFirst = false;
bool optBeforeShift() { return optFirst; }

// The last arrow press moved the selection (not: already at the edge that way).
static bool moved = false;
bool arrowsMoved() { return moved; }

// A or B down on a menu item: its action waits for the keys to be let go.
static bool armed = false, shifted = false;          // SHIFT with it
static int armedSlot = -1;                           // the slot it was pressed on (-1: a list item)
static int armedItem = -1;                           // or the list item
static bool loadedOnPress = false;                   // its press loaded the card's project: held on, the menu closes
static uint32_t armedAt = 0;
static constexpr uint32_t LONG_PRESS_MS = 600;

static bool nextSceneArmed = false;               // A pressed on the scene cell: the next scene once it's let go

// An arrow held that switches screens when let go: into the menu, or out of it.
static uint8_t menuOnRelease = 0, leaveOnRelease = 0;
// LEFT+RIGHT pressed together (the second within LR_WINDOW_MS of the first,
// nothing else held) and held LONG_PRESS_MS: the other screen (switchHold()).
// From the second one's press until both are let go, they don't move the
// selection (the first one's step: undone by the second's, or it stopped at
// an edge).
static bool lrHeld = false, lrDone = false;
static uint32_t lrAt = 0, leftAt = 0, rightAt = 0;   // (the combo's start; each arrow's last fresh press)
static constexpr uint8_t LR = m8::BTN_LEFT | m8::BTN_RIGHT;
static constexpr uint32_t LR_WINDOW_MS = 100;
bool screenSwitchPending() { return menuOnRelease || leaveOnRelease || (lrHeld && !lrDone); }
uint8_t screenSwitchKey() { return menuOnRelease | leaveOnRelease | (lrHeld && !lrDone ? LR : 0); }

// Loads project slot p from the menu; a new project (an empty slot) opens
// with the selection on track 1 (the looper's, for when the menu closes);
// another keeps the selection as it is.
static bool loadFromMenu(int p) {
  bool fresh = !projectFilled(p);
  int from = projectCurrent();
  if (!projectLoad(p)) return false;
  menuNoteLoad(from);                              // (its card and the old one's take turns until it comes in)
  if (fresh) setMainSelection(NUM_TRACKS - 1, WAVE_CELL);
  metronomeOn = metronomeWanted();                 // (now: its first beat clicks too)
  return true;
}

// Out of the menu, back to the looper (as A on the loaded project's card);
// with no project loaded, a new one is made in the first empty slot (its card
// selected), as A there. False: nowhere to go.
// key: the arrow it left with (the looper's selection where it lands,
// menuLanding()); 0: the looper's selection as it was.
static bool leaveMenu(uint8_t key = 0) {
  int row, cell;
  bool land = menuLanding(key, row, cell);       // (from the item it left on, before a new project's card is selected)
  bool back = projectCurrent() >= 0;
  if (!back) {
    int slot = menuFirstEmptySlot();
    if (slot < 0) return false;
    menuSelectSlot(slot);
    if (loadFromMenu(slot)) back = true;
  }
  if (back) {
    if (land) setMainSelection(row, cell);
    closeMenu();
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

// A or B held on a card whose project its press loaded: after LONG_PRESS_MS,
// the menu closes too. Called every pass (pollKeys()).
static void menuHold() {
  if (!menuOpen()) closeWhenIn = false;
  if (closeWhenIn && !menuLoadPending()) closeToLooper();   // (come in: now)
  if (!armed || !loadedOnPress || !menuOpen() || millis() - armedAt < LONG_PRESS_MS) return;
  loadedOnPress = false;
  armed = false;                                     // (letting go: nothing more)
  closeToLooper();
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
  if (!lrHeld || lrDone || millis() - lrAt < LONG_PRESS_MS) return;
  lrDone = true;
  if (!menuOpen()) {
    openMenu();
  } else if (menuLoadPending()) {
    closeWhenIn = true;
    drawPending();
  } else if (!leaveMenu()) {
    drawPending();
  }
}

static void onKeys(uint8_t keys, uint8_t pressed, bool repeat) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  if (pressed & m8::BTN_OPT) optFirst = !(shift && !(pressed & m8::BTN_SHIFT));
  if (pressed & m8::BTN_SHIFT) optFirst = opt && !(pressed & m8::BTN_OPT);
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
    if (menuOnRelease || leaveOnRelease) stepped = false;   // (it stopped at an edge, a switch waiting: that's off)
    menuOnRelease = leaveOnRelease = 0;
    if (!stepped) pressed &= ~LR;
    else repeat = true;                              // (stepping back: no switch off this edge)
    drawPending();
  } else if (lrHeld || (lrDone && (keys & LR))) {
    pressed &= ~LR;
  }
  if (lrDone && !(keys & LR)) lrDone = false;
  // Any arrow off the looper's edge (LEFT off a row's first cell, RIGHT off
  // its last, UP on track 1, DOWN on the status row; a fresh press, not a
  // held arrow repeating): the menu opens once that key is let go (the clock
  // goes on).
  // (menuOnRelease: the key to wait for, file scope: screenSwitchPending())
  if (menuOnRelease && !(keys & menuOnRelease)) {
    uint8_t key = menuOnRelease;
    menuOnRelease = 0;
    openMenu();
    menuLand(key);                                   // (its selection: the nearest item that way, as if wrapped)
    return;
  }
  // RIGHT off the menu's right edge, LEFT off its left, UP off its top or
  // DOWN off its bottom (menuLeavesOn()): back to the looper once that key is let go, as A on the
  // loaded project's card; with no project loaded, a new one is made in the
  // first empty slot (its card selected), as A there.
  // (leaveOnRelease: likewise)
  if (leaveOnRelease && !(keys & leaveOnRelease)) {
    uint8_t key = leaveOnRelease;
    leaveOnRelease = 0;
    if (menuLeavesOn(key)) leaveMenu(key);
    return;
  }
  for (uint8_t key : {m8::BTN_RIGHT, m8::BTN_UP, m8::BTN_DOWN, m8::BTN_LEFT})
    if (menuLeavesOn(key) && (pressed & key) && !repeat && !(keys & (m8::BTN_SHIFT | m8::BTN_OPT | m8::BTN_EDIT | m8::BTN_PLAY))) {
      leaveOnRelease = key;
      return;
    }
  // The menu's items: the arrows move its selection; A or B runs the action
  // button's action once let go (on the item it was pressed on); START starts /
  // stops the clock, as in the looper's view. The menu doesn't
  // start or stop the clock.
  // SHIFT+A on a project: cuts it when let go (no confirmation). A on a card,
  // at once on the press: its project loaded, the menu staying open (held
  // LONG_PRESS_MS: it closes too, menuHold(); let go, the two cards take turns
  // as the loaded one until it comes in: menuNoteLoad()); on an empty slot: a
  // new project made and opened, the menu closing; on the loaded project's
  // card: the menu closes. While a project loaded here is on its way (playing:
  // until its switch point) the menu stays: closing waits for it
  // (closeToLooper()), and no edge leads out (menuLeavesOn()). The list's
  // items act once let go. A and B do the same
  // everywhere in the menu.
  if (!menuOpen()) armed = false;
  if (menuOpen() && !menuOnStatus()) {
    if (pressed & m8::BTN_PLAY) {                    // START: the clock, as in the looper's view (SHIFT: from 1.1.1)
      if (shift) transportRestart();
      else if (transport.running) transportStop();
      else transportStart();
    }
    const uint8_t act = m8::BTN_EDIT | m8::BTN_OPT;
    if (pressed & act) noteActionPress();            // (the icon: what letting go will do)
    if ((pressed & act) && !armed) {
      armed = true;
      armedSlot = menuSlot();
      armedItem = menuListItem();
      shifted = shift;
      armedAt = millis();
      loadedOnPress = false;
      int slot = armedSlot;
      if (slot >= 0 && !shift) {                     // a card: at once, on the press
        if (slot == projectCurrent()) {              // the loaded one: back to the looper (one on its way: once it has come in)
          closeToLooper();
          return;
        }
        closeWhenIn = false;                         // (another one now: the menu stays)
        bool fresh = !projectFilled(slot);
        if (loadFromMenu(slot)) {                    // another: loaded (the looper's selection stays as it is)
          if (fresh) {                               // an empty slot: a new project, opened at once (on track 1)
            closeMenu();
            for (int i = 0; i < ROWS; i++) drawTrack(i);
            return;
          }
          loadedOnPress = true;                      // (held LONG_PRESS_MS: the menu closes too, menuHold())
          drawMenu();
        }
      }
    }
    if (armed && !(keys & (m8::BTN_EDIT | m8::BTN_OPT))) {   // all let go: act
      armed = false;
      int slot = armedSlot;
      if (slot < 0) {                                // the list's items: export, storage mode
        if (armedItem == 0 && !shifted && projectCurrent() >= 0 && projectHasClips() && !menuExportShown()) exportToAbleton();
        if (armedItem == 1 && !shifted) {
          bool wasRunning = transport.running;
          if (usbDriveMode()) {                      // (back from it: everything read from the card again)
            drawMenu();
            if (wasRunning) transportStart();        // (running before: again, from 1.1.1)
          }
        }
      } else if (shifted) {                          // SHIFT: cut its project, or paste one into it
        if (projectFilled(slot)) projectCut(slot);
        else projectPaste(slot);
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
    if (onStatus && selectedCellIn(STATUS_ROW) == SCENE_CELL && sceneNext())
      for (int i = 0; i < ROWS; i++) drawTrack(i);
  }
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
    } else if ((pressed & (m8::BTN_UP | m8::BTN_DOWN)) && unitOf(selected).sample == 0 && !recordingOn(selected) &&
               recordQueued() != selected) {
      // An empty track (a change on its way counts): B + UP/DOWN moves it up /
      // down, the track in its way taking its place (wrapping round the 7);
      // the selection goes with it.
      int to = (pressed & m8::BTN_UP) ? (selected + 1) % NUM_TRACKS : (selected + NUM_TRACKS - 1) % NUM_TRACKS;   // (row 6: track 1, at the top)
      if (swapTracks(selected, to)) {
        drawTrack(selected);
        selected = to;
      }
    } else if (selectedCell == WAVE_CELL) {
      // Loop range in beats: any whole number of beats, up to the whole loop.
      //   LEFT/RIGHT: move its start a step, wrapping around the loop (a range
      //     may run past the loop's end): a beat for loops up to 8 bars (BEAT_STEP_MAX_BARS), half
      //     a bar up to 16 bars, a bar beyond; a range of the whole loop, the
      //     start position's grid (startGrid(): each step moves the start
      //     position). Starts off that grid go to the next grid point that way.
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
      // The whole loop: the start position's own grid (startGrid()), so every
      // step moves it (a finer step would leave it where it was).
      if (rb >= loopBeats) step = max(step, startGrid(loopBeats, rb));
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
      // (another sample: the waveform cell selected, to see it)
      if ((pressed & m8::BTN_LEFT) && stepSample(selected, -1)) selectedCell = WAVE_CELL;
      if ((pressed & m8::BTN_RIGHT) && stepSample(selected, 1)) selectedCell = WAVE_CELL;
    }
  } else {
    int row = selected, at = selectedCellIn(selected);
    // (By place on the screen: track 1 at the top, the status row at the bottom.)
    int pos = rowPos(selected);
    if (pressed & m8::BTN_UP && pos > 0) selected = rowAtPos(pos - 1);              // no wrapping
    else if ((pressed & m8::BTN_UP) && !repeat) menuOnRelease = m8::BTN_UP;      // UP on track 1 (a fresh press): the menu, once let go
    // DOWN: on to the status row (a held DOWN stops there); on the status row
    // (a fresh press): the menu, once let go.
    if (pressed & m8::BTN_DOWN && pos < ROWS - 1) selected = rowAtPos(pos + 1);
    else if ((pressed & m8::BTN_DOWN) && pos == ROWS - 1 && !repeat) menuOnRelease = m8::BTN_DOWN;
    // To or from the status row: the cell below / above, the status row's having the menu cell to the left of the tracks' first.
    // (From an empty track, its one cell: the column kept for the rows with three, not that cell's.)
    if (selected == STATUS_ROW && row != STATUS_ROW) selectedCell = min(selectedCell, CELLS - 1) + 1;
    else if (row == STATUS_ROW && selected != STATUS_ROW) selectedCell = max(at - 1, 0);
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
  // B+A on an empty track: resample (B otherwise adjusts cells). With SHIFT too, the order they were pressed in:
  // B then SHIFT, +A: paste the scene clipboard after the scene; SHIFT then
  // B, +A: cut the scene to it.
  if (pressed & m8::BTN_EDIT) noteActionPress();     // before it acts: the icon shows what it does
  bool edit = (pressed & m8::BTN_EDIT) && !opt;
  if ((pressed & m8::BTN_EDIT) && opt) {
    if (shift) {
      sceneChanged = optFirst ? scenePaste() : sceneCut();
    } else if (!onStatus && tracks[selected].state == TrackState::Empty && !recordingOn(selected) &&
               recordQueued() != selected && anyClipHeard()) {
      // An empty track resamples: records the app's output (queued behind a
      // recording, as A records), if there's anything to: a clip not muted.
      if (recordPress(selected, true)) selectedCell = WAVE_CELL;   // (a new recording: its waveform cell selected)
    }
  }
  bool transportKey = pressed & m8::BTN_PLAY;
  bool toggleTransport = transportKey && !shift;
  if ((transportKey || (edit && onStatus && !onScene && !onTempo)) && shift) transportRestart();
  if (edit && onStatus && shift && onTempo) {      // SHIFT+A on the tempo cell: the metronome's next setting
    setMetronomeMode(nextMetronomeMode());
    metronomeOn = metronomeWanted();               // (at once: the next beat too)
  }
  if (edit && onStatus && shift && onScene) sceneChanged = sceneDuplicate();   // SHIFT+A on the scene cell: a copy of the scene after it, as SHIFT+B twice
  if (edit && onStatus && !shift) {
    if (onScene) nextSceneArmed = true;            // (acts once A is let go, below)
    else if (onClock) {                            // play / stop, as START
      if (transport.running) transportStop();
      else transportStart();
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
      if (recordPress(selected)) selectedCell = WAVE_CELL;   // (a new recording: its waveform cell selected)
    } else {
      setTrackMuted(selected, !trackMuted(selected));
    }
  }
  if (toggleTransport) {
    if (transport.running) {
      transportStop();
    } else {
      transportStart();
    }
  }
  if (!sceneMod) sceneChanged |= sceneHold(false);  // SHIFT (or B) let go: to the scene stepped to
  // Show the result right away rather than on the rows' next redraw: the new
  // selection first, then the old one, so highlight and icon move together.
  if (sceneChanged)
    for (int i = 0; i < ROWS; i++) drawTrack(i);
  drawTrack(selected);
  static bool wasPending = false;                  // (the frame hidden or back: the rows sharing its borders too)
  if (screenSwitchPending() != wasPending) {
    wasPending = screenSwitchPending();
    if (!menuOpen())
      for (int r : {rowAbove(selected), rowBelow(selected)})
        if (r >= 0 && r < NUM_TRACKS) drawTrack(r);
  }
  if (prevSel != selected) {
    drawTrack(prevSel);
    for (int r : {rowAbove(selected), rowBelow(selected), rowAbove(prevSel), rowBelow(prevSel)})   // (sharing a border row with them: the ground there)
      if (r >= 0 && r < NUM_TRACKS) drawTrack(r);
  }
}

// After a recording finishes on the selected track, move on: to the next
// empty track (in their numbering, from the bottom up, wrapping), else the
// next muted one, else stay.
static void selectAfterRecording(int t) {
  if (selected != t) return;                       // the user has moved on already
  for (TrackState want : {TrackState::Empty, TrackState::Muted}) {
    for (int k = 1; k < NUM_TRACKS; k++) {
      int i = (t - k + NUM_TRACKS) % NUM_TRACKS;   // (the track numbered next: the row below)
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
  menuHold();
  switchHold();
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
