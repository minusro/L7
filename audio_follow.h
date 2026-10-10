// audio_follow.h — audio follow: the tempo and the beat heard in the input
// (the line input and USB audio, as monitored), followed by the clock.
//
// On (SHIFT+A on the tempo cell; saved with the session), the interrupt measures
// how sharply the input gets louder, block by block (engine.h onsetRing).
// From that, every half second, the tempo: the autocorrelation of the last
// ~6 s, each candidate scored by its own lag and three multiples (a beat's
// pattern repeats every beat, two, three, four), weighted towards the tempo
// already set (or followed), so a beat isn't taken for its half or double.
// Then the beat: a tracker that expects each one a period after the last,
// looks for the sharpest rise near there, and moves its beat and its period a
// little towards it (none: carried on). Its beats found often enough: locked.
//
// The clock follows: stopped, the tempo is theirs as soon as it's found (and
// as the tracker refines it, beat by beat: ready to start); running, once
// their beat is found (locked), the tempo and nudged to their beat once a beat, as
// an external MIDI clock's (midi_in.cpp): a little faster or slower for the next
// beat, the beat as heard from them (the input's latency, inputLatency(),
// taken off) on our beat. Beats only: the bars are where START put them. START
// while locked starts on their beat: pressed in the first half of their beat,
// at once, that far into it; in the second half, on their next beat.
//
// Meanwhile the tempo can't be changed by hand, nor by a project: B+UP/DOWN only
// halve or double it (audioFollowScale()), the beat staying theirs. A MIDI clock
// being followed comes first (audio follow waits, its tempo theirs).
#pragma once
#include <stdint.h>

void audioFollowBegin(bool on);          // the session's setting
bool audioFollowOn();                    // the mode
void audioFollowToggle();
bool audioFollowActive();                // on, and no MIDI clock followed meanwhile
bool audioFollowLocked();                // their beat found (active)
void audioFollowService();               // loop(): the tempo, the beat, the clock
bool audioFollowStart();                 // START pressed: true if it starts (now or on their next beat) in step with them
// The beat to flash on (active): true in the first half of each beat (running:
// ours; stopped: theirs, found or still being looked for; nothing found yet:
// the tempo shown's); false when not active.
bool audioFollowFlash();
float audioFollowBpm();                  // their tempo as found, halved or doubled as set (0: none yet)
// B+UP/DOWN on the tempo cell while on: the clock at their tempo doubled (by > 0)
// or halved (by < 0), again and again (within the clock's range; 0 again when
// turned on). Their tempo not found yet: the tempo shown halved or doubled
// instead, the estimate leaning towards it (so it finds that octave). False:
// not on, or out of range.
bool audioFollowScale(int by);
void audioFollowShift(double beats);     // tap tempo while following: the clock eased this many beats later (negative: earlier), its bar onto the taps
bool audioFollowShifting();              // one under way
void audioFollowOffsetChanged();         // (midi_in.h syncOffsetMs[SYNC_AUDIO] changed: caught up quickly)
// The external tempo source followed now (midi_in.h SyncSource): a MIDI
// clock (serial or USB), else audio follow (active), else SYNC_NONE.
int syncSource();
