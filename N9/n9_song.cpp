// n9_song.cpp — see n9_song.h.
#include "n9_song.h"

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

int clipFindAt(const Clip &c, uint8_t pitch, uint8_t step) {
  for (int i = 0; i < c.count; i++) {
    const ClipNote &n = c.notes[i];
    if (n.pitch == pitch && step >= n.start && step < n.start + n.len) return i;
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
  int last = 0;
  for (int i = 0; i < c.count; i++) {
    int end = c.notes[i].start + c.notes[i].len;
    if (end > last) last = end;
  }
  uint8_t bars = (uint8_t)((last + 15) / 16);
  c.bars = bars < 1 ? 1 : bars > CLIP_MAX_BARS ? CLIP_MAX_BARS : bars;
}
