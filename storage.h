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
  uint32_t version, sample;                      // sample 0: empty track
  uint32_t rangeStart, rangeLen, rangePhase;     // beats; position in the range at transport beat 0, in quarters
  uint32_t muted;
  int8_t filterLo, filterHi, volume, balance;
};
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
// of ~60 ms is enough.
int32_t beginSceneChange(bool quick = false, bool prepared = false);
void endSceneChange();
bool sceneChangeWaiting();             // a scene change hasn't reached its switch point yet
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
void serviceStorage();               // call as often as possible from loop()

void transportStart();               // from bar 1, all tracks in sync
void transportStop();
void transportRestart();             // from bar 1 again, running or not
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
bool recordPress(int t, bool resample = false);
int recordQueued();                   // the track recording next, or -1
bool recordQueuedResample();          // and it resamples
// A resampling saved since the last call: its track (once), or -1. The other
// tracks stay quiet until their clips change (the scene after it), or
// endSilencing().
int takeResampled();
void endSilencing();
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
// The track's sample: the one recorded next (dir 1) or before (-1), wrapping
// around; an empty track takes the oldest (1) or newest (-1). Needs at least
// 2 samples on the card. Volume, balance, filter and mute stay; the range resets to
// the whole loop. false: nothing changed.
bool stepSample(int t, int dir);
// Samples on the card holding recordings (0001-9999), including ones no
// track plays.
uint32_t sampleCount();
bool sampleExists(uint32_t n);
// Loop range in beats: starts on any beat of the loop; any length from a beat
// up to the whole loop. It may run past the loop's end and carry on from its
// start. While playing, the change is queued and takes over at
// the next quarter beat the track hasn't buffered yet (usually within a beat);
// trackRange() reports the queued range if any.
bool setRange(int t, uint32_t startBeat, uint32_t beats);
void trackRange(int t, uint32_t &startBeat, uint32_t &beats);
// The range's start position (a beat of the loop): for a range of a beat,
// its start; shorter than a bar, the first bar start or middle in it; a bar
// or longer, the first bar start in it. The transport starting at 1.1.1
// plays each track from here.
uint32_t startPosition(uint32_t loopBeats, uint32_t rangeStart, uint32_t rangeBeats);

// The session: the tempo, the loaded project (0-11) and the master volume, in
// session.txt. (row/cell: the selection, saved there before projects kept it;
// -1: none.)
// menu: the menu open at power off (its selection, ui_menu.cpp menuState()), or -1.
struct Session { float bpm = 120.0f; int row = -1, cell = -1, master = MASTER_VOLUME_UNITY, project = 0, menu = -1; };
Session loadSession();
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
