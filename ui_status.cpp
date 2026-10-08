// ui_status.cpp — the status row: menu icon, scene, clock and tempo (see ui.h)
#include "ui.h"
#include "input.h"
#include "audio_follow.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
// The labels (the "SCN" and "BPM" tags, the scene's position, the tempo) are grey,
// light grey on the selected cell.
//   first cell: the scene, an inverted "SCN" tag and its position "3/7" (the
//     empty scene "0/7" and the copy after the last "*/7" in grey). Both in the
//     accent colour while SHIFT is held, or B alone on the cell: SHIFT or B +
//     UP/DOWN step through the scenes. The position flashes with the beat (the accent colour, then grey) while
//     another scene is selected but not there yet: SHIFT (or B) still held on
//     it, or the change waiting for its switch point. Selected, A (let go) goes to the
//     next scene (the clock as it is); SHIFT+A duplicates it (as SHIFT+B twice).
//   second cell: the clock (bar.beat.step), after a small play icon (the
//     accent colour while it runs), centred together: stopped, 1.1.1 as the
//     other labels (grey, light grey when selected); running, grey with light grey on each beat's
//     first step (selected: light grey, white on the first step), in the
//     accent colour on 1.1.1. Selected, A starts / stops the
//     clock (as START; SHIFT+A as SHIFT+START).
//   third cell: the tempo, followed by an inverted "BPM" tag (both in the
//     accent colour while B alone is held there: B + arrows change it). With
//     audio follow on (SHIFT+A there), the tempo flashes with the beat (black until
//     their beat is locked, then the accent colour; then as it was): the clock's
//     while it runs, else the one heard (none found yet: at the tempo shown).
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)

// The status row's cells: the scene's, the clock's and the tempo's (ui.h).
static constexpr int STATUS_CELLS = TEMPO_CELL + 1;
static const Cell &statusCell(int cell) {
  return cell == TEMPO_CELL ? TEMPO_BOX : cell == CLOCK_CELL ? CLOCK_BOX : cell == SCENE_CELL ? SCENE_BOX : MENU_BOX;
}

// ---- The master volume and balance: in the clock cell while B alone is held
// on it (B + UP/DOWN: the volume, LEFT/RIGHT: the balance). A vertical line
// per step in every other column, centred across the cell inside the border's
// block of space: each line's upper half the left channel's, its lower half
// the right's, lit in the accent colour up to that channel's level (the
// volume less the balance away from that side; none at 0), black after it.
// Each half meters its channel's output over them: white up to its level, on
// the same scale (line MASTER_VOLUME_UNITY: 0 dBFS), falling at
// MV_FALL_DB_PER_S. Shown again, it starts from silence.
static constexpr int MV_COLS = CLOCK_BOX.w / PX, MV_ROWS = STATUS_H / PX;   // 47 x 13, the border's included
static constexpr int MV_LINES = MASTER_VOLUME_MAX + 1;                      // (0 included)
static constexpr int MV_LEFT = (MV_COLS - (2 * MV_LINES - 1)) / 2;          // the first line's column (centred: 3 ... 43)
static constexpr int MV_HALF = (MV_ROWS - 4 - 1) / 2;                       // rows 2-5 left, 7-10 right
static_assert(MV_LEFT >= 2, "the lines inside the border's block of space");
static constexpr float MV_FALL_DB_PER_S = 24;

static void drawMasterVolume(bool full) {
  static uint16_t shown[MV_LINES][2];
  static float meterDb[2] = {-200, -200};
  static uint32_t lastMs = 0;
  auto &t = board.tft();
  const int top = rowTop(STATUS_ROW);
  float peak[2];
  takeOutputPeaks(peak[0], peak[1]);
  if (full) peak[0] = peak[1] = 0;                 // (what played while hidden: not shown)
  uint32_t now = millis();
  float fall = (now - lastMs) * (MV_FALL_DB_PER_S / 1000);
  lastMs = now;
  if (full) {
    t.fillRect(CLOCK_BOX.x + PX, top + PX, CLOCK_BOX.w - 2 * PX, STATUS_H - 2 * PX, rowGround(STATUS_ROW));   // (inside the border)
    meterDb[0] = meterDb[1] = -200;
  }
  const int vol = masterVolume(), bal = masterBalance();
  const int levels[2] = {vol - max(0, bal), vol - max(0, -bal)};
  for (int ch = 0; ch < 2; ch++) {
    float db = peak[ch] > 0 ? 20 * log10f(peak[ch]) : -200.0f;
    meterDb[ch] = max(db, meterDb[ch] - fall);
    // (Line j: step j's level, (j - MASTER_VOLUME_UNITY) * MASTER_STEP_DB dB.)
    int lit = meterDb[ch] < -150 ? 0 : constrain((int)floorf(meterDb[ch] / MASTER_STEP_DB + MASTER_VOLUME_UNITY + 0.5f) + 1, 0, MV_LINES);
    for (int j = 0; j < MV_LINES; j++) {
      bool inLevel = levels[ch] > 0 && j <= levels[ch];   // (0: none)
      uint16_t color = j < lit && inLevel ? INK : inLevel ? ACCENT : BG;
      if (!full && color == shown[j][ch]) continue;
      shown[j][ch] = color;
      t.fillRect(CLOCK_BOX.x + (MV_LEFT + 2 * j) * PX, top + (2 + ch * (MV_HALF + 1)) * PX, PX, MV_HALF * PX, color);
    }
  }
}

