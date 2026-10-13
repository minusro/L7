// n9_mi.cpp — see n9_mi.h.
#include "n9_mi.h"

#include <Arduino.h>

#include "mi.h"
#include "n9_state.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

// Registration order in plaits/dsp/voice.cpp.
const char *const MI_MODEL_NAMES[MI_MODELS] = {
    "VA-VCF", "PHASED", "6-OP A", "6-OP B", "6-OP C", "TERRAIN", "STRMACH", "CHIP",
    "ANALOG", "SHAPER", "FM", "GRAIN", "HARMON", "WTABLE", "CHORD", "SPEECH",
    "SWARM", "NOISE", "PARTICLE", "STRING", "MODAL", "KICK", "SNARE", "HIHAT"};
static constexpr uint8_t SPEECH_MODEL = 15;

static constexpr uint8_t NV = 4;
DMAMEM static char bufs[NV][16384];
static plaits::Voice voices[NV];
static plaits::Patch patches[NV];
static plaits::Modulations mods[NV];
static uint8_t vNote[NV], vVel[NV], vTrack[NV];
static bool vGate[NV];
static float vLevel[NV], vTarget[NV];
static uint8_t trigChunks[NV];
static uint32_t vAge[NV];
static uint32_t ageCtr = 0;
static uint8_t model = 8;                         // ANALOG to start
static bool inited = false;

// Rendered-ahead FIFO per voice (plaits renders 12-sample blocks).
static float fifo[NV][2 * plaits::kBlockSize];
static int fifoN[NV];

static void init() {
  for (int v = 0; v < NV; v++) {
    stmlib::BufferAllocator allocator(bufs[v], sizeof bufs[v]);
    voices[v].Init(&allocator);
    patches[v].engine = model;
    patches[v].note = 60;
    patches[v].harmonics = 0.5f; patches[v].timbre = 0.5f; patches[v].morph = 0.5f;
    patches[v].frequency_modulation_amount = 0;
    patches[v].timbre_modulation_amount = 0;
    patches[v].morph_modulation_amount = 0;
    patches[v].decay = 0.55f;
    patches[v].lpg_colour = 0.3f;
    mods[v] = plaits::Modulations();
    mods[v].trigger_patched = true;
    mods[v].level_patched = true;
    vGate[v] = false; vLevel[v] = vTarget[v] = 0; fifoN[v] = 0;
  }
  inited = true;
}

uint8_t miModel() { return model; }

void miSetModel(uint8_t m) {
  model = m % MI_MODELS;
  Serial.printf("mi: model %02u %s\n", model, MI_MODEL_NAMES[model]);
}

void miNoteOn(uint8_t track, uint8_t note, uint8_t vel, uint8_t m) {
  if (!inited) init();
  if (m >= MI_MODELS) m = 0;
  int pick = -1;
  for (int v = 0; v < NV; v++) if (!vGate[v] && vLevel[v] < 0.01f) { pick = v; break; }
  if (pick < 0) { uint32_t old = UINT32_MAX; for (int v = 0; v < NV; v++) if (vAge[v] < old) { old = vAge[v]; pick = v; } }
  vNote[pick] = note;
  vVel[pick] = vel;
  vTrack[pick] = track;
  const uint8_t *p = engParams[track][E_MI_FIRST + m];   // the track's INST cells
  patches[pick].engine = m;
  patches[pick].harmonics = p[0] / 99.0f;
  patches[pick].timbre = p[1] / 99.0f;
  patches[pick].morph = p[2] / 99.0f;
  patches[pick].decay = p[3] / 99.0f;
  patches[pick].lpg_colour = p[4] / 99.0f;
  // 44.1 kHz playback of a 48 kHz model: +1.47 semitones keeps the pitch true.
  patches[pick].note = note + 1.47f;
  vGate[pick] = true;
  vTarget[pick] = (0.1f + 0.9f * (vel / 127.0f)) * (0.25f + 0.75f * p[5] / 99.0f);
  trigChunks[pick] = 2;
  vAge[pick] = ++ageCtr;
}

void miNoteOff(uint8_t track, uint8_t note) {
  for (int v = 0; v < NV; v++)
    if (vGate[v] && vTrack[v] == track && vNote[v] == note) { vGate[v] = false; vTarget[v] = 0; }
}

uint8_t miActive() {
  if (!inited) return 0;
  uint8_t n = 0;
  for (int v = 0; v < NV; v++) if (vGate[v] || vLevel[v] > 0.003f) n++;
  return n;
}

void miRender(float (*bus)[128], int n) {
  if (!inited) return;
  for (int v = 0; v < NV; v++) {
    if (!vGate[v] && vLevel[v] <= 0.003f && fifoN[v] == 0) continue;
    { // the INST cells land on sounding voices too, eased against zipper
      const uint8_t *p = engParams[vTrack[v]][E_MI_FIRST + patches[v].engine];
      auto ease = [](float &f, float tgt) { f += 0.12f * (tgt - f); };
      ease(patches[v].harmonics, p[0] / 99.0f);
      ease(patches[v].timbre, p[1] / 99.0f);
      ease(patches[v].morph, p[2] / 99.0f);
      ease(patches[v].decay, p[3] / 99.0f);
      ease(patches[v].lpg_colour, p[4] / 99.0f);
      if (vGate[v]) ease(vTarget[v], (0.1f + 0.9f * (vVel[v] / 127.0f)) * (0.25f + 0.75f * p[5] / 99.0f));
    }
    int out = 0;
    while (out < n) {
      if (fifoN[v] == 0) {                        // render another plaits block
        plaits::Voice::Frame frames[plaits::kBlockSize];
        // the level input is our envelope: fast toward the target
        vLevel[v] += 0.08f * (vTarget[v] - vLevel[v]);
        mods[v].level = vLevel[v];
        mods[v].trigger = trigChunks[v] ? 1.0f : 0.0f;
        if (trigChunks[v]) trigChunks[v]--;
        voices[v].Render(patches[v], mods[v], frames, plaits::kBlockSize);
        for (int i = 0; i < (int)plaits::kBlockSize; i++) fifo[v][i] = frames[i].out * (1.0f / 32768.0f);
        fifoN[v] = plaits::kBlockSize;
      }
      int take = fifoN[v] < n - out ? fifoN[v] : n - out;
      int base = plaits::kBlockSize - fifoN[v];
      for (int i = 0; i < take; i++) bus[vTrack[v]][out + i] += fifo[v][base + i] * 0.35f;
      fifoN[v] -= take;
      out += take;
    }
  }
}
