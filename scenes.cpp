// scenes.cpp — see scenes.h
//
// scenes.dat: block 0 is the list (which record slot is at each position,
// how many, and the current position); blocks 1.. are the scenes' records,
// in slots reused as scenes come and go. Inserting or removing a scene only
// rewrites the list block.
#include "scenes.h"
#include "storage.h"

namespace looper {

struct SceneInfo {
  char magic[4];                                 // "M8SC"
  uint32_t version;
  float tempo;                                   // 0: keeps the tempo (the empty scene)
  TrackUnit units[NUM_TRACKS];
};
struct SceneList {
  char magic[4];                                 // "M8SL"
  uint16_t version, count, current, reserved;
  uint16_t slot[MAX_SCENES];                     // record slot of each scene, in list order
};
static constexpr uint32_t BLOCK = 512;
static constexpr uint64_t FILE_BYTES = BLOCK * (1 + (uint64_t)MAX_SCENES);
static_assert(sizeof(SceneList) <= BLOCK && sizeof(SceneInfo) <= BLOCK, "a block each");

static FsFile file;
static SceneList list;
static float (*getTempo)() = nullptr;
static bool (*setTempo)(float) = nullptr;
static bool changed = false;                     // the looper changed: the current scene must follow
static bool loading = false;                     // a scene is being put on the looper
static bool listDirty = false;
static SceneInfo record;                         // waiting to be written to recordSlot
static int recordSlot = -1;
static SceneInfo clip;                           // the scene clipboard
static bool clipFull = false;

int sceneCurrent() { return list.current; }
int sceneCount() { return list.count; }
bool sceneClipboardFull() { return clipFull; }
void sceneStateChanged() { if (!loading) changed = true; }

static SceneInfo emptyScene() {
  SceneInfo s;
  memset(&s, 0, sizeof s);
  memcpy(s.magic, "M8SC", 4);
  s.version = 1;
  for (auto &u : s.units) {
    memcpy(u.magic, "M8TU", 4);
    u.version = 1;
    u.filterHi = FILTER_MAX;
    u.volume = VOLUME_UNITY;
  }
  return s;
}

static SceneInfo snapshot() {
  SceneInfo s = emptyScene();
  s.tempo = getTempo();
  for (int t = 0; t < NUM_TRACKS; t++) s.units[t] = unitOf(t);
  return s;
}

static bool liveEmpty() {
  for (int t = 0; t < NUM_TRACKS; t++) if (unitOf(t).sample) return false;
  return true;
}

static bool writeBlock(uint32_t block, const void *data, size_t n) {
  uint8_t b[BLOCK] = {};
  memcpy(b, data, n);
  debugStage = "SD write (scenes)";
  bool ok = file.seekSet((uint64_t)block * BLOCK) && file.write(b, BLOCK) == (int)BLOCK;
  return file.sync() && ok;
}

static void writeRecordNow() {
  if (recordSlot >= 0) writeBlock(1 + recordSlot, &record, sizeof record);
  recordSlot = -1;
}

// The scene at a position (0: empty; 1..count).
static SceneInfo sceneAt(int pos) {
  if (pos < 1 || pos > list.count) return emptyScene();
  int slot = list.slot[pos - 1];
  if (slot == recordSlot) return record;         // not written yet
  SceneInfo s;
  if (!file.seekSet((uint64_t)(1 + slot) * BLOCK) || file.read(&s, sizeof s) != (int)sizeof s ||
      memcmp(s.magic, "M8SC", 4))
    return emptyScene();
  return s;
}

// A new scene at position pos (1..count + 1), the ones from there on moving
// up; returns its slot, or -1 when the list is full.
static int insertAt(int pos) {
  if (list.count >= MAX_SCENES) return -1;
  bool used[MAX_SCENES] = {};
  for (int i = 0; i < list.count; i++) used[list.slot[i]] = true;
  int slot = 0;
  while (used[slot]) slot++;
  memmove(list.slot + pos, list.slot + pos - 1, (list.count - (pos - 1)) * sizeof list.slot[0]);
  list.slot[pos - 1] = slot;
  list.count++;
  listDirty = true;
  return slot;
}

static void removeAt(int pos) {
  if (list.slot[pos - 1] == recordSlot) recordSlot = -1;   // no need to write it now
  memmove(list.slot + pos - 1, list.slot + pos, (list.count - pos) * sizeof list.slot[0]);
  list.count--;
  listDirty = true;
}

// Saves the looper's state as the current scene: on 0* as a new scene 1, on
// N+1* as scene N+1.
static void commit() {
  if (!changed) return;
  changed = false;
  if (list.current == 0 || list.current == list.count + 1) {
    int pos = list.current == 0 ? 1 : list.count + 1;
    if (insertAt(pos) < 0) {
      Serial.println("scene list full: change not kept in a scene");
      return;
    }
    list.current = pos;
  }
  int slot = list.slot[list.current - 1];
  if (recordSlot >= 0 && recordSlot != slot) writeRecordNow();   // another scene's change: out first
  record = snapshot();
  recordSlot = slot;
}

// Puts a scene on the looper (its tempo, if it has one, after the tracks).
// While playing, all of it changes on the next bar line with time to load.
static void apply(const SceneInfo &s) {
  loading = true;
  int32_t bar = beginSceneChange();
  for (int t = 0; t < NUM_TRACKS; t++) setTrackUnit(t, s.units[t]);
  if (s.tempo > 0) {
    uint32_t before = transport.pendingSpb;
    transport.spbAtBeat = bar;
    if (!setTempo(s.tempo) || transport.pendingSpb == before) transport.spbAtBeat = -1;
  }
  endSceneChange();
  loading = false;
}

static bool busy() { return recording.mode != Recording::Idle; }

static bool go(int pos) {
  commit();
  pos = constrain(pos, 0, list.count + 1);
  if (busy() || pos == list.current) return false;
  list.current = pos;
  listDirty = true;
  apply(sceneAt(min(pos, (int)list.count)));     // N+1*: a copy of N
  return true;
}

bool sceneStep(int dir) { return go(list.current + dir); }
bool sceneGoEmpty() { return go(0); }
bool sceneGoEnd() { return go(list.count + 1); }

bool sceneCut() {
  commit();
  if (busy()) return false;
  clipFull = true;
  if (list.current == 0) {                       // the empty scene: copied, stays
    clip = emptyScene();
    return true;
  }
  clip = snapshot();
  if (list.current <= list.count) removeAt(list.current);   // N+1* was only a copy
  go(list.current - 1);
  return true;
}

bool scenePaste() {
  commit();
  if (busy() || !clipFull) return false;
  int pos = list.current == list.count + 1 ? list.count + 1 : list.current + 1;   // N+1*: replaced
  int slot = insertAt(pos);
  if (slot < 0) return false;
  if (recordSlot >= 0) writeRecordNow();
  record = clip;
  recordSlot = slot;
  list.current = pos;
  apply(clip);
  return true;
}

bool sceneDuplicate() {
  commit();
  if (busy()) return false;
  SceneInfo s = list.current == 0 ? emptyScene() : snapshot();
  int pos = list.current == list.count + 1 ? list.count + 1 : list.current + 1;   // N+1*: made a scene
  int slot = insertAt(pos);
  if (slot < 0) return false;
  if (recordSlot >= 0) writeRecordNow();
  record = s;
  recordSlot = slot;
  list.current = pos;
  return true;
}

void serviceScenes() {
  commit();
  if (!file || (recordSlot < 0 && !listDirty) || !storageMayWrite()) return;
  if (recordSlot >= 0) {                          // one write per call
    writeRecordNow();
  } else {
    writeBlock(0, &list, sizeof list);
    listDirty = false;
  }
}

void scenesBegin(float (*getTempoFn)(), bool (*setTempoFn)(float)) {
  getTempo = getTempoFn;
  setTempo = setTempoFn;
  file = SD.sdfs.open("/looper/scenes.dat", O_RDWR);
  if (!file || file.fileSize() < FILE_BYTES) {    // first time: made at full size
    file.close();
    file = SD.sdfs.open("/looper/scenes.dat", O_RDWR | O_CREAT | O_TRUNC);
    if (file) {
      file.preAllocate(FILE_BYTES);
      uint8_t zero = 0;
      file.seekSet(FILE_BYTES - 1);
      file.write(&zero, 1);
    }
  }
  memset(&list, 0, sizeof list);
  if (file) {
    file.seekSet(0);
    file.read(&list, sizeof list);
  }
  if (memcmp(list.magic, "M8SL", 4) || list.count > MAX_SCENES) {
    memset(&list, 0, sizeof list);
    memcpy(list.magic, "M8SL", 4);
    list.version = 1;
  }
  list.current = min(list.current, (uint16_t)(list.count + 1));
  recordSlot = -1;
  listDirty = true;
  // The looper holds clips but sits on the empty scene (first start with
  // scenes): they become scene 1.
  changed = list.current == 0 && !liveEmpty();
  commit();
  if (file) {
    writeRecordNow();
    writeBlock(0, &list, sizeof list);
    listDirty = false;
  }
}

}  // namespace looper
