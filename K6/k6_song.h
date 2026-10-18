// k6_song.h — the note store: one clip per grid cell (9 tracks x 8 scenes),
// one bar of sixteenth steps each for now (the design allows 16 bars; pages
// come later). The editor edits the clip at the grid cursor; the transport
// (k6_synth) plays that same clip. Edits happen in loop(), playback reads in
// the audio interrupt: fields are written in a safe order, nothing locks.
#pragma once
#include <stdint.h>

constexpr uint8_t CLIP_STEPS = 16, CLIP_MAX_NOTES = 128, CLIP_MAX_BARS = 16;
// The editor's pitch window is 13 rows; its top note moves by octaves.
constexpr uint8_t EDIT_TOP_PITCH = 60;

// Note starts are absolute sixteenths (bar * 16 + step, 0-255). A clip loops
// its own bar count, which follows the notes (polymeter between tracks is
// free); the editor shows one bar page at a time.
struct ClipNote { uint8_t pitch, vel, start, len; };
// used: the slot holds a clip (A on an empty slot creates one; it may still
// have no notes). Adding a note in the editor marks the slot used too.
// barsFixed: 0 = Auto (bars follow the notes, up to 16), else 1/2/4/8/16
// (B+UP/DOWN on the grid steps the ladder).
struct Clip { uint8_t used; uint8_t count; uint8_t bars = 1; uint8_t barsFixed = 0; ClipNote notes[CLIP_MAX_NOTES]; };

// The clips live in RAM2 (DMAMEM: no startup init there - songInit() does it);
// the sequencer reads them at step rate only, so the slower RAM is free.
extern Clip clips[9][8];
void songInit();   // zero the clips, every bar count to 1 (call before audio)

// Clip launching, session style: per track, the playing scene (-1: none) and
// what's pending for the next bar line (-1: nothing, CLIP_STOP: stop, else a
// scene to start from its top). Pendings are set in loop() (the grid's A),
// applied and consumed in the audio interrupt at each bar line (k6_synth
// fireStep); clipPlaying persists over a stopped transport, so PLAY resumes
// the same clips from their tops.
constexpr int8_t CLIP_STOP = -2;
extern volatile int8_t clipPlaying[9];
extern volatile int8_t clipPending[9];
// clipFreeze: the sequencer keeps off the clips (a project load is swapping
// them); clipTakeNow: pendings apply on the next step, not the bar line (a
// project came in on the beat).
extern volatile uint8_t clipFreeze, clipTakeNow;

// SHIFT+A on the grid: a non-empty clip cuts here, an empty one pastes.
extern Clip clipClipboard;
extern bool clipClipFull;
extern bool clipClipPlaying;   // the cut clip was playing: the paste resumes it

// SHIFT+A in the editor: the selected notes, starts relative to the first.
extern ClipNote noteClipboard[CLIP_MAX_NOTES];
extern uint8_t noteClipCount;

// The project's key (0 = C .. 11 = B) and scale. The piano roll only
// navigates in-scale rows and MIDI input snaps to the scale; notes already
// stored stay and play exactly as recorded.
constexpr uint8_t SCALE_COUNT = 12;
extern const char *const KEY_NAMES[12];
extern const char *const SCALE_NAMES[SCALE_COUNT];
extern uint8_t projKey, projScale;        // default C chromatic (no restriction)
bool scaleHas(uint8_t pitch);
uint8_t scaleSnap(uint8_t pitch);         // nearest in-scale pitch, ties downward

// The mixer: per-track volume (80 = unity), pan (50 = centre) and mute,
// and the master volume (80 = unity), ahead of the soft clipper.
extern uint8_t trackVol[9], trackPan[9];
extern bool trackMute[9];
extern uint8_t masterVol;

Clip &clipAt(uint8_t t, uint8_t s);
// The note covering (pitch, step), or -1.
int clipFindAt(const Clip &c, uint8_t pitch, uint8_t step);
// A note's audible length: the written one, cut short where the same pitch
// starts again (cyclically over the clip's bars). Drawing, editing and
// playback all use this; the written length stays, so removing the blocker
// gives the tail back.
uint8_t clipEffLen(const Clip &c, uint8_t i);
// Adds a step-long note at (pitch, step), or removes the one covering it.
void clipToggle(Clip &c, uint8_t pitch, uint8_t step, uint8_t vel);
// Recomputes the bar count from the notes (after a length edit).
void clipRefreshBars(Clip &c);
