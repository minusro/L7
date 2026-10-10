// ui_wave.cpp — waveforms, loop range markers and playheads (see ui.h)
#include "ui.h"
#include "ui_font.h"
#include "input.h"

// ---- Waveforms
// Each track's whole loop in WAVE_COLS columns of blocks, inside the cell's
// border (shared with its neighbours: the selection frame) and a block of
// space. Rows of blocks (16 in the cell):
//   0, 15   the border
//   1, 14   space (the playheads reach into it)
// A black line the waveform's height, two pixels wide, just before each bar's
// start (but the first's), on every track, while recording too.
//   2 - 13  the waveform, in pixels (the only part not in blocks): a 2-pixel
//           line along the middle, the peaks reaching up to 11 pixels above
//           and below it, symmetric; 180 pixel columns. Heights go with the
//           square root of the peak level (as the overview stores it).
// The waveform is light grey inside the loop range (white on the selected
// track) and 50% grey outside; on a muted clip the range is 50% grey and the
// rest dark grey. While B is held on the selected waveform cell (other keys too; B +
// arrows change the range), the range is in the accent colour, muted or not.
// The start position: a checkerboard line of 1-pixel squares, two pixels wide and the
// waveform's height, where a bar line there would be, white and
// black (B held to edit the range too); on every track. The playhead is an accent-coloured line (white
// while the range is in the accent colour); on a muted clip a black one
// (cutting through its waveform).
// A recording track's waveform area is in the accent colour (a block of black
// round it, inside the border), its waveform growing in white (live view),
// the bar lines in the dark accent, the record head inverting its column.
// Bar lines, range edges, the start position and the playhead are placed by
// the pixel (180 across), not by the 2-pixel block; vertically they fill
// whole block rows. Full redraws only happen when something changes; the
// playhead moves by redrawing just the block columns it leaves and covers.

static constexpr int BLOCK_COLS = WAVE_W / PX;                  // 94, the border's included
static constexpr int WAVE_LEFT = 2;                            // the first waveform column (after the border and a space)
static constexpr int WAVE_COLS = BLOCK_COLS - 2 * WAVE_LEFT;   // 90 waveform columns
static constexpr int BAND_ROWS = CELL_H / PX;                  // 16
static constexpr int MID_ROW = 7, WAVE_REACH = 5;              // the line: rows 7-8; peaks: rows 2-13

// The waveform in pixels: a 2-pixel line along the middle of rows 2-13 (pixel
// rows 15-16 of the cell), the peaks reaching up to WAVE_REACH_PX pixels above
// and below it (to the edges of rows 2 and 13), symmetric, a pixel at a time.
static constexpr int WAVE_MID_Y = (MID_ROW + 1) * PX - 1;        // 15: the line's upper pixel row
static constexpr int WAVE_REACH_PX = WAVE_REACH * PX + 1;      // 11
static_assert(WAVE_MID_Y - WAVE_REACH_PX == (MID_ROW - WAVE_REACH) * PX, "the peaks reach the top of row 2");
// Reach (pixels each way) for a stored overview level (the square root of the
// peak, 0-255): in proportion.
static int pixelReach(uint8_t level) { return (level * WAVE_REACH_PX + 127) / 255; }

struct WaveView {
  bool visible = false;                            // something is shown (not an empty track)
  bool live = false;                               // recording in progress: live view
  uint16_t bg = BG;                                // behind the waveform (a recording's: the accent colour)
  uint16_t ground = BG;                            // round it (rowGround())
  uint64_t borders = 0;                            // the border's sides (bordersOf())
  int rangeFirstPx = 0, rangePx = 0;               // pixel columns inside the loop range: rangeFirstPx on, wrapping
  int markPx = -1;                                 // the start position's line: its first pixel column; -1: none
  int playPx = -1;                                 // playhead / record head: its first pixel column; -1: none
  uint16_t headColor = 0;                          // a line this colour; INVERT_HEAD: the column inverted
  uint16_t rangeInk = INK;                         // the waveform inside the loop range
  uint16_t outInk = DARK;                          // the waveform outside the loop range
  uint16_t turn = 0;                               // turned right this many columns (the fine rotate)
  // live view
  uint32_t liveBars = 0;
  int liveFilled = 0;                              // columns with recorded audio
  uint8_t liveData[OVERVIEW_COLS] = {};
  // what the last full draw showed
  uint8_t version = 0, state = 0;
  bool rec = false;
  uint32_t drawn = 0;                              // uiGeneration when last drawn whole
  uint32_t rs = 0, rb = 0;
  char label[24] = "";                             // the sample's label (SHIFT held: its name, X/N; "": none)
  bool ownFrame = false;                           // (the sample switch's accent frame drawn here: another cell selected)
  bool labelFlash = false;                         // (its name and number dark grey: the sample just stepped)
  int labelX = 0, labelW = 0;                      // its box: first block column, width in blocks
};
static WaveView views[NUM_TRACKS];
static constexpr uint32_t LABEL_FLASH_MS = 100;           // (a sample step: its label's name and number dark grey this long)
static constexpr int LABEL_TOP = BAND_ROWS - 7;             // the sample label's box: rows 9-15 (over the bottom border, its own bottom row)
// The record head's look (not a colour the playheads use): its column inverted.
static constexpr uint16_t INVERT_HEAD = 0x0001;

