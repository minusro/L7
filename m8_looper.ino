// m8_looper.ino — 8-track stereo looper for the Dirtywave M8 Model:01
//
// engine.cpp runs the master clock, varispeed playback, filters, mixing,
// input monitoring and MIDI clock; storage.cpp streams the tracks from SD and
// records new ones. app.* holds the shared state (board, tempo, selection),
// ui*.* draws the screen and input.* handles the keys; scenes.* keeps the
// scene list; this file sets things up and runs the main loop. Recordings are
// WAV samples in /looper on the SD card and survive power-off, along with
// each track's settings, the scenes and the tempo. More in docs/.
//
// Behaviour:
//   * the input (LINE IN) is always heard
//   * recording is free length in whole bars (up to 32): stopped within a beat
//     after a bar line it ends there, otherwise at the end of that bar; it
//     starts on the bar line just passed (pressed within ~250 ms of it) or
//     the next one
//   * changing the tempo repitches the loops (30-240 BPM, whatever they were
//     recorded at)
//   * each track loops a range of its recording: any start beat, 1 beat up to
//     the whole loop, possibly across the loop's end
//   * scenes: snapshots of all tracks and the tempo, switched without gaps
//   * MIDI clock + start/stop go out on serial MIDI port 2
//
// Keys:
//   UP/DOWN          select a row: tracks 8 (top) to 1 (just above the status row),
//                      then the status row; it starts on track 1
//   LEFT/RIGHT       select a cell: waveform, filter, volume (on the status row: scene, tempo)
//   OPT twice        reset the current cell (waveform: whole loop; filter: open;
//                      volume: unity, balance centred)
//   OPT + arrows     change the current cell:
//                      waveform: LEFT/RIGHT move the loop range's start (a beat;
//                                  clips over 4 bars half a bar, over 16 a bar);
//                                  twice: by its own length, stopping at the clip edge
//                                UP/DOWN lengthen/shorten it (1/4, 1/2, 3/4, 1, 1 1/2,
//                                  2, 3, 4, 5 ... bars); twice: powers of two (3 x 2^n
//                                  doubles/halves)
//                      filter: LEFT/RIGHT move the band, UP/DOWN widen/narrow it
//                      volume: LEFT/RIGHT balance, UP/DOWN volume
//                      tempo: LEFT/RIGHT -/+ 0.1 BPM, UP/DOWN +/- 1 BPM
//   EDIT             empty track: record / stop recording; full track: mute/unmute
//   SHIFT+LEFT/RIGHT the track's sample: previous / next on the card (empty track:
//                      newest / oldest); keeps volume, filter, mute; whole-loop range
//   SHIFT+EDIT       cut the track's clip to the clipboard; on an empty track, paste it
//   SHIFT twice+EDIT (second tap held) empty track: resample (record the output, from the
//                      next bar line); then it moves to a new scene after this one (on the
//                      same track). With a clip: overdub (not there yet)
//   SHIFT+UP/DOWN    newer / older scene (0*: empty; after the last, a copy of it);
//                    on the status row's scene cell B+UP/DOWN does the same
//                    (while playing: on the next bar line after SHIFT is let go; SCN
//                    flashes until then. To and from N+1*, the jumps to the ends, and
//                    cutting, pasting and duplicating scenes: at once, on the next beat)
//   SHIFT+UP twice   (second tap held a moment) straight to the copy after the last
//                    (N+1*); SHIFT+DOWN: to the empty scene (0*)
//   OPT, SHIFT, EDIT paste the scene clipboard after the current scene (OPT pressed first)
//   SHIFT, OPT, EDIT cut the scene (to the scene clipboard), go to the one before (SHIFT first)
//   OPT+EDIT         open / close the menu
//   EDIT or OPT (menu, project slot)  on release: load its project (empty slot: a new one), back to the looper
//   SHIFT+EDIT or OPT (menu, slot)    cut its project / paste one into an empty slot (stopped only)
//   EDIT or OPT (menu list)           export to ALS / USB data mode (nothing yet)
//   SHIFT+OPT twice  insert a copy of the current scene after it
//   PLAY             start from 1.1.1 / stop
//   SHIFT+PLAY       start from 1.1.1, also while running (also SHIFT+EDIT on the status row)
//   EDIT (scene cell) open the menu (as OPT+EDIT)
//   EDIT (tempo cell) tap tempo: the tempo follows the taps; the 5th, 9th, 13th...
//                      tap starts from 1.1.1 (running: snaps to the nearest bar start)
//   power (hold)     switch off
//
// Build: Teensyduino, board "Teensy 4.0", USB type "Serial", CPU speed 600 MHz.
// Libraries: ILI9341_t3n (KurtE), Audio, SD (bundled with Teensyduino).

