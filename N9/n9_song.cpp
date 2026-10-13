// n9_song.cpp — see n9_song.h.
#include "n9_song.h"

Clip clips[9][8];

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
    return;
  }
  if (c.count >= CLIP_MAX_NOTES) return;
  ClipNote &n = c.notes[c.count];     // fields first, the count last
  n.pitch = pitch; n.vel = vel; n.start = step; n.len = 1;
  c.count++;
}