// Level shown in pixel column p (0 .. WAVE_COLS * PX - 1): the peak of the
// stored overview columns (OVERVIEW_COLS of them) that it covers.
static uint8_t pixelLevel(const uint8_t *src, int p) {
  const int cols = WAVE_COLS * PX;
  int a = p * OVERVIEW_COLS / cols, b = max(a + 1, (p + 1) * OVERVIEW_COLS / cols);
  uint8_t m = 0;
  for (int k = a; k < b; k++) m = max(m, src[k]);
  return m;
}

// The pixel column edge nearest beat `beat` of a loop of loopBeats (0..PX_COLS;
// past the loop's end for beats past it): edge k is between pixel columns
// k - 1 and k. Range edges, bar starts and the start position all snap to
// these, so they agree.
static constexpr int PX_COLS = WAVE_COLS * PX;                  // 180
static constexpr int LINE_W = 2;                               // a bar line's, the start position's and the playhead's width, pixels
static int edgePx(uint32_t beat, uint32_t loopBeats) {
  return (int)(((uint64_t)beat * PX_COLS + loopBeats / 2) / loopBeats);
}
// A line LINE_W wide from pixel column `from` (wrapping) covers pixel column p.
static bool covers(int from, int p) { return from >= 0 && (p - from + PX_COLS) % PX_COLS < LINE_W; }

// The start position's line in waveform column c of view v: the waveform's
// height (rows 2-13), a checkerboard of 1-pixel squares, white and black
// (over the waveform; editing the range too); on every track.
static constexpr int START_TOP = MID_ROW - WAVE_REACH, START_BOTTOM = MID_ROW + 1 + WAVE_REACH;   // rows 2-13

