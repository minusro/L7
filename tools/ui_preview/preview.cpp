// preview.cpp — renders the looper's screen on a PC, for working on the UI
// without flashing the M8. Sets the tracks up in a few states, draws every
// row with the real UI code (into the stand-in display, stubs/M8Board.h) and
// writes each screen as a PNG (2x, via Python's PIL).
//
//   ./build.sh && ./ui_preview [out-dir]
#include <cmath>
#include <cstdlib>
#include <string>
#include "app.h"
#include "ui.h"
#include "input.h"

// What the Teensy-library stand-ins expect from their host (as in looper_sim).
uint32_t simMicros = 0;
void (*simDelayHook)(uint32_t us) = nullptr;
std::string simRoot = "/tmp/m8_ui_preview_sd";
SDClass SD;
SerialStub Serial;
int simFailIo = 0, simReinits = 0, simFsChanges = 0, simFileCreates = 0;
void (*simSdLatency)(size_t, bool) = nullptr;
void (*IntervalTimer::fn)() = nullptr;
HardwareSerial midi;

bool optBeforeShift() { return true; }             // (input.cpp isn't built here)

// A made-up waveform: a hit on every beat (louder on bar starts), decaying,
// over some noise.
static void fakeOverview(Track &tr, uint32_t seed, float density) {
  uint32_t beats = tr.loopBars * BEATS_PER_BAR;
  for (int c = 0; c < OVERVIEW_COLS; c++) {
    float pos = (float)c * beats / OVERVIEW_COLS, inBeat = pos - floorf(pos);
    bool bar = (int)pos % BEATS_PER_BAR == 0;
    seed = seed * 1103515245 + 12345;
    float noise = ((seed >> 16) & 255) / 255.0f;
    float level = (bar ? 1.0f : 0.7f) * expf(-inBeat * 5 / density) + 0.15f * noise;
    tr.overview[c] = (uint8_t)fminf(255, level * 230);
  }
  tr.overviewVersion++;
}

static void setTrack(int t, TrackState state, uint32_t bars, uint32_t rangeStart, uint32_t rangeBeats, uint32_t seed,
                     float density) {
  Track &tr = tracks[t];
  tr.state = state;
  tr.loopBars = bars;
  tr.spbNative = transport.spb;
  tr.playSpb = transport.spb;
  tr.recordedFrames = bars * BEATS_PER_BAR * transport.spb;
  tr.rangeStartBeat = rangeStart;
  tr.rangeBeats = rangeBeats;
  tr.rangeAnchorQ = 0;
  tr.live = true;
  fakeOverview(tr, seed, density);
}

static void writePng(const std::string &path) {
  auto &d = board.tft();
  std::string ppm = path + ".ppm";
  FILE *f = fopen(ppm.c_str(), "wb");
  fprintf(f, "P6 %d %d 255\n", d.W, d.H);
  for (int i = 0; i < d.W * d.H; i++) {
    uint16_t c = d.px[i];
    uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  std::string cmd = "python3 -c \"from PIL import Image; im = Image.open('" + ppm + "'); im.resize((im.width * 2, im.height * 2), Image.NEAREST).save('" + path + "')\" && rm '" + ppm + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not convert %s\n", ppm.c_str());
  printf("wrote %s\n", path.c_str());
}

// Draws the whole screen as the looper would after a full redraw.
static void render(const std::string &path) {
  simMicros += 5000000;                            // time passes: the meters fall to the current peaks
  for (int t = 0; t < NUM_TRACKS; t++) { tracks[t].peakL = 0.02f + 0.03f * t; tracks[t].peakR = 0.015f + 0.025f * t; }
  board.tft().fillScreen(BG);
  uiGeneration++;
  for (int i = 0; i < ROWS; i++) drawTrack(i);
  updatePlayheads();
  writePng(path);
}

int main(int argc, char **argv) {
  std::string out = argc > 1 ? argv[1] : ".";
  transport.spb = spbForBpm(bpm);
  transport.running = true;
  transport.beat = 37;
  transport.sampleInBeat = transport.spb / 3;
  setTrack(0, TrackState::Playing, 2, 0, 8, 1, 1.0f);
  setTrack(1, TrackState::Playing, 4, 4, 8, 2, 0.6f);
  setTrack(2, TrackState::Muted, 1, 0, 4, 3, 1.5f);
  setTrack(3, TrackState::Playing, 2, 2, 4, 4, 0.8f);
  setTrackFilter(3, 6, 22);
  setTrack(4, TrackState::Playing, 1, 0, 4, 5, 2.0f);
  setTrackMix(4, 24, -6);
  setTrack(6, TrackState::Playing, 8, 28, 8, 6, 0.5f);   // a range across the loop's end
  setTrackFilter(6, 0, 14);

  selected = 1;
  selectedCell = WAVE_CELL;
  render(out + "/ui_wave.png");
  selected = 3;
  selectedCell = FILTER_CELL;
  render(out + "/ui_filter.png");
  selected = 4;
  selectedCell = MIX_CELL;
  render(out + "/ui_mix.png");
  // B held on each kind of cell: what B + arrows change shows in the accent colour.
  board.heldKeys = m8::BTN_OPT;
  selected = 1;
  selectedCell = WAVE_CELL;
  render(out + "/ui_wave_edit.png");
  selected = 3;
  selectedCell = FILTER_CELL;
  render(out + "/ui_filter_edit.png");
  selected = 4;
  selectedCell = MIX_CELL;
  render(out + "/ui_mix_edit.png");
  board.heldKeys = 0;
  selected = STATUS_ROW;
  selectedCell = TEMPO_CELL;
  render(out + "/ui_status.png");
  board.heldKeys = m8::BTN_OPT;                    // B held on the tempo: B + arrows change it
  render(out + "/ui_status_tempo_edit.png");
  selectedCell = SCENE_CELL;                       // B held on the scene: B + UP/DOWN step through them
  render(out + "/ui_status_scene_edit.png");
  board.heldKeys = 0;
  render(out + "/ui_status_scene.png");
  selectedCell = TEMPO_CELL;
  bpm = 85.5f;                                     // a tempo below 100
  render(out + "/ui_status_slow.png");
  bpm = 120.0f;
  selected = 5;                                    // an empty track: A records
  selectedCell = WAVE_CELL;
  render(out + "/ui_empty.png");
  selected = 2;                                    // a muted track
  render(out + "/ui_muted.png");
  // Track 6 recording, 2 bars and a bit in (its live view: no audio saved yet).
  recording.track = 5;
  recording.mode = Recording::Running;
  transport.now = 10 * transport.spb;
  recording.startSample = transport.now - 9 * transport.spb;
  recording.startBeat = transport.beat - 9;
  selected = 5;
  render(out + "/ui_recording.png");
  selected = 1;                                    // A held on track 7: its panel and the action button highlighted
  selectedCell = WAVE_CELL;
  board.heldKeys = m8::BTN_EDIT;
  noteActionPress();
  render(out + "/ui_action_held.png");
  board.heldKeys = 0;
  selected = 1;                                    // the panel (B+A) over the screen
  render(out + "/ui_panel.png");
  board.tft().fillScreen(BG);
  uiGeneration++;
  for (int i = 0; i < ROWS; i++) drawTrack(i);
  openPanel();
  writePng(out + "/ui_panel.png");
  closePanel();
  return 0;
}
