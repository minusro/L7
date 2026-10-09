// preview.cpp — renders the looper's screen on a PC, for working on the UI
// without flashing the M8. Sets the tracks up in a few states, draws every
// row with the real UI code (into the stand-in display, stubs/M8Board.h) and
// writes each screen as a PNG (2x, via Python's PIL).
//
//   ./build.sh && ./ui_preview [out-dir]
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include "app.h"
#include "ui.h"
#include "audio_follow.h"
#include "input.h"

// What the Teensy-library stand-ins expect from their host (as in looper_sim).
uint32_t simMicros = 0;
void (*simDelayHook)(uint32_t us) = nullptr;
std::string simRoot = "/tmp/m8_ui_preview_sd";
SDClass SD;
SerialStub Serial;
int simFailIo = 0, simReinits = 0, simFsChanges = 0, simFileCreates = 0, simSampleTruncates = 0;
void (*simSdLatency)(size_t, bool) = nullptr;
void (*IntervalTimer::fn)() = nullptr;
HardwareSerial midi;

bool optBeforeShift() { return true; }             // (input.cpp isn't built here)
bool arrowsMoved() { return true; }
static uint8_t simSwitchKey = 0;                   // (an arrow held that switches screens: the peeps)
bool screenSwitchPending() { return simSwitchKey != 0; }
bool tempoNudging() { return false; }
uint32_t lastTapMs() { return 0; }
uint8_t mainKeys() { return board.keys(); }
uint32_t menuCardPressMs() { return 0; }
bool menuJustCut() { return false; }
uint8_t arrivalEdgeKey() { return 0; }
float menuCutProgress(int &slot) { slot = -1; return 0; }
uint8_t arrowModifier() { uint8_t k = board.keys(); return k & m8::BTN_OPT ? m8::BTN_OPT : k & m8::BTN_SHIFT ? m8::BTN_SHIFT : 0; }
uint8_t screenSwitchKey() { return simSwitchKey; }

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
  if (menuOpen()) drawMenu();                      // (in the track rows' place)
  else updatePlayheads();
  drawEdgeLine();                                  // (an arrow held off an edge: its line)
  writePng(path);
}