// Block column bc (0 and BLOCK_COLS - 1: the frame's), drawn whole: two
// pixel columns. The waveform is drawn in pixels (each pixel column its own
// peak, each pixel row a step), and so are the bar lines, the start position
// and the playhead across (each pixel column checked); the frame in blocks.
static void drawColumn(int i, int bc) {
  if (bc < 0 || bc >= BLOCK_COLS) return;
  const ShownTrack &tr = shownTrack(i);           // (as the row shows it: a switch all at once)
  const WaveView &v = views[i];
  static uint16_t px[CELL_H * PX];                 // PX pixels wide, CELL_H tall
  auto block = [&](int r, uint16_t color) {        // block row r, both pixel columns
    for (int y = 0; y < PX; y++) px[(r * PX + y) * PX] = px[(r * PX + y) * PX + 1] = color;
  };
  int c = bc - WAVE_LEFT;                          // waveform column
  for (int r = 0; r < BAND_ROWS; r++) {           // the border (the frame), round the background
    int b = borderAt(i, WAVE_CELL, r, bc, BAND_ROWS, BLOCK_COLS);
    // The background (a recording's accent colour) only behind the waveform:
    // a block of black between it and the border.
    bool behind = c >= 0 && c < WAVE_COLS && r >= MID_ROW - WAVE_REACH && r <= MID_ROW + 1 + WAVE_REACH;
    block(r, b >= 0 ? (uint16_t)b : behind ? v.bg : v.ground);
  }
  if (c >= 0 && c < WAVE_COLS && v.visible) {
    if (!v.live || c < v.liveFilled) {
      for (int sub = 0; sub < PX; sub++) {                        // the range's colour by the pixel (live: all of it, on the coloured cell)
        const int pxCols = WAVE_COLS * PX;
        bool in = v.live || (c * PX + sub - v.rangeFirstPx + pxCols) % pxCols < v.rangePx;
        uint16_t ink = !in ? v.outInk : v.rangeInk;
        int reach = pixelReach(pixelLevel(v.live ? v.liveData : tr.overview, c * PX + sub));
        for (int y = WAVE_MID_Y - reach; y <= WAVE_MID_Y + 1 + reach; y++) px[y * PX + sub] = ink;
      }
    }
    int bars = v.live ? (int)v.liveBars : (int)tr.loopBars;      // (recording: as it's shown so far)
    for (int sub = 0; sub < PX; sub++) {
      const int p = c * PX + sub;                                 // (this pixel column)
      for (int b = 1; b < bars; b++)                              // bars 2 on: a black line just before each start
        if (covers(edgePx(b * BEATS_PER_BAR, bars * BEATS_PER_BAR) - LINE_W, p)) {
          for (int y = (MID_ROW - WAVE_REACH) * PX; y < (MID_ROW + 2 + WAVE_REACH) * PX; y++) px[y * PX + sub] = v.live ? ACCENT_DARK : v.bg;   // (recording: the dark accent)
          break;
        }
      if (!v.live && covers(v.markPx, p))                         // the start position: a checkerboard line, 1-pixel squares,
        for (int y = START_TOP * PX; y < (START_BOTTOM + 1) * PX; y++)   // the waveform's height, white and black (B held too)
          px[y * PX + sub] = (p + y) % 2 == 0 ? INK : BG;
      if (covers(v.playPx, p)) {                                  // on top: the playhead
        int reach = v.live ? 0 : 1;                               // (a block past the waveform each way; recording: within its backdrop)
        for (int y = (MID_ROW - WAVE_REACH - reach) * PX; y < (MID_ROW + 2 + WAVE_REACH + reach) * PX; y++) {
          uint16_t &q = px[y * PX + sub];
          q = v.headColor != INVERT_HEAD ? v.headColor           // (on a coloured cell: black on it, it on black)
            : q == v.bg ? (v.bg == BG ? INK : BG) : v.bg;
        }
      }
    }
  }
  // SHIFT held on the track (its waveform cell framed in the accent colour:
  // LEFT/RIGHT step its sample): an arrow out of each side of the frame, at
  // its middle, the frame cleared a block round its tip.
  const bool switching = sampleSwitchShown(i);
  if (switching && selectedCellIn(i) != WAVE_CELL) {   // (another cell selected: the accent frame drawn here, the selected one's grey)
    if (bc == 0 || bc == BLOCK_COLS - 1) for (int r = 0; r < BAND_ROWS; r++) block(r, ACCENT);
    else { block(0, ACCENT); block(BAND_ROWS - 1, ACCENT); }
  }
  if ((bc <= 1 || bc >= BLOCK_COLS - 2) && switching) {
    const bool edge = bc == 0 || bc == BLOCK_COLS - 1;   // (the frame's column: the tip; the one inside it: the arrow's back)
    for (int r = MID_ROW - 2; r <= MID_ROW + 3; r++) {
      bool tip = r == MID_ROW || r == MID_ROW + 1, back = r >= MID_ROW - 1 && r <= MID_ROW + 2;
      if (edge) block(r, tip ? ACCENT : v.ground);
      else if (back) block(r, ACCENT);
    }
  }
  // SHIFT held (the frame's arrows above): the sample's label, black on an
  // accent-coloured box (a block over the text, two either side, its top corners cut), its
  // bottom row the bottom border's.
  if (v.label[0] && bc >= v.labelX && bc < v.labelX + v.labelW) {
    const bool side = bc == v.labelX || bc == v.labelX + v.labelW - 1;   // (its top corners cut)
    for (int r = LABEL_TOP + side; r < LABEL_TOP + 7; r++) block(r, ACCENT);
    int c = bc - v.labelX - 2;                     // (the text's column)
    bool count = false;                            // (past the '/': the count, never flashed)
    for (const char *p = v.label; *p && c >= 0; p++) {
      if (*p == '/') count = true;
      const uint16_t ink = v.labelFlash && !count ? DARK : BG;   // (a step: the name and number dark grey a moment)
      const Glyph *g = glyphFor(*p);
      int w = g ? glyphWidth(*g) : 2;
      if (c < w) {
        for (int r = 0; g && r < 5 && g->rows[r]; r++)
          if (g->rows[r][c] == '#') block(LABEL_TOP + 1 + r + g->top, ink);
        break;
      }
      c -= w + 1;
    }
  }
  auto &t = board.tft();
  int x = WAVE_X + bc * PX, top = rowTop(i);
  t.writeRect(x, top, PX, CELL_H, px);
}

