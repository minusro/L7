// n9_song.h — the note store: one clip per grid cell (9 tracks x 8 scenes),
// one bar of sixteenth steps each for now (the design allows 16 bars; pages
// come later). The editor edits the clip at the grid cursor; the transport
// (n9_synth) plays that same clip. Edits happen in loop(), playback reads in
// the audio interrupt: fields are written in a safe order, nothing locks.
#pragma once
#include <stdint.h>

constexpr uint8_t CLIP_STEPS = 16, CLIP_MAX_NOTES = 64, CLIP_MAX_BARS = 16;
// The editor's pitch window is 13 rows; its top note moves by octaves.
constexpr uint8_t EDIT_TOP_PITCH = 60;

// Note starts are absolute sixteenths (bar * 16 + step, 0-255). A clip loops
// its own bar count, which follows the notes (polymeter between tracks is
// free); the editor shows one bar page at a time.
struct ClipNote { uint8_t pitch, vel, start, len; };
struct Clip { uint8_t count; uint8_t bars = 1; ClipNote notes[CLIP_MAX_NOTES]; };

extern Clip clips[9][8];

// The project's key (0 = C .. 11 = B) and scale. The piano roll only
// navigates in-scale rows and MIDI input snaps to the scale; notes already
// stored stay and play exactly as recorded.
constexpr uint8_t SCALE_COUNT = 12;
extern const char *const KEY_NAMES[12];
extern const char *const SCALE_NAMES[SCALE_COUNT];
extern uint8_t projKey, projScale;        // default C chromatic (no restriction)
bool scaleHas(uint8_t pitch);
uint8_t scaleSnap(uint8_t pitch);         // nearest in-scale pitch, ties downward

Clip &clipAt(uint8_t t, uint8_t s);
// The note covering (pitch, step), or -1.
int clipFindAt(const Clip &c, uint8_t pitch, uint8_t step);
// Adds a step-long note at (pitch, step), or removes the one covering it.
void clipToggle(Clip &c, uint8_t pitch, uint8_t step, uint8_t vel);
// Recomputes the bar count from the notes (after a length edit).
void clipRefreshBars(Clip &c);