// ---- Showcase (./ui_preview <out-dir> showcase): eight screens as varied
// and plausible as can be, each its own project (and colour), tempo, scenes,
// clock and tracks.
namespace showcase {
struct Clip { int t; TrackState state; uint32_t bars, rangeStart, rangeBeats; float density; int lo = 0, hi = FILTER_MAX, vol = VOLUME_UNITY, bal = 0; };

static void clearAll() {
  recording.mode = Recording::Idle;
  recording.track = 0;
  recording.resample = false;
  board.heldKeys = 0;
  for (int t = 0; t < NUM_TRACKS; t++) {
    tracks[t].state = TrackState::Empty;
    tracks[t].live = false;
    tracks[t].sample = 0;
    setTrackFilter(t, 0, FILTER_MAX);
    setTrackMix(t, VOLUME_UNITY, 0);
  }
}

// Project p loaded at tempo t, with these clips, `scenes` scenes, on scene `at`.
static void setUp(int p, float tempo, std::initializer_list<Clip> clips, int scenes, int at, uint32_t seed) {
  transport.running = false;
  clearAll();
  projectLoad(p);
  ACCENT = PROJECT_COLORS[p];
  ACCENT_DARK = blend565(ACCENT, BG, 128);
  bpm = tempo;
  transport.spb = spbForBpm(bpm);
  for (const Clip &c : clips) {
    setTrack(c.t, c.state, c.bars, c.rangeStart, c.rangeBeats, seed + c.t * 7, c.density);
    tracks[c.t].sample = 1000 + p * 16 + c.t;      // (a clip: the project has clips)
    setTrackFilter(c.t, c.lo, c.hi);
    setTrackMix(c.t, c.vol, c.bal);
  }
  if (scenes > 0) {
    sceneStateChanged();
    for (int i = 0; i < std::max(1, scenes - 1); i++) sceneDuplicate();   // (the first: 0* becomes scene 1, and a copy after it)
    while (sceneCurrent() > at) sceneStep(-1);
  }
}

// The screen, its meters at these levels (left, right per track).
static void shot(const std::string &path, int32_t beat, float phase, std::initializer_list<float> peaks) {
  transport.running = true;
  transport.beat = beat;
  transport.sampleInBeat = (uint32_t)(transport.spb * phase);
  int t = 0;
  for (float pk : peaks) {
    if (t >= NUM_TRACKS) break;
    tracks[t].peakL = pk;
    tracks[t].peakR = pk * 0.85f;
    t++;
  }
  simMicros += 5000000;
  board.tft().fillScreen(BG);
  uiGeneration++;
  for (int i = 0; i < ROWS; i++) drawTrack(i);
  if (menuOpen()) drawMenu();
  else updatePlayheads();
  writePng(path);
}

static void run(const std::string &out) {
  std::system(("rm -rf " + simRoot + " && mkdir -p " + simRoot + "/L7/PROJECTS " + simRoot + "/L7/DATA " + simRoot + "/L7/SAMPLES").c_str());
  scenesBegin([] { return bpm; }, loadTempo, 0, newProjectTempo);
  using TS = TrackState;
  // 1. A groove in full swing: five clips of different lengths, one muted, a filtered pad.
  setUp(0, 124.0f, {{6, TS::Playing, 1, 0, 4, 1.4f}, {5, TS::Playing, 2, 0, 8, 0.9f}, {4, TS::Playing, 4, 0, 16, 0.5f, 4, 20},
                    {3, TS::Muted, 2, 4, 4, 1.8f}, {1, TS::Playing, 8, 8, 16, 0.35f, 0, 18, 3, -1}}, 5, 3, 11);
  selected = 5; selectedCell = WAVE_CELL;
  shot(out + "/show_1_groove.png", 4 * 16 + 2, 0.4f, {0, 0.18f, 0, 0.25f, 0.4f, 0.55f, 0.7f});
  // 2. Shortening a loop's range: B held on a waveform (the range in the accent colour).
  setUp(2, 98.0f, {{6, TS::Playing, 2, 0, 8, 1.1f}, {5, TS::Playing, 4, 6, 6, 0.7f}, {4, TS::Playing, 1, 0, 4, 2.2f, 0, 26},
                   {2, TS::Playing, 8, 20, 12, 0.4f, 6, 30, 4, 2}}, 4, 2, 23);
  selected = 5; selectedCell = WAVE_CELL;
  board.heldKeys = m8::BTN_OPT;
  shot(out + "/show_2_range.png", 4 * 9 + 1, 0.7f, {0, 0, 0.3f, 0, 0.35f, 0.5f, 0.6f});
  // 3. Recording a third loop over two others: the live view in the accent colour.
  setUp(4, 86.5f, {{6, TS::Playing, 4, 0, 16, 0.6f}, {5, TS::Playing, 2, 0, 8, 1.3f, 0, 22}}, 2, 2, 37);
  recording.track = 4;
  recording.mode = Recording::Running;
  transport.now = 30 * transport.spb;
  recording.startSample = transport.now - 11 * transport.spb;
  transport.beat = 4 * 6 + 3;
  recording.startBeat = transport.beat - 11;
  selected = 4; selectedCell = WAVE_CELL;
  shot(out + "/show_3_recording.png", 4 * 6 + 3, 0.2f, {0, 0, 0, 0, 0, 0.45f, 0.6f});
  // 4. Sweeping a filter: B held on track 3's filter, a narrow band.
  setUp(6, 140.0f, {{6, TS::Playing, 1, 0, 4, 2.0f, 0, 30}, {5, TS::Playing, 2, 0, 8, 1.0f}, {4, TS::Playing, 2, 0, 8, 0.8f, 9, 17},
                    {3, TS::Playing, 4, 0, 16, 0.45f, 0, 12}, {2, TS::Muted, 1, 0, 4, 1.6f}, {0, TS::Playing, 8, 0, 32, 0.3f, 14, 36, 2}}, 9, 6, 51);
  selected = 4; selectedCell = FILTER_CELL;
  board.heldKeys = m8::BTN_OPT;
  shot(out + "/show_4_filter.png", 4 * 41 + 2, 0.55f, {0.2f, 0, 0, 0.3f, 0.22f, 0.5f, 0.65f});
  // 5. Mixing: B held on a volume cell, the meters running, one track panned.
  setUp(8, 110.5f, {{6, TS::Playing, 2, 0, 8, 1.2f}, {5, TS::Playing, 2, 0, 8, 0.7f, 0, 24, 3, 2}, {4, TS::Playing, 4, 0, 16, 0.5f, 0, 36, 4, -3},
                    {3, TS::Playing, 1, 0, 4, 2.4f, 5, 36, 2}, {1, TS::Muted, 4, 0, 16, 0.4f}}, 4, 1, 67);
  selected = 4; selectedCell = MIX_CELL;
  board.heldKeys = m8::BTN_OPT;
  shot(out + "/show_5_mix.png", 4 * 3 + 1, 0.15f, {0, 0, 0, 0.6f, 0.5f, 0.32f, 0.75f});
  // 6. Tapping the tempo: A held on the tempo cell (TAP pressed), a fast set.
  setUp(10, 172.0f, {{6, TS::Playing, 2, 0, 8, 1.5f}, {5, TS::Playing, 4, 0, 16, 0.9f, 0, 20}, {4, TS::Playing, 2, 0, 8, 1.1f},
                     {3, TS::Playing, 1, 0, 4, 2.5f, 12, 36}, {2, TS::Playing, 8, 0, 32, 0.3f}, {1, TS::Muted, 2, 0, 8, 1.2f},
                     {0, TS::Playing, 4, 8, 8, 0.6f, 0, 28}}, 4, 4, 83);
  selected = STATUS_ROW; selectedCell = TEMPO_CELL;
  board.heldKeys = m8::BTN_EDIT;
  noteActionPress();
  shot(out + "/show_6_tap.png", 4 * 32, 0.05f, {0.4f, 0, 0.55f, 0.35f, 0.5f, 0.45f, 0.7f});
  board.heldKeys = 0;
  // 7. The menu: five projects on the card, G loaded and playing, C selected.
  for (int p : {0, 2, 4, 10}) {
    setUp(p, 120.0f, {{6, TS::Playing, 2, 0, 8, 1.0f}}, 1, 1, 90 + p);
  }
  setUp(6, 128.0f, {{6, TS::Playing, 2, 0, 8, 1.0f}, {5, TS::Playing, 4, 0, 16, 0.6f}}, 3, 2, 101);
  openMenu();
  menuSelectSlot(2);
  shot(out + "/show_7_menu.png", 4 * 12 + 1, 0.2f, {0, 0, 0, 0, 0, 0.4f, 0.6f});
  closeMenu();
  // 8. A new project: waiting for the bar line to record its first loop, the metronome clicking.
  setUp(3, 120.0f, {}, 0, 0, 0);
  recording.track = 6;
  recording.mode = Recording::WaitBar;
  metronomeOn = true;
  selected = 6; selectedCell = WAVE_CELL;
  shot(out + "/show_8_new.png", 3, 0.1f, {0, 0, 0, 0, 0, 0, 0});
  metronomeOn = false;
  clearAll();
}
}  // namespace showcase