void drawWave(int i, bool recordingHere) {
  const ShownTrack &tr = shownTrack(i);           // (as the row shows it: a switch all at once)
  WaveView &v = views[i];
  uint32_t rs = tr.rs, rb = tr.rb;
  uint16_t bg = cellBackground(i, WAVE_CELL);
  uint64_t borders = bordersOf(i, WAVE_CELL);
  // The range: light grey, white on the selected track (all its cells look
  // selected; the frame is on the selected one), the accent colour while B
  // alone is held on the selected cell (editing, muted or not); muted: grey.
  bool muted = tr.state == TrackState::Muted, here = shownSelected(i);
  bool editing = editingCell(i, WAVE_CELL);        // (B held, with other keys or not: as the filter cell)
  uint16_t rangeInk = editing ? ACCENT : muted ? DIM : here ? INK : LIGHT;
  if (recordingHere) rangeInk = INK;                 // the recording's waveform: white
  uint16_t ground = rowGround(i);
  uint16_t outInk = onGround(muted ? DARK : DIM, ground);   // outside it: grey (muted: dark grey; black on the selected track's ground)
  char label[24] = "";                             // (SHIFT held on the waveform cell: the sample's label)
  if (sampleSwitchShown(i)) sampleLabel(i, label, sizeof label);
  const bool switching = sampleSwitchShown(i) && selectedCellIn(i) != WAVE_CELL;   // (its own frame drawn: redrawn as it comes and goes)
  bool flash = label[0] && sampleSteppedAt && millis() - sampleSteppedAt < LABEL_FLASH_MS;
  bool labelSame = !strcmp(label, v.label) && flash == v.labelFlash && switching == v.ownFrame;
  v.ownFrame = switching;
  if (!labelSame) {
    v.labelFlash = flash;
    strcpy(v.label, label);
    v.labelW = textWidth(label) / PX + 4;          // (two blocks either side of the text)
    v.labelX = (BLOCK_COLS - v.labelW) / 2;
  }
  if (labelSame && v.drawn == uiGeneration && v.version == tr.overviewVersion && v.turn == tr.turn && v.state == (uint8_t)tr.state &&
      v.rec == recordingHere && v.rs == rs && v.rb == rb && v.bg == bg && v.ground == ground && v.borders == borders &&
      v.rangeInk == rangeInk && v.outInk == outInk)
    return;
  v.drawn = uiGeneration;
  v.version = tr.overviewVersion;
  v.turn = tr.turn;
  v.state = (uint8_t)tr.state;
  v.rec = recordingHere;
  v.rs = rs;
  v.rb = rb;
  v.bg = bg;
  v.ground = ground;
  v.borders = borders;
  v.rangeInk = rangeInk;
  v.outInk = outInk;
  v.live = recordingHere;
  v.visible = recordingHere || tr.state != TrackState::Empty;
  if (!v.visible || v.live) v.playPx = -1;
  v.markPx = -1;                                   // (every clip has one, muted or not)
  if (v.live) {
    v.liveBars = 0;                                // updatePlayheads() fills it in
    v.liveFilled = 0;
  } else if (v.visible) {
    uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
    // (Range edges, bar starts and the start position all on the same column
    // edges: the range's colour stops exactly at the bar lines.)
    int first = edgePx(rs, loopBeats), end = edgePx(rs + rb, loopBeats);   // (end unwrapped)
    v.rangeFirstPx = first % PX_COLS;
    v.rangePx = constrain(end - first, 1, PX_COLS);
    int mark = edgePx(startPosition(loopBeats, rs, rb), loopBeats) % PX_COLS;
    v.markPx = mark == 0 ? 0 : mark - LINE_W;      // (the pixels before its edge, as a bar line's: on the bar line at a bar start)
  }
  for (int bc = 0; bc < BLOCK_COLS; bc++) drawColumn(i, bc);
}

