// k6_song.cpp — see k6_song.h.
#include "k6_song.h"

#include <Arduino.h>
#include <string.h>

DMAMEM Clip clips[9][8];

void songInit() {
  memset(clips, 0, sizeof clips);
  for (int t = 0; t < 9; t++)
    for (int s = 0; s < 8; s++) clips[t][s].bars = 1;
}

volatile int8_t clipPlaying[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
volatile int8_t clipPending[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
Clip clipClipboard;
bool clipClipFull = false;
bool clipClipPlaying = false;
ClipNote noteClipboard[CLIP_MAX_NOTES];
uint8_t noteClipCount = 0;
volatile uint8_t clipFreeze = 0, clipTakeNow = 0;

const char *const KEY_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char *const SCALE_NAMES[SCALE_COUNT] = {
    "CHROM", "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN",
    "MIXOLYD", "LOCRIAN", "HARM MIN", "PENT MAJ", "PENT MIN", "BLUES"};
// Semitone masks from the key's root (bit 0 = the root).
static const uint16_t SCALE_MASKS[SCALE_COUNT] = {
    0xFFF,                                    // chromatic
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),    // major
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),    // natural minor
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),    // dorian
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),    // phrygian
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),    // lydian
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),    // mixolydian
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),    // locrian
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),    // harmonic minor
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                           // major pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                          // minor pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),               // blues
};
uint8_t projKey = 0, projScale = 0;

uint8_t trackVol[9] = {80, 80, 80, 80, 80, 80, 80, 80, 80};
uint8_t trackPan[9] = {50, 50, 50, 50, 50, 50, 50, 50, 50};
bool trackMute[9];
bool trackSolo[9];
uint8_t masterVol = 80;

bool scaleHas(uint8_t pitch) {
  return SCALE_MASKS[projScale] & (1 << ((pitch + 12 - projKey) % 12));
}

uint8_t scaleSnap(uint8_t pitch) {
  for (int d = 0; d <= 6; d++) {            // nearest, ties downward
    if (pitch >= d && scaleHas(pitch - d)) return pitch - d;
    if (pitch + d < 128 && scaleHas(pitch + d)) return pitch + d;
  }
  return pitch;
}

Clip &clipAt(uint8_t t, uint8_t s) { return clips[t][s]; }

uint8_t clipEffLen(const Clip &c, uint8_t i) {
  int span = c.bars * 16;
  const ClipNote &n = c.notes[i];
  int best = n.len;
  for (int k = 0; k < c.count; k++) {
    if (k == i || c.notes[k].pitch != n.pitch) continue;
    int d = (c.notes[k].start - n.start + span) % span;
    if (d && d < best) best = d;
  }
  return (uint8_t)(best < 1 ? 1 : best);
}

int clipFindAt(const Clip &c, uint8_t pitch, uint8_t step) {
  int span = c.bars * 16;
  for (int i = 0; i < c.count; i++) {
    const ClipNote &n = c.notes[i];
    if (n.pitch != pitch) continue;
    int len = clipEffLen(c, i);
    if (step >= n.start && step < n.start + len) return i;
    if (n.start + len > span && step < n.start + len - span) return i;   // its wrapped tail
  }
  return -1;
}

void clipToggle(Clip &c, uint8_t pitch, uint8_t step, uint8_t vel) {
  int i = clipFindAt(c, pitch, step);
  if (i >= 0) {                       // remove: the last note fills the hole
    c.notes[i] = c.notes[c.count - 1];
    c.count--;
    clipRefreshBars(c);
    return;
  }
  if (c.count >= CLIP_MAX_NOTES) return;
  ClipNote &n = c.notes[c.count];     // fields first, the count last
  n.pitch = pitch; n.vel = vel; n.start = step; n.len = 1;
  c.count++;
  c.used = 1;                         // an edit in an empty slot creates the clip
  clipRefreshBars(c);
}

void clipRefreshBars(Clip &c) {
  if (c.barsFixed) { c.bars = c.barsFixed; return; }
  uint8_t top = 0;
  for (uint8_t i = 0; i < c.count; i++) {
    uint8_t e = c.notes[i].start + c.notes[i].len;
    if (e > top) top = e;
  }
  uint8_t bars = (top + 15) / 16;
  if (bars < 1) bars = 1;
  if (bars > CLIP_MAX_BARS) bars = CLIP_MAX_BARS;
  c.bars = bars;
}

// ---- the SONG arranger --------------------------------------------------------

#include "k6_state.h"   // NT/NS
#include "k6_synth.h"

SongRow songRows[SONG_MAX];
uint8_t songLen = 0;
bool songOn = false;
uint8_t songPos = 0;
uint16_t songBarsLeft = 0;

uint8_t songSceneSpan(uint8_t s) {
  uint8_t span = 1;
  for (int t = 0; t < NT; t++) {
    const Clip &c = clipAt((uint8_t)t, s);
    if (c.used && c.bars > span) span = c.bars;
  }
  return span;
}

uint16_t songRowBars(uint8_t i) {
  if (i >= songLen) return 1;
  uint16_t b = (uint16_t)songRows[i].rep * songSceneSpan(songRows[i].scene);
  return b ? b : 1;
}

static void songSceneNow(uint8_t s) {            // instant, riding the clock
  for (int t = 0; t < NT; t++) {
    clipPending[t] = -1;                         // a queued switch is overtaken
    clipPlaying[t] = clipAt((uint8_t)t, s).used ? (int8_t)s : -1;
  }
}

void songQueueNext() {                           // pendings land on the next bar line
  uint8_t next = songLen ? (uint8_t)((songPos + 1) % songLen) : 0;
  uint8_t s = songRows[next].scene;
  for (int t = 0; t < NT; t++)
    clipPending[t] = clipAt((uint8_t)t, s).used ? (int8_t)s : CLIP_STOP;
}

void songLaunch(uint8_t row) {
  if (!songLen) return;
  if (row >= songLen) row = 0;
  songSceneNow(songRows[row].scene);
  songPos = row;
  songBarsLeft = songRowBars(row);
  songOn = true;
  if (!synth.seqOn()) synth.seqToggle();
  if (songBarsLeft == 1) songQueueNext();
}

void songStop() {
  if (!songOn) return;
  songOn = false;
  for (int t = 0; t < NT; t++) clipPending[t] = -1;   // a queued row switch dies with it
}

bool songService() {
  static uint16_t lastBar = 0xFFFF;
  if (!synth.seqOn()) { lastBar = 0xFFFF; return false; }
  uint16_t bar = synth.seqBarN();
  if (bar == lastBar) return false;
  bool fresh = lastBar == 0xFFFF;                // just started/resumed: no bar elapsed
  lastBar = bar;
  if (!songOn || !songLen || fresh) return songOn && fresh;
  if (songBarsLeft > 1) {
    songBarsLeft--;
    if (songBarsLeft == 1) songQueueNext();
    return true;
  }
  // the queued pendings took effect at this bar line: the next row is on
  songPos = (uint8_t)((songPos + 1) % songLen);
  songBarsLeft = songRowBars(songPos);
  if (songBarsLeft == 1) songQueueNext();
  return true;
}