// A cell's border (the frame), each side in its colour (borderAt(): the
// two cells share the side between them).
static void drawFrame(int cell) {
  auto &t = board.tft();
  const Cell &c = statusCell(cell);
  int top = rowTop(STATUS_ROW), rows = STATUS_H / PX, cols = c.w / PX;
  t.fillRect(c.x + PX, top, c.w - 2 * PX, PX, borderAt(STATUS_ROW, cell, 0, 1, rows, cols));
  t.fillRect(c.x + PX, top + STATUS_H - PX, c.w - 2 * PX, PX, borderAt(STATUS_ROW, cell, rows - 1, 1, rows, cols));
  t.fillRect(c.x, top + PX, PX, STATUS_H - 2 * PX, borderAt(STATUS_ROW, cell, 1, 0, rows, cols));
  t.fillRect(c.x + c.w - PX, top + PX, PX, STATUS_H - 2 * PX, borderAt(STATUS_ROW, cell, 1, cols - 1, rows, cols));
  for (int r : {0, rows - 1})                      // and its corners (top: cut; bottom: the frame's, not cut)
    for (int k : {0, cols - 1})
      t.fillRect(c.x + k * PX, top + r * PX, PX, PX, borderAt(STATUS_ROW, cell, r, k, rows, cols));
}