// Moves a view's playhead (or record head) to pixel column at (its first;
// -1: none) with the given look, redrawing only the block columns it leaves
// and covers.
static void movePlayhead(int i, int at, uint16_t color) {
  WaveView &v = views[i];
  if (at == v.playPx && color == v.headColor) return;
  int old = v.playPx;
  v.playPx = at;
  v.headColor = color;
  auto redraw = [&](int from) {                    // (the block columns a line from pixel `from` covers)
    if (from < 0) return;
    int a = from / PX, b = ((from + LINE_W - 1) % PX_COLS) / PX;
    drawColumn(i, a + WAVE_LEFT);
    if (b != a) drawColumn(i, b + WAVE_LEFT);
  };
  if (old != at) redraw(old);
  redraw(at);
}

// Live view of the recording: new columns as audio is written, a full
// redraw when the shown length grows, and the record head.
static void updateLive(int i) {
  WaveView &v = views[i];
  uint8_t cols[OVERVIEW_COLS];
  uint32_t bars, framesPerBar, recorded;
  if (!recordingWaveform(cols, bars, framesPerBar, recorded)) return;
  uint32_t total = bars * framesPerBar;
  int filled = min(WAVE_COLS, (int)(((uint64_t)recorded * WAVE_COLS + total - 1) / total));
  noInterrupts();
  int32_t sofar = (int32_t)(transport.now - recording.startSample);
  interrupts();
  int head = sofar < 0 ? -1 : min(PX_COLS - LINE_W, (int)((uint64_t)sofar * PX_COLS / total));   // (pixels)
  if (tracks[i].live) {
    // The loop already plays (the rest of the recording is still being
    // saved): its playhead instead of the record head.
    noInterrupts();
    double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;
    interrupts();
    double loopBeats = bars * BEATS_PER_BAR, beat = fmod(pos - recording.startBeat, loopBeats);
    if (beat < 0) beat += loopBeats;
    head = min(PX_COLS - LINE_W, (int)(beat * PX_COLS / loopBeats));
  }
  if (bars != v.liveBars) {                        // new scale: redraw everything
    v.liveBars = bars;
    v.liveFilled = filled;
    memcpy(v.liveData, cols, OVERVIEW_COLS);
    v.playPx = head;
    v.headColor = INVERT_HEAD;
    for (int bc = 0; bc < BLOCK_COLS; bc++) drawColumn(i, bc);
    return;
  }
  auto levels = [&](int c) { return (uint16_t)(pixelLevel(v.liveData, c * PX) << 8 | pixelLevel(v.liveData, c * PX + 1)); };   // (a block column's two pixel columns)
  uint16_t before[WAVE_COLS];
  for (int c = 0; c < WAVE_COLS; c++) before[c] = levels(c);
  memcpy(v.liveData, cols, OVERVIEW_COLS);
  int oldFilled = v.liveFilled;
  v.liveFilled = filled;
  for (int c = 0; c < WAVE_COLS; c++)
    if (levels(c) != before[c] || (c < filled) != (c < oldFilled)) drawColumn(i, c + WAVE_LEFT);
  movePlayhead(i, head, INVERT_HEAD);              // (on the coloured cell)
}

// Playhead column for each track from the transport position and the
// track's range alignment.
void updatePlayheads() {
  noInterrupts();
  bool running = transport.running;
  double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;   // in beats
  interrupts();
  for (int i = 0; i < NUM_TRACKS; i++) {
    const ShownTrack &tr = shownTrack(i);         // (the range shown: the old clip's until a switch is through)
    WaveView &v = views[i];
    // Recording started or ended: redraw the row now (its cells change too),
    // not on its next turn.
    if (v.live != recordingOn(i)) drawTrack(i);
    if (v.live) { updateLive(i); continue; }
    // The accent colour (white on a range in the accent colour); muted: black.
    bool muted = tr.state == TrackState::Muted;
    int at = -1;
    if (v.visible && running && tracks[i].live) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      // Spread over the range's pixels (as the range is drawn): never past its edges.
      uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
      int first = edgePx(tr.rangeStartBeat, loopBeats);
      int pxs = max(LINE_W, edgePx(tr.rangeStartBeat + tr.rangeBeats, loopBeats) - first);
      at = (first + min(pxs - LINE_W, (int)(inRange * pxs / tr.rangeBeats))) % PX_COLS;
    }
    movePlayhead(i, at, muted ? MUTED_HEAD : v.rangeInk == ACCENT ? INK : ACCENT);
  }
}
