// storage.h — SD side of the looper: streaming tracks into their rings,
// writing recordings, and keeping tracks/tempo across power cycles.
// Everything here runs from loop(), never from an interrupt.
#pragma once
#include "engine.h"
#include "scenes.h"

namespace looper {

// A track's clip and settings: its sample, loop range and alignment, mute,
// filter, volume and balance. Saved in its unit file; scenes hold one per track.
struct TrackUnit {
  char magic[4];                                 // "M8TU"
  // The version in the low 16 bits; in the high 16 the clip's step offset
  // (quarter beats, the part of `rotate` taken in whole quarters: shown as the
  // range, start, playhead and bar lines moved, exported as the loop's start
  // moved). Units saved before it: 0 there (a small version number).
  uint32_t version : 16, steps : 16;
  uint32_t sample;                               // sample 0: empty track
  uint32_t rangeStart, rangeLen, rangePhase;     // beats; position in the range at transport beat 0, in quarters
  // Muted (0/1), and the clip's fine rotate: turned this many frames later
  // (its audio as it was; < its loop's frames). The rotate in the bits a
  // 32-bit mute left unused: units saved before it read 0.
  uint32_t muted : 8, rotate : 24;
  int8_t filterLo, filterHi, volume, balance;
};
static_assert(sizeof(TrackUnit) == 32, "a unit's size as saved (scenes, headers): unchanged");
TrackUnit unitOf(int t);
// Units before version 2 kept volume 0-32 (unity 28, 1.5 dB steps) and
// balance -16..16: brought to today's steps (config.h).
void upgradeUnit(TrackUnit &u);
// Puts unit u on track t. Stopped: at once. Playing: a track keeping its clip
// and range plays on; one with another clip or range switches to it with no
// gap on the next quarter beat with time to load (scene changes: see below).
// false: its sample is gone (the track is left empty) or it's recording.
bool setTrackUnit(int t, const TrackUnit &u);
// Scene changes while playing: the setTrackUnit() calls between these wait
// for one bar line, the first at least ~150 ms ahead (time to open the new
// samples and read their starts), and take effect together there. Stepping on
// before it reuses it (unless it's under ~50 ms away). quick: the next beat
// instead (a change already waiting moves up to it). Returns that point's
// transport beat (-1: stopped, at once).
// prepared: the scene's samples were opened ahead (prepareUnits()): a lead
// of ~60 ms is enough. restart (a project switch): at that bar line every
// track starts over from its 1.1.1 position (as transportStart() places it),
// and the clock reads 1.1.1 there (transportOrigin()).
int32_t beginSceneChange(bool quick = false, bool prepared = false, bool restart = false, bool clockToo = true);   // restart: every clip from its start position there (clockToo: the clock from 1.1.1 too: a project switch)
// The transport beat the clock counts from (its 1.1.1): 0, or where a
// project switch while playing started over.
int32_t transportOrigin();
void endSceneChange();
bool sceneChangeWaiting();             // a scene change hasn't reached its switch point yet
int32_t sceneSwitchBeat();             // and the transport beat it switches on (INT32_MIN: none waiting)
// Preparing a scene ahead (selected while SHIFT is held): the units' sample
// files are opened in the background, one per pass, so the change can launch
// sooner. nullptr: drop it. unitsPrepared(): all opened (false when nothing
// is being prepared).
void prepareUnits(const TrackUnit *units);
bool unitsPrepared();
// A write may go to the card now (stopped, or the playback buffers are full);
// false also asks for the buffers to be topped up first.
bool storageMayWrite();

// Mount SD, load saved tracks; prepares recording space the first time
// (progress messages go to `progress`). false = no card / no space.
bool storageBegin(void (*progress)(const char *));
// Drops everything held from the card without touching it (storage mode: the
// computer may have changed any file), the tracks emptied; storageBegin() and
// scenesBegin() then read it all again. Stopped and not recording only.
void storageForget();
void serviceStorage();               // call as often as possible from loop()

void transportStart();               // from bar 1, all tracks in sync
// From that beat (the clock reads it; each track where it'd be then: an
// external clock's song position); inBeat (asked once the tracks are filled):
// samples already into that beat, so it starts exactly where the clock is.
void transportStartAt(int32_t beat, uint32_t (*inBeat)() = nullptr);
void transportStop();
void transportRestart();             // from bar 1 again, running or not
// The beat the clock last stopped in (as it read: 0 is 1.1.1), for resuming
// from it (SHIFT+START); back to 1.1.1 when a project loads.
int32_t transportResumeBeat();
void transportForgetPosition();
// Running (and not recording): jump to the nearest bar start (the playheads
// follow, in phase); the bar count carries on from there.
void transportSnapToBar();

// Record button for track t: start (or arm until the next bar), then stop.
// Returns false if the track already holds a loop. Pressed on another track
// while one is recording: that one stops and this one records next (pressed
// again: not any more).
// resample: record the app's output (everything heard) instead of the input;
// it starts on a bar line (the next one, or bar 1 when stopped). When it's
// stopped, the other tracks playing go quiet where its loop ends.
// heldFor: samples since A was pressed (it records once let go): the late
// press's window counts from the press (as far as the input history allows).
bool recordPress(int t, bool resample = false, uint32_t heldFor = 0);
int recordQueued();                   // the track recording next, or -1
bool recordQueuedResample();          // and it resamples
// A resampling saved since the last call: its track (once), or -1. The other
// tracks stay quiet until their clips change (the scene after it), or
// endSilencing().
int takeResampled();
void endSilencing();
// The letter recordings made from now on are named with (the loaded project's,
// 'A'-'L'; 0: none): "A30012.wav", with the track's number (1-7, as on the screen).
void setRecordingLetter(char letter);
void hearAgain(int t);                // track t no longer silenced (it keeps its clip in the scene after it)
// Track t recording (or waiting to, or queued): stop at once and throw the
// recording away (its sample file goes back to the pool of empty ones; a
// queued recording starts from the next bar line instead).
bool cancelRecording(int t);
void clearTrack(int t);               // its sample stays on the card
// Clipboard: a track's sample and settings. cutTrack copies a track there and
// clears it; pasteTrack puts it on an empty track (it stays on the clipboard).
bool cutTrack(int t);
bool pasteTrack(int t);
bool clipboardFull();
void clearClipClipboard();          // emptied
// Tracks a and b swap their units (a clip, its range, mute, filter, volume,
// balance): at once while stopped, else both on the next quarter beat after
// the lead, each clip in step with the clock as it was (no gap); the scene
// follows. Not with a recording on (or queued for) either.
bool swapTracks(int a, int b);
// Track t's clip turned `by` frames later (negative: earlier), round its loop
// (its audio as it was: TrackUnit's rotate): at once while stopped, else on
// the next quarter beat after the lead (no gap); the scene follows. false:
// no clip (or another on its way).
bool rotateTrack(int t, int32_t by, int32_t bySteps = 0);   // by: frames in all; bySteps: of them, whole quarter beats (the step offset)
// Samples are known by an id: the name's tag (project letter, track) and
// number ("A30012.wav"; older ones: the number alone, "0012.wav").
// Marks on the samples in use (clearSampleMarks(), markSample() each), then
extern void (*deleteProgress)(int done, int total);   // called as deleteUnmarkedSamples() goes through the files (nullptr: none)
// deleteUnmarkedSamples() deletes the recordings not marked and not on a
// track, the clipboard or recording; stopped only (file-system changes);
// returns how many (the marks cleared).
void clearSampleMarks();
void markSample(uint32_t id);
bool sampleMarked(int i);             // sampleAt(i)'s mark
int deleteUnmarkedSamples(const uint32_t *only = nullptr, int onlyCount = 0);   // (only: just those of these ids)
// The track's sample: the next (dir 1) or previous (-1) in the order of their
// names, wrapping around; an empty track takes the first (1) or last (-1) of
// its own in the loaded project (or the name after / before). Needs at least
// 2 samples on the card. Volume, balance, filter and mute stay; the range resets to
// the whole loop. false: nothing changed. Empty recordings (EMPTY_SPAN,
// config.h) are stepped over, and noted: takeEmptySamples() hands them on
// (to be deleted if nothing uses them, scenes.h deleteEmptySamples()).
bool stepSample(int t, int dir);
int silentSamples(uint32_t *ids, int most);   // the silent recordings on the card (each one read: slow; deleteProgress meanwhile)
char sampleLetter(uint32_t id);          // its name's first character (a project's letter; an older one's: a digit)
bool sampleFirstOfLetter(uint32_t id);   // the first, in L7's order, of the samples whose names start so
bool sampleLetterEndsAt(uint32_t id, int dir);   // the next one that way starts with another letter
// Track t's sample as SHIFT+LEFT/RIGHT shows it: its name and its place
// among all of them ("92.6 A013 13/24": its tempo, its name, its place; none: "", no label).
void sampleLabel(int t, char *buf, size_t size);
// The loaded project's sample last stepped to (SHIFT+LEFT/RIGHT) or recorded,
// per project (A-L: 0-11; kept in the session): an empty track's SHIFT+LEFT
// goes to it, SHIFT+RIGHT to the one after it.
extern uint32_t touchedSample[12];
void noteTouched(uint32_t id);       // the loaded project's (none loaded: nothing)
int takeEmptySamples(uint32_t *ids, int most);
// Samples on the card holding recordings, including ones no track plays now
// (until deleteUnusedSamples(), scenes.h: no project's), in the order of their
// names: sampleAt(0 .. sampleCount() - 1).
uint32_t sampleAt(int i);
void sampleFile(uint32_t id, char *path, size_t size);   // its file now ("/L7/SAMPLES/A30012.wav"; tests)
bool moveFile(const char *from, const char *to);        // renames a file (an older card's layout) unless `to` exists; true: moved
// For the Live Set export (ableton.cpp; stopped): sample n's details (its
// name: "A30012", or "0012" for an older one) and a copy of its file.
// bars: each bar line's frame (bars 1 .. loopBars; the last the clip's end):
// where they fell when the tempo changed while it recorded, else evenly.
struct SampleDetails { uint32_t frames, spb, loopBars; uint64_t bytes; char name[12]; uint32_t bars[MAX_BARS]; float headroomDb; uint8_t profile[PROFILE_BYTES]; };   // (headroomDb: its volume scale, engine.h)
bool sampleDetails(uint32_t n, SampleDetails &d);
bool copySample(uint32_t n, const char *to, void (*progress)() = nullptr);
bool copySampleDoubled(uint32_t n, uint32_t frames, const char *to, void (*progress)() = nullptr);   // its loop twice over, a plain WAV (the export)
uint32_t sampleCount();
bool sampleExists(uint32_t n);
// Loop range in beats: starts on any beat of the loop; any length from a beat
// up to the whole loop. It may run past the loop's end and carry on from its
// start. While playing, the change is queued and takes over at
// the next quarter beat the track hasn't buffered yet (usually within a beat);
// its start moved (its length as it was), the playhead moves with it (as far
// into the new range as into the old). trackRange() reports the queued range if any.
bool setRange(int t, uint32_t startBeat, uint32_t beats);
void trackRange(int t, uint32_t &startBeat, uint32_t &beats);
// A change on its way to track t (a scene's, a project's, a move): requested,
// or its clip switch planned or under way in the stream, until the switch
// point has passed and the track has taken it on.
bool trackChangeOnWay(int t);
// For showing a scene before it plays (ui.cpp): the unit on its way to track
// t (a change, a planned switch; none: its own), and a clip's looks (sample
// on track t: playing, switched to, or prepared; false: not opened yet).
TrackUnit unitOnWay(int t);
struct ClipLooks { const uint8_t *overview; uint32_t loopBars, spb; };
bool clipLooks(int t, uint32_t sample, ClipLooks &c);
// The range's start position (a beat of the loop): for a range of a beat,
// its start; shorter than a bar, the first bar start or middle in it; a bar
// or longer, the first bar start in it. The transport starting at 1.1.1
// plays each track from here.
uint32_t startPosition(uint32_t loopBeats, uint32_t rangeStart, uint32_t rangeBeats);
// The grid start positions snap to, in beats (1, half a bar or a bar).
uint32_t startGrid(uint32_t loopBeats, uint32_t rangeBeats);

// The session: the tempo, the loaded project (0-11; -1: the dummy one) and the master volume, in
// SESSION.DAT. row/cell: the selection (the app's; -1: none saved).
// menu: the menu's selection, + 1000 if it was open (ui_menu.cpp menuState()); -1: none saved.
// playing: the clock was running (when last saved: at power-off, or stopped).
struct Session {
  float bpm = 120.0f, newBpm = 120.0f;
  int row = -1, cell = -1, master = MASTER_VOLUME_UNITY, balance = 0, project = 0, menu = -1;
  bool playing = false;
  int metro = 1;                                   // the metronome: 0 off, 1 auto, 2 on (app.h)
  bool follow = false;                             // audio follow (audio_follow.h)
  int octaves[3] = {0, 0, 0};                      // the external clocks' tempo multipliers (powers of two): serial MIDI, USB MIDI, audio
  int offsets[3] = {0, 0, 0};                      // the external clocks' offsets, ms (midi_in.h syncOffsetMs: serial MIDI, USB MIDI, audio)
  bool settings[4] = {true, true, true, true};     // app.h's settings: monitor, play new, next free, empty project click
  int helpSeen = 0, helpPage = 0;                  // the help pages seen (a bit each) and the one last shown (app.h)
  uint32_t touched[12] = {};                       // each project's sample last stepped to or recorded (0: none; touchedSample())
};
Session loadSession();
// A new recording plays once it's done (false: it comes in muted; app.h's setting).
extern bool newClipsPlay;
void saveSession(const Session &s);  // creates/rewrites a file: only while stopped
void trackChanged(int t);            // settings changed: header saved when buffers allow
// Mute (a change on its way to the track included).
void setTrackMuted(int t, bool muted);
bool trackMuted(int t);
void flushHeaders();                 // save all pending headers now (power off)

// Live waveform of the recording in progress (OVERVIEW_COLS columns over
// `bars` bars: the final loop length once stopped, before that the bars
// started so far). false = not recording.
bool recordingWaveform(uint8_t *cols, uint32_t &bars, uint32_t &framesPerBar, uint32_t &recordedFrames);

// Debugging: the step loop() is in, for the freeze detector in the sketch.
extern volatile const char *debugStage;

}  // namespace looper
