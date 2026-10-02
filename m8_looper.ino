// m8_looper.ino — 8-track stereo looper for the Dirtywave M8 Model:01
//
// engine.cpp runs the master clock, varispeed playback, filters, mixing,
// input monitoring and MIDI clock; storage.cpp streams the tracks from SD and
// records new ones. app.* holds the shared state (board, tempo, selection),
// ui*.* draws the screen and input.* handles the keys; this file sets things
// up and runs the main loop. Tracks are WAV files in /looper on
// the SD card and survive power-off, along with the tempo.
//
// Behaviour:
//   * the input (LINE IN) is always heard
//   * recording is free length; the loop snaps to a power of two bars (or
//     shrinks to the one below if you stopped less than a beat past it), and
//     starts on the bar line you were nearest to
//   * changing the tempo repitches the loops (50%-200% of each one's tempo)
//   * each track can loop any range of whole bars of its recording
//   * MIDI clock + start/stop go out on serial MIDI port 1
//
// Keys:
//   UP/DOWN          select a row: tracks 1-8, then the status row
//   LEFT/RIGHT       select a cell: waveform, filter, volume (on the status row the first is the tempo)
//   OPT twice        reset the current cell (waveform: whole loop; filter: open;
//                      volume: unity, balance centred)
//   OPT + arrows     change the current cell:
//                      waveform: LEFT/RIGHT move the loop range by a beat,
//                                UP/DOWN double/halve it (1/4 bar to the whole loop)
//                      filter: LEFT/RIGHT move the band, UP/DOWN widen/narrow it
//                      volume: LEFT/RIGHT balance, UP/DOWN volume
//                      tempo: LEFT/RIGHT -/+ 0.1 BPM, UP/DOWN +/- 1 BPM
//   EDIT             empty track: record / stop recording; full track: mute/unmute
//   SHIFT+EDIT       clear the track
//   PLAY             start from 1.1.1 / stop (also EDIT on the status row)
//   SHIFT+PLAY       start from 1.1.1, also while running (also SHIFT+EDIT on the status row)
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
      saveTempoNow();
      board.powerOff();
    }
  }
  board.powerButtonPressed();
}

static void fatal(const char *msg) {
  auto &t = board.tft();
  t.setTextColor(TEXT_ERROR, BG);
  t.setTextSize(1);
  t.setCursor(4, 100);
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

  board.tft().fillScreen(BG);
  board.tft().setTextWrap(false);
  auto progress = [](const char *msg) {
    auto &t = board.tft();
    t.setTextColor(TEXT_PROGRESS, BG);
    t.setTextSize(1);
    t.setCursor(4, 100);
    t.printf("%-50s", msg);
    Serial.println(msg);
  };
  if (!storageBegin(progress))
    fatal("SD card missing or full: the looper needs ~180 MB in /looper.");
  progress("");
  bpm = roundf(loadTempo() * 10) / 10;
  transport.spb = spbForBpm(bpm);
  if (MIDI_OUT_ENABLED) midiBegin(board.midiOutBegin());
  freezeTimer.priority(255);
  freezeTimer.begin(freezeCheck, 100000);
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
  saveTempoWhenIdle();

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