void drawStatus() {
  if (menuOpen()) return;                          // (the menu: only the action button shows on the status row)
  static uint64_t shownBorders = 0;                // the cells' borders (bordersOf())
  static uint32_t drawn = 0;                       // uiGeneration when the cells were last cleared
  auto &t = board.tft();
  int top = rowTop(STATUS_ROW);
  // Selected: on a dark grey ground (rowGround()); only the selected cell in
  // its selected look (not the whole row's, as on a track).
  const uint16_t G = rowGround(STATUS_ROW);
  static uint16_t shownGround = BG;
  bool cleared = drawn != uiGeneration || G != shownGround;
  if (cleared) {
    drawn = uiGeneration;
    shownGround = G;
    for (int c = 0; c < STATUS_CELLS; c++)        // (the ground under the cells: the scene cell from the screen's left edge)
      t.fillRect(statusCell(c).x, top, statusCell(c).w, bgHeight(STATUS_ROW), c == MENU_CELL ? BG : G);
    shownBorders = ~0ull;
  }
  uint64_t borders = 0;
  for (int c = 0; c < STATUS_CELLS; c++) borders = borders * 31 + bordersOf(STATUS_ROW, c);
  if (borders != shownBorders) {
    shownBorders = borders;
    for (int c = SCENE_CELL; c < STATUS_CELLS; c++) drawFrame(c);   // (MENU_CELL: none, nothing drawn)
  }

  char text[24];
  // The tempo. A scene's tempo waiting for its switch point: the one playing until then.
  noInterrupts();
  bool tempoWaiting = transport.pendingSpb && transport.spbAtBeat >= 0;
  uint32_t playingSpb = transport.spb;
  interrupts();
  float shown = tempoWaiting ? roundf(bpmForSpb(playingSpb) * 10) / 10 : bpm;
  snprintf(text, sizeof text, "%.1f", shown);
  int cell = selected == STATUS_ROW && !(menuOpen() && !menuOnStatus()) ? selectedCellIn(STATUS_ROW) : -1;
  auto labelInk = [&](int c) -> uint16_t { return cell == c ? LIGHT : DIM; };   // (only the selected cell: not the whole row, as on a track)
  int tagY = top + (STATUS_H - TAG_H) / 2 / PX * PX, textY = top + (STATUS_H - FONT_H) / 2 / PX * PX;   // (on the block grid)
  {
    // The tempo, then an inverted "BPM" tag at the cell's right edge (like the
    // scene's "SCN"): both in the accent colour while B alone is held on the
    // cell (B + arrows change the tempo).
    const Cell &box = TEMPO_BOX;
    int right = box.x + box.w - INSET;
    uint16_t ink = cell == TEMPO_CELL && bAlone() ? ACCENT : labelInk(TEMPO_CELL);
    int tagX = right - tagWidth("BPM");
    drawTag(tagX, tagY, "BPM", TAG_INVERTED, ink, G);
    static const int tempoW = textWidth("888.8");  // the widest tempo: its box clears any other
    int numRight = tagX - PX, numLeft = numRight - tempoW, w = textWidth(text);
    t.fillRect(numLeft, tagY, numRight - numLeft + PX, textY - tagY, G);   // round the text: what was there
    t.fillRect(numLeft, textY + FONT_H, numRight - numLeft + PX, tagY + TAG_H - textY - FONT_H, G);
    t.fillRect(numLeft, textY, tempoW - w, FONT_H, G);
    t.fillRect(numRight, textY, PX, FONT_H, G);
    uint16_t numInk = audioFollowFlash() ? (audioFollowLocked() ? ACCENT : BG) : ink;   // (audio follow: the beat; black until locked)
    drawText(numRight - w, textY, text, numInk, G);
  }

  // The clock, or the master volume while B alone is held on its cell.
  static bool shownVolume = false;
  bool volume = cell == CLOCK_CELL && bAlone();
  if (volume) {
    drawMasterVolume(!shownVolume || cleared);
    shownVolume = true;
  } else if (shownVolume) {
    t.fillRect(CLOCK_BOX.x + PX, top + PX, CLOCK_BOX.w - 2 * PX, STATUS_H - 2 * PX, G);   // (the clock again)
    shownVolume = false;
  }
  noInterrupts();
  int32_t beat = transport.beat - transportOrigin();   // (from a project switch's bar line)
  uint32_t step = (uint64_t)transport.sampleInBeat * 4 / transport.spb;   // quarter beats
  bool running = transport.running;
  interrupts();
  if (running)
    snprintf(text, sizeof text, "%ld.%ld.%lu", beat / BEATS_PER_BAR % CLOCK_BARS + 1, beat % BEATS_PER_BAR + 1, step + 1);
  else
    snprintf(text, sizeof text, "1.1.1");          // where it starts from
  // Stopped: as the scene's and tempo's labels (grey; light grey selected). Running: grey, light grey on each
  // beat's first step (x.y.1) (selected: light grey, white on x.y.1; while the
  // metronome clicks, the accent colour instead); the accent colour on 1.1.1.
  bool first = beat % (BEATS_PER_BAR * CLOCK_BARS) == 0 && step == 0, here = cell == CLOCK_CELL;
  bool clicking = metronomeOn && transport.beat >= clickFromBeat;   // (silent while a switch waits for its bar line)
  uint16_t clockInk = !running ? labelInk(CLOCK_CELL) : first ? ACCENT
                    : step == 0 ? (clicking ? ACCENT : here ? INK : LIGHT) : (here ? LIGHT : DIM);
  if (!volume) {
    // A small play icon (in the accent colour while the clock runs), then the
    // text, centred together (on the block grid); what longer text left
    // either side cleared.
    static const char *const PLAY[5] = {"#..", "##.", "###", "##.", "#.."};
    const int iconW = 3 * PX, gap = 2 * PX;
    int w = textWidth(text), inL = CLOCK_BOX.x + INSET, inR = CLOCK_BOX.x + CLOCK_BOX.w - INSET;
    int x = CLOCK_BOX.x + (CLOCK_BOX.w - iconW - gap - w) / 2 / PX * PX, tx = x + iconW + gap;
    x -= PX;                                       // (the icon a block further left)
    t.fillRect(inL, textY, x - inL, FONT_H, G);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 3; c++)
        t.fillRect(x + c * PX, textY + r * PX, PX, PX, PLAY[r][c] == '#' && running ? ACCENT : G);
    t.fillRect(x + iconW, textY, tx - x - iconW, FONT_H, G);
    t.fillRect(tx + w, textY, inR - tx - w, FONT_H, G);
    drawText(tx, textY, text, clockInk, G);
  }

  // The scene. Its box reaches the cell's inner edge: the position's text
  // (up to "250*/250") always fits, and clears any longer text before it.
  const Cell &box = SCENE_BOX;
  // The tag and position in the accent colour: SHIFT held (SHIFT + UP/DOWN
  // step through the scenes), or B alone on the cell (B + UP/DOWN do).
  bool sceneAccent = (board.keys() & m8::BTN_SHIFT) || (cell == SCENE_CELL && bAlone());
  drawTag(box.x + INSET, tagY, "SCN", TAG_INVERTED, sceneAccent ? ACCENT : labelInk(SCENE_CELL), G);
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  if (scene > count) snprintf(text, sizeof text, "*/%d", count);   // the copy after the last
  else snprintf(text, sizeof text, "%d/%d", scene, count);       // (0: the empty scene)
  uint16_t ink = sceneAccent ? ACCENT : temporary ? DIM : labelInk(SCENE_CELL);
  if (sceneHeld() || sceneChangeWaiting()) {
    noInterrupts();
    bool firstHalf = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    ink = firstHalf ? ACCENT : DIM;
  }
  int x = box.x + INSET + tagWidth("SCN") + PX;
  drawText(x, textY, text, ink, G, box.x + box.w - INSET - x);

}

