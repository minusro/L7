// n9_song.h — the note store: one clip per grid cell (9 tracks x 8 scenes),
// one bar of sixteenth steps each for now (the design allows 16 bars; pages
// come later). The editor edits the clip at the grid cursor; the transport
// (n9_synth) plays that same clip. Edits happen in loop(), playback reads in
// the audio interrupt: fields are written in a safe order, nothing locks.
#pragma once
#include <stdint.h>

constexpr uint8_t CLIP_STEPS = 16, CLIP_MAX_NOTES = 64;
// The editor's pitch window: 13 rows, C4 on top down to C3.
constexpr uint8_t EDIT_TOP_PITCH = 60;

struct ClipNote { uint8_t pitch, vel, start, len; };
struct Clip { uint8_t count; ClipNote notes[CLIP_MAX_NOTES]; };

extern Clip clips[9][8];

Clip &clipAt(uint8_t t, uint8_t s);
// The note covering (pitch, step), or -1.
int clipFindAt(const Clip &c, uint8_t pitch, uint8_t step);
// Adds a step-long note at (pitch, step), or removes the one covering it.
void clipToggle(Clip &c, uint8_t pitch, uint8_t step, uint8_t vel);