int main(int argc, char **argv) {
  std::string out = argc > 1 ? argv[1] : ".";
  if (argc > 2 && std::string(argv[2]) == "showcase") {
    showcase::run(out);
    return 0;
  }
  showSplash();                                    // the splash: the L and the 7 sliding in
  simMicros += 400000;
  splashTick();
  writePng(out + "/ui_splash_slide.png");
  simMicros += 2200000;                            // its hue cycle done and the line faded in
  splashTick();
  writePng(out + "/ui_splash.png");
  simMicros += 850000;                             // 3.45 s: everything in place, just before it fades out
  splashTick();
  writePng(out + "/ui_splash_hold.png");
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
  setTrackMix(4, 8, -6);
  setTrack(6, TrackState::Playing, 8, 28, 8, 6, 0.5f);   // a range across the loop's end
  setTrackFilter(6, 0, 14);

  selected = 1;
  selectedCell = WAVE_CELL;
  render(out + "/ui_wave.png");
  selected = 3;
  selectedCell = FILTER_CELL;
  render(out + "/ui_filter.png");
  for (auto [k, name] : {std::pair<uint8_t, const char *>{m8::BTN_LEFT, "left"}, {m8::BTN_RIGHT, "right"}, {m8::BTN_UP, "up"}, {m8::BTN_DOWN, "down"}}) {
    simSwitchKey = k;                              // an arrow held off each edge: its line (towards the menu)
    render(out + "/ui_navi_" + name + ".png");
  }
  simSwitchKey = 0;
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
  selected = LAST_TRACK_ROW;                       // track 7: its ground stops at its button's bottom (none above the action button)
  selectedCell = MIX_CELL;
  render(out + "/ui_track7.png");
  for (int t = 0; t < NUM_TRACKS; t++)             // B held on an empty track: up and down arrows for its number (B + UP/DOWN moves it)
    if (tracks[t].state == TrackState::Empty) {
      selected = t;
      board.heldKeys = m8::BTN_OPT;
      render(out + "/ui_move.png");
      board.heldKeys = 0;
      break;
    }
  selected = STATUS_ROW;
  selectedCell = TEMPO_CELL;
  render(out + "/ui_status.png");
  notesScreen = notesLayout = true;                // the notes screen: the status row on top
  uiGeneration++;
  render(out + "/ui_notes_status.png");
  selected = 3;
  render(out + "/ui_notes.png");
  selected = STATUS_ROW;
  notesScreen = notesLayout = false;
  uiGeneration++;
  board.heldKeys = m8::BTN_EDIT;                   // A held on the tempo: TAP pressed, a block lower
  noteActionPress();
  render(out + "/ui_status_tap_held.png");
  board.heldKeys = 0;
  render(out + "/ui_status.png");
  board.heldKeys = m8::BTN_OPT;                    // B held on the tempo: B + arrows change it; B+A, the metronome
  render(out + "/ui_status_tempo_edit.png");
  board.heldKeys = m8::BTN_SHIFT;                  // SHIFT held on the tempo: audio follow (an ear)
  render(out + "/ui_status_follow.png");
  audioFollowToggle();                             // following: the ear crossed out (SHIFT+A: off)
  render(out + "/ui_status_follow_on.png");
  audioFollowToggle();
  board.heldKeys = m8::BTN_OPT;                    // B held on the tempo: the metronome
  render(out + "/ui_status_metro.png");
  {
    bool wasRunning = transport.running;           // clicking: crossed out
    transport.running = true;
    metronomeOn = true;
    render(out + "/ui_status_metro_on.png");
    metronomeOn = false;
    transport.running = wasRunning;
  }
  selectedCell = SCENE_CELL;                       // and on the scene: duplicate it
  render(out + "/ui_status_dup.png");
  selectedCell = TEMPO_CELL;
  selectedCell = SCENE_CELL;                       // B held on the scene: B + UP/DOWN step through them
  render(out + "/ui_status_scene_edit.png");
  board.heldKeys = 0;
  render(out + "/ui_status_scene.png");
  selectedCell = MENU_CELL;                        // LEFT to the menu cell: not selectable (stays on the scene cell)
  render(out + "/ui_status_menu.png");
  {                                                // SHIFT held on a track's filter cell: its waveform cell framed (accent), the filter's not
    int wasSel = selected, wasCell = selectedCell;
    selected = 2; selectedCell = FILTER_CELL;
    board.heldKeys = m8::BTN_SHIFT;
    render(out + "/ui_shift_track.png");
    board.heldKeys = 0;
    selected = wasSel; selectedCell = wasCell;
  }
  selectedCell = CLOCK_CELL;                       // the clock: A starts / stops (as START)
  render(out + "/ui_status_clock.png");
  setMasterVolume(MASTER_VOLUME_UNITY - 4);          // B held on it: the master volume and balance, and its meter
  setMasterBalance(6);
  board.heldKeys = m8::BTN_OPT;
  render(out + "/ui_status_master.png");
  board.heldKeys = 0;
  setMasterVolume(MASTER_VOLUME_UNITY);
  setMasterBalance(0);
  selectedCell = TEMPO_CELL;
  bpm = 85.5f;                                     // a tempo below 100
  render(out + "/ui_status_slow.png");
  bpm = 120.0f;
  selected = 5;                                    // an empty track: A records
  selectedCell = WAVE_CELL;
  render(out + "/ui_empty.png");
  selected = 4;                                    // track 3 (under the empty track 2), its filter: the shared border by place
  selectedCell = FILTER_CELL;
  render(out + "/ui_under_empty.png");
  selected = 5;
  selectedCell = WAVE_CELL;
  selected = 2;                                    // a muted track
  render(out + "/ui_muted.png");
  board.heldKeys = m8::BTN_SHIFT;                  // SHIFT + A: cut a clip, paste onto an empty track
  selected = 1;
  render(out + "/ui_cut.png");
  selected = 5;
  render(out + "/ui_paste.png");
  board.heldKeys = m8::BTN_SHIFT | m8::BTN_OPT;    // with B too: the scene versions (paste: B first)
  render(out + "/ui_scene_paste.png");
  board.heldKeys = 0;
  selected = 2;
  board.heldKeys = m8::BTN_OPT;                    // B held on a clip's cell: adjusting (four arrows)
  render(out + "/ui_adjust.png");
  board.heldKeys = 0;
  // Track 6 recording, 2 bars and a bit in (its live view: no audio saved yet).
  recording.track = 5;
  recording.mode = Recording::Running;
  transport.now = 10 * transport.spb;
  recording.startSample = transport.now - 9 * transport.spb;
  recording.startBeat = transport.beat - 9;
  selected = 5;
  render(out + "/ui_recording.png");
  recording.resample = true;                       // resampling: the record dot with a plus on its button
  render(out + "/ui_resampling.png");
  recording.resample = false;
  selected = 1;                                    // A held on track 7: its panel and the action button highlighted
  selectedCell = WAVE_CELL;
  board.heldKeys = m8::BTN_EDIT;
  noteActionPress();
  render(out + "/ui_action_held.png");
  board.heldKeys = 0;
  // Projects: 1 (loaded), 4, 6 and 11 hold one; the rest empty.
  transport.running = false;
  recording.mode = Recording::Idle;
  std::system(("rm -rf " + simRoot + " && mkdir -p " + simRoot + "/L7").c_str());
  scenesBegin([] { return bpm; }, loadTempo, 0, newProjectTempo);
  projectLoad(3);
  projectLoad(5);
  projectLoad(10);
  projectLoad(0);
  selected = 1;                                    // the menu (B+A), its first box selected
  openMenu();
  render(out + "/ui_menu.png");
  menuKeys(m8::BTN_RIGHT);                         // an empty slot: new
  render(out + "/ui_menu_new.png");
  board.heldKeys = m8::BTN_SHIFT;                  // SHIFT: paste (the clipboard empty: dark)
  render(out + "/ui_menu_paste.png");
  menuKeys(m8::BTN_RIGHT);
  menuKeys(m8::BTN_RIGHT);                         // a project, SHIFT: cut
  render(out + "/ui_menu_cut.png");
  board.heldKeys = 0;
  render(out + "/ui_menu_load.png");
  menuKeys(m8::BTN_LEFT);
  menuKeys(m8::BTN_LEFT);
  menuKeys(m8::BTN_LEFT);
  for (int i = 0; i < 4; i++) menuKeys(m8::BTN_DOWN);   // down to the list's second item
  render(out + "/ui_menu_button.png");
  menuKeys(m8::BTN_UP);                            // the list's first item: export
  render(out + "/ui_menu_export.png");
  menuExportStatus("SAVED AS L7 A005", 9);          // the label after an export: the name in the project's colour
  render(out + "/ui_menu_exported.png");
  menuExportStatus(nullptr);
  menuKeys(m8::BTN_LEFT);                          // a card with A held: its frame in the project's colour
  board.heldKeys = m8::BTN_EDIT;
  render(out + "/ui_menu_held.png");
  board.heldKeys = 0;
  menuKeys(m8::BTN_RIGHT);                         // on the right column, RIGHT held: the way out lit
  menuKeys(m8::BTN_RIGHT);
  menuKeys(m8::BTN_RIGHT);
  simSwitchKey = m8::BTN_RIGHT;
  render(out + "/ui_menu_leaving.png");
  for (auto [k, name] : {std::pair<uint8_t, const char *>{m8::BTN_LEFT, "left"}, {m8::BTN_UP, "up"}, {m8::BTN_DOWN, "down"}}) {
    simSwitchKey = k;                              // (the other edges' lines, out of the menu)
    render(out + "/ui_menu_leaving_" + name + ".png");
  }
  simSwitchKey = 0;
  for (int i = 0; i < 6; i++) menuKeys(m8::BTN_DOWN);   // the settings: open, the second item selected, one off
  menuSettings(true);
  menuSettingsKeys(m8::BTN_UP);
  setSetting(SET_NEXT_FREE, false);
  board.charging = true;                           // (the battery charging, full)
  board.battery = 96;
  render(out + "/ui_menu_settings.png");
  setSetting(SET_NEXT_FREE, true);
  board.charging = false;
  menuSettings(false);
  writePng(out + "/ui_menu_settings_closed.png");  // (as drawn, not redrawn: closed, what was under it back)
  menuSettings(true);
  writePng(out + "/ui_menu_settings_opened.png");  // (as drawn: opened over the menu as it was)
  menuSettings(false);
  closeMenu();
  return 0;
}
