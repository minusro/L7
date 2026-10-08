// midi_in.h — following an external MIDI clock on the serial MIDI in
// (config.h MIDI_IN_ENABLED; the port: M8Board.h midiIn).
//
// Clock (0xF8, 24 a beat): while their transport runs (from their Start or
// Continue until their Stop, or until no tick for FOLLOW_TIMEOUT_MS) the
// tempo is theirs. Their ticks while they're stopped (some send clock all
// the time) are timed but not followed: the tempo stays free to change, and
// their Start takes theirs at once. Running, the transport is also kept on their beat: once a beat, the
// difference (averaged over the beat's ticks) is taken out by running a
// little faster or slower for the next one (half of it a beat, at most
// MAX_NUDGE either way), on top of their tempo. Start (0xFA): the transport
// from 1.1.1, on their next tick; Continue (0xFB): from their latest Song
// Position (0xF2, in 16ths; none: 1.1.1), starting on their next whole beat
// (transportStartAt(): the clock reads it, the tracks where they'd be then);
// a Song Position while they run: a jump there, the same way. Running
// already: stopped first. Stop (0xFC): the transport stops. Their position
// known, our beat is kept on theirs bars and all.
// Started here instead (START), only the beats are kept in step.
// Meanwhile the tempo can't be changed by hand (tap tempo, B + arrows: no
// effect), and a project's own tempo doesn't apply.
#pragma once
#include <stdint.h>

class HardwareSerial;

void midiInBegin(HardwareSerial *port);   // reads it from then on (the MIDI timer's interrupt)
// Where a byte came from: the serial MIDI in, or USB MIDI (midi_usb.cpp). With
// clocks on both, the one ticking first is followed until it stops (no tick
// for FOLLOW_TIMEOUT_MS); the other's ticks, starts and stops are ignored meanwhile.
enum MidiSource : uint8_t { MIDI_SERIAL = 0, MIDI_USB = 1 };
void midiInByte(uint8_t b, uint32_t us, MidiSource from = MIDI_SERIAL);   // a byte received at micros() us (from an interrupt; the simulator feeds it)
void midiInSuspend(bool off);             // storage mode: nothing taken in meanwhile
void midiInSongPosition(uint16_t pos16, uint32_t us, MidiSource from);   // a Song Position (0xF2; in 16ths): USB's, already put together
void midiInService();                     // loop(): start / stop, the tempo and the beat followed
bool midiClockFollowing();                // an external clock is running (its Start had, its ticks coming in)
int midiFollowSource();                   // the clock followed (MidiSource; meaningful while midiClockFollowing())
// External tempo sources (audio_follow.h syncSource()): each one's offset of
// our clock from theirs, in ms (positive: L7 plays later; 0 by default),
// SYNC_OFFSET_STEP_MS a press of SHIFT+RIGHT / LEFT on the status row,
// within +-SYNC_OFFSET_MAX_MS; saved with the session. syncOffsetChanged():
// after a change, the follower catches it up quickly.
enum SyncSource : int8_t { SYNC_NONE = -1, SYNC_SERIAL = 0, SYNC_USB = 1, SYNC_AUDIO = 2 };
constexpr int SYNC_SOURCES = 3, SYNC_OFFSET_MAX_MS = 250, SYNC_OFFSET_STEP_MS = 10;
extern int16_t syncOffsetMs[SYNC_SOURCES];
void syncOffsetChanged(int source);
uint32_t midiTheirSpb();                  // their tempo as measured (samples a beat; 0: none yet)

// From the app (app.cpp): the tempo shown, and an external clock's tempo
// taken on (spb exactly, nudged to its beat; shownBpm the tempo shown).
extern float bpm;
void followTempo(uint32_t spb, float shownBpm);