#include <Audio.h>
#include <SD.h>
#include "app.h"
#include "ui.h"
#include "input.h"

AudioInputI2S        i2sIn;
LooperEngine         engine;
AudioOutputI2S       i2sOut;
AudioConnection      c1(i2sIn, 0, engine, 0), c2(i2sIn, 1, engine, 1);
AudioConnection      c3(engine, 0, i2sOut, 0), c4(engine, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

// ---- Freeze detector (debugging): loop() and the audio interrupt both count
// their progress; a low-priority timer reports on Serial if either stalls for
// 1.5 s, naming the step loop() was in. It can only print while interrupts
// still run, so silence after a freeze means interrupts were off or it crashed
// (then CrashReport prints at the next boot).
static volatile uint32_t loopCount = 0;
static IntervalTimer freezeTimer;

static void freezeCheck() {
  static uint32_t lastLoop = 0, lastNow = 0, loopStill = 0, audioStill = 0;
  uint32_t lc = loopCount, now = transport.now;
  if (lc == lastLoop) {
    if (++loopStill == 15) Serial.printf("FREEZE: loop() stuck for 1.5 s in: %s\n", (const char *)debugStage);
  } else {
    if (loopStill >= 15) Serial.printf("loop() resumed after %lu ms\n", loopStill * 100);
    loopStill = 0;
    lastLoop = lc;
  }
  if (now == lastNow) {
    if (++audioStill == 15) Serial.printf("FREEZE: audio interrupt stopped (loop in: %s)\n", (const char *)debugStage);
  } else {
    if (audioStill >= 15) Serial.printf("audio resumed after %lu ms\n", audioStill * 100);
    audioStill = 0;
    lastNow = now;
  }
}

// ---- Power
static void handlePowerButton() {
  if (!board.powerButtonPressed()) return;
  uint32_t start = millis();
  while (board.powerButtonHeld()) {
    if (millis() - start >= 1000) {
      codec.volume(0.0f);
      flushHeaders();
      saveSessionNow();
      flushScenes();
      board.powerOff();
    }
  }
  board.powerButtonPressed();
}

static void fatal(const char *msg) {
  auto &t = board.tft();
  t.setTextColor(TEXT_ERROR, BG);
  t.setTextSize(1);
  t.setCursor(4, splashBottom() + 16);           // under the logo
  t.print(msg);
  for (;;) handlePowerButton();
}

void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  AudioMemory(12);
  board.begin();
  board.powerButtonPressed();                        // the press that switched us on
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(5);
  codec.volume(0.6f);

  board.tft().setTextWrap(false);
  showSplash();
  auto progress = [](const char *msg) {          // under the logo
    auto &t = board.tft();
    t.setTextColor(TEXT_PROGRESS, BG);
    t.setTextSize(1);
    t.setCursor(4, splashBottom() + 16);
    t.printf("%-50s", msg);
    Serial.println(msg);
  };
  if (!storageBegin(progress))
    fatal("SD card missing or full: the looper needs ~100 MB free in /looper.");
  progress("");
  beginSession();                                  // the tempo and the selection
  transport.spb = spbForBpm(bpm);
  scenesBegin([] { return bpm; }, setTempo, sessionProject());
  takeProjectSelection(false);
  if (MIDI_OUT_ENABLED) midiBegin(board.midiOutBegin());
  freezeTimer.priority(255);
  freezeTimer.begin(freezeCheck, 100000);
  hideSplash();
  restoreMenu(sessionMenu());                      // its selection (open at power off: open again)
}

void loop() {
  loopCount++;
  debugStage = "power button";
  handlePowerButton();
  debugStage = "storage";
  serviceStorage();

  // Speaker off while headphones are plugged in.
  bool phones;
  if (board.headphonesChanged(phones)) {
    board.setSpeaker(!phones);
    Serial.printf("headphones %s: speaker %s\n", phones ? "in" : "out", phones ? "off" : "on");
  }

  debugStage = "keys (I2C)";
  pollKeys();
  saveSessionWhenIdle();
  serviceScenes();

  // Report dropouts with what the storage code was doing (debugging).
  static uint32_t lastDropCheck = 0, lastDrops[NUM_TRACKS] = {};
  if (millis() - lastDropCheck >= 250) {
    lastDropCheck = millis();
    for (int t = 0; t < NUM_TRACKS; t++) {
      uint32_t d = tracks[t].underruns;
      if (d != lastDrops[t]) {
        Serial.printf("dropout: track %d +%lu samples (loop in: %s, recording: %d)\n", t + 1,
                      d - lastDrops[t], (const char *)debugStage, (int)recording.mode);
        lastDrops[t] = d;
      }
    }
  }

  followRecording();

  debugStage = "screen";
  uiRefresh();
}
