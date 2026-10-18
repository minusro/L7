// k6_project.cpp — see k6_project.h. One binary blob per project; reads and
// writes go through a DMAMEM staging copy so the audio interrupt never sees
// half a load: clipFreeze holds the sequencer off the clips while the live
// structures are swapped, and the FX types pass through FXT_NONE around
// their parameter copies (the activation-gate rule, k6_fx.cpp).
#include "k6_project.h"

#include <Arduino.h>

#include "k6_apps.h"
#include "k6_fx.h"
#include "k6_song.h"
#include "k6_state.h"
#include "k6_synth.h"

int8_t projCurrent = 0;
bool projExists[12];
static bool clipFileExists = false;
static int8_t pendingLoad = -1;     // a load waiting for the next beat
static uint32_t pendingBeat = 0;    // the beat it was asked on

static constexpr uint32_t MAGIC = 0x3250364B;    // "K6P2" (the SONG arranger joined)
static constexpr uint32_t MAGIC_V1 = 0x3150364B; // "K6P1" still loads (no song rows)

struct ProjectBlob {
  uint32_t magic;
  float bpm;
  uint8_t key, scale, masterVol, mfxType;
  uint8_t mfxP[8];
  uint8_t engine[9];
  uint8_t vol[9], pan[9], mute[9];
  int8_t playing[9];
  uint8_t tfxType[9][2];
  uint8_t tfxP[9][2][8];
  uint8_t sendP[3][8];
  uint8_t gmProg[9];
  uint8_t drumMach[9];
  TrackLayer layer[9];
  uint8_t metro, pad1, pad2, pad3;
  uint8_t params[9][ENGINE_COUNT][8];
};   // (K6P2: a SongBlob follows, then the clips stream in; K6P1: clips only)
struct SongBlob { uint8_t n, pad; SongRow rows[SONG_MAX]; };
static DMAMEM ProjectBlob blob;

static void path(int slot, char *out, size_t n) {
  snprintf(out, n, "/K6/PROJECTS/%c.K6P", 'A' + slot);
}

static void gather(ProjectBlob &b) {
  b.magic = MAGIC;
  b.bpm = synth.seqBpm();
  b.key = projKey; b.scale = projScale; b.masterVol = masterVol;
  b.mfxType = masterFx.type;
  memcpy(b.mfxP, masterFx.p, sizeof b.mfxP);
  memcpy(b.engine, trackEngine, sizeof b.engine);
  memcpy(b.vol, trackVol, sizeof b.vol);
  memcpy(b.pan, trackPan, sizeof b.pan);
  for (int t = 0; t < 9; t++) {
    b.mute[t] = trackMute[t];
    b.playing[t] = clipPlaying[t];
    for (int s = 0; s < 2; s++) {
      b.tfxType[t][s] = trackFx[t][s].type;
      memcpy(b.tfxP[t][s], trackFx[t][s].p, 8);
    }
  }
  for (int i = 0; i < 3; i++) memcpy(b.sendP[i], sendFx[i].p, 8);
  memcpy((void *)b.gmProg, gmProgram, sizeof b.gmProg);
  memcpy((void *)b.drumMach, drumMachine, sizeof b.drumMach);
  memcpy(b.layer, trackL, sizeof b.layer);
  b.metro = metroOn;
  memcpy(b.params, engParams, sizeof b.params);
}

static void apply(const ProjectBlob &b) {
  clipFreeze = 1;                                  // the sequencer off the clips
  songOn = false;
  for (int t = 0; t < 9; t++) { clipPlaying[t] = -1; clipPending[t] = -1; }
  fxSetMasterType(FXT_NONE);
  for (int t = 0; t < 9; t++)
    for (int s = 0; s < 2; s++) fxSetTrackType(t, s, FXT_NONE);
  memcpy(gmProgram, b.gmProg, sizeof gmProgram);
  memcpy(drumMachine, b.drumMach, sizeof drumMachine);
  memcpy(trackL, b.layer, sizeof trackL);
  metroOn = b.metro;
  memcpy(engParams, b.params, sizeof engParams);
  memcpy(trackEngine, b.engine, sizeof trackEngine);
  memcpy(trackVol, b.vol, sizeof trackVol);
  memcpy(trackPan, b.pan, sizeof trackPan);
  for (int t = 0; t < 9; t++) trackMute[t] = b.mute[t];
  masterVol = b.masterVol;
  projKey = b.key; projScale = b.scale;
  synth.setSeqBpm(b.bpm);
  for (int i = 0; i < 3; i++) memcpy(sendFx[i].p, b.sendP[i], 8);
  memcpy(masterFx.p, b.mfxP, 8);
  fxSetMasterType(b.mfxType);                      // types last: the activation gate
  for (int t = 0; t < 9; t++)
    for (int s = 0; s < 2; s++) {
      memcpy(trackFx[t][s].p, b.tfxP[t][s], 8);
      fxSetTrackType(t, s, b.tfxType[t][s]);
    }
  for (int t = 0; t < 9; t++)                      // the saved playing state, as it was
    if (b.playing[t] >= 0 && b.playing[t] < 8) clipPlaying[t] = b.playing[t];
  clipFreeze = 0;
}

void projectScan() {
  for (int k = 0; k < 12; k++) {
    char p[32];
    path(k, p, sizeof p);
    projExists[k] = appsSd().exists(p);
  }
  clipFileExists = appsSd().exists("/K6/PROJECTS/CLIP.K6P");
}

bool projectClipFull() { return clipFileExists; }

bool projectSaveCurrent() {
  if (projCurrent < 0) return false;
  char p[32];
  path(projCurrent, p, sizeof p);
  gather(blob);
  FsFile f = appsSd().open(p, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  SongBlob sb = {songLen, 0, {}};
  memcpy(sb.rows, songRows, sizeof sb.rows);
  bool ok = f.write(&blob, sizeof blob) == (int)sizeof blob;
  ok &= f.write(&sb, sizeof sb) == (int)sizeof sb;
  ok &= f.write(clips, sizeof clips) == (int)sizeof clips;   // (loop() owns the clips: safe to stream live)
  f.close();
  if (ok) projExists[projCurrent] = true;
  FsFile c = appsSd().open("/K6/CURRENT.TXT", O_WRONLY | O_CREAT | O_TRUNC);
  if (c) { c.write(&projCurrent, 1); c.close(); }
  return ok;
}

static bool doLoad(uint8_t slot) {
  char p[32];
  path(slot, p, sizeof p);
  FsFile f = appsSd().open(p, O_RDONLY);
  if (!f) return false;
  // K6P1 files carry no song block between the blob and the clips.
  SongBlob sb = {0, 0, {}};
  bool ok = f.read(&blob, sizeof blob) == (int)sizeof blob &&
            (blob.magic == MAGIC || blob.magic == MAGIC_V1);
  if (ok && blob.magic == MAGIC) ok = f.read(&sb, sizeof sb) == (int)sizeof sb;
  if (ok) {                                        // the clips stream in under the freeze
    clipFreeze = 1;
    for (int t = 0; t < 9; t++) { clipPlaying[t] = -1; clipPending[t] = -1; }
    if (f.read(clips, sizeof clips) != (int)sizeof clips) {
      memset(clips, 0, sizeof clips);              // half a read: a clean slate instead
      for (int t = 0; t < 9; t++)
        for (int sc = 0; sc < 8; sc++) clips[t][sc].bars = 1;
    }
  }
  f.close();
  if (!ok) { Serial.printf("project %c: bad file\n", 'A' + slot); return false; }
  apply(blob);
  songLen = sb.n > SONG_MAX ? SONG_MAX : sb.n;
  memcpy(songRows, sb.rows, sizeof songRows);
  projCurrent = slot;
  Serial.printf("project %c loaded\n", 'A' + slot);
  return true;
}

static uint32_t beatNow() { return ((uint32_t)synth.seqBarN() * 16 + synth.seqStepN()) / 4; }

void projectRequestLoad(uint8_t slot) {
  projectSaveCurrent();
  if (!synth.seqOn()) {
    doLoad(slot);
    return;
  }
  pendingLoad = slot;                              // commits on the next beat
  pendingBeat = beatNow();
}

bool projectService() {
  if (pendingLoad < 0) return false;
  if (synth.seqOn() && beatNow() == pendingBeat) return false;
  uint8_t slot = pendingLoad;
  pendingLoad = -1;
  doLoad(slot);
  return true;
}

void projectNew(uint8_t slot) {
  projectSaveCurrent();
  clipFreeze = 1;
  songOn = false;
  songLen = 0;
  memset(songRows, 0, sizeof songRows);
  for (int t = 0; t < 9; t++) { clipPlaying[t] = -1; clipPending[t] = -1; }
  memset(clips, 0, sizeof clips);
  for (int t = 0; t < 9; t++)
    for (int s = 0; s < 8; s++) clips[t][s].bars = 1;
  clipFreeze = 0;
  memset(trackEngine, 0, sizeof trackEngine);
  memset(gmProgram, 0, sizeof gmProgram);
  memset(drumMachine, 0, sizeof drumMachine);
  trackLayerInit();
  metroOn = false;
  paramsInit();
  fxSlotsInit();
  for (int t = 0; t < 9; t++) { trackVol[t] = 80; trackPan[t] = 50; trackMute[t] = false; }
  masterVol = 80;
  projKey = 0; projScale = 0;
  synth.setSeqBpm(132.0f);
  projCurrent = slot;
  projectSaveCurrent();                            // the card fills right away
}

bool projectCut(uint8_t slot) {
  if (slot == projCurrent || !projExists[slot]) return false;
  char p[32];
  path(slot, p, sizeof p);
  if (clipFileExists) appsSd().remove("/K6/PROJECTS/CLIP.K6P");
  bool ok = appsSd().rename(p, "/K6/PROJECTS/CLIP.K6P");
  if (ok) { projExists[slot] = false; clipFileExists = true; }
  return ok;
}

bool projectPaste(uint8_t slot) {
  if (projExists[slot] || !clipFileExists) return false;
  char p[32];
  path(slot, p, sizeof p);
  bool ok = appsSd().rename("/K6/PROJECTS/CLIP.K6P", p);
  if (ok) { projExists[slot] = true; clipFileExists = false; }
  return ok;
}

void projectReloadCurrent() {
  projectScan();
  if (projCurrent >= 0 && projExists[projCurrent]) doLoad(projCurrent);
}

void projectsBegin() {
  projectScan();
  int8_t last = 0;
  FsFile c = appsSd().open("/K6/CURRENT.TXT", O_RDONLY);
  if (c) { if (c.read(&last, 1) != 1 || last < 0 || last > 11) last = 0; c.close(); }
  projCurrent = last;
  if (projExists[projCurrent]) doLoad(projCurrent);
}
