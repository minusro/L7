// scenes.cpp — see scenes.h
//
// A project file (/L7/projects/1.dat ... 12.dat; clip.dat the clipboard):
// block 0 is the list (which record slot is at each position, how many, the
// current position, the selected row and cell, and the project's tempo: one
// for all its scenes); blocks 1.. are the
// scenes' records, in slots reused as scenes come and go. Inserting or
// removing a scene only rewrites the list block. All of them are made once at
// full size; an empty slot's block 0 is zeros.
#include "scenes.h"
#include "storage.h"
#include <utility>

namespace looper {

struct SceneInfo {
  char magic[4];                                 // "M8SC"
  uint32_t version;
  float tempo;                                   // the tempo when saved (projects keep one tempo now:
                                                 // an older project's comes from its current scene's)
  TrackUnit units[NUM_TRACKS];
  uint8_t row, cell;                             // the selection on it, each + 1 (0: none saved; older records)
};
struct SceneList {
  char magic[4];                                 // "M8SL"
  uint16_t version, count, current;
  uint8_t row, cell;                             // the selection, each + 1 (0: none saved)
  uint16_t slot[MAX_SCENES];                     // record slot of each scene, in list order
  uint16_t tempo10;                              // the project's tempo, tenths of a BPM (0: not kept yet, older files)
};
static constexpr uint32_t BLOCK = 512;
static constexpr uint64_t FILE_BYTES = BLOCK * (1 + (uint64_t)MAX_SCENES);
static_assert(sizeof(SceneList) <= BLOCK && sizeof(SceneInfo) <= BLOCK, "a block each");

static FsFile file;                              // the loaded project's
static SceneList list;
static constexpr int CLIP = PROJECTS, DUMMY = PROJECTS + 1;   // the clipboard's file, the dummy project's
static int project = 0;                          // the loaded one (0-11, or DUMMY)
static bool filled[PROJECTS];                    // the slots holding a project
static bool projectClip = false;                 // the project clipboard holds one
static float (*getTempo)() = nullptr;
static bool (*setTempo)(float) = nullptr;        // a project's tempo, as it loads (not a change by hand)
static float (*newTempo)() = nullptr;            // new projects' tempo (the last one set by hand)
static uint16_t tenths(float b) { return (uint16_t)lroundf(b * 10); }
static bool changed = false;                     // the looper changed: the current scene must follow
static bool loading = false;                     // a scene is being put on the looper
static bool listDirty = false;
static SceneInfo record;                         // waiting to be written to recordSlot
static int recordSlot = -1;
static SceneInfo clip;                           // the scene clipboard
static bool holding = false;                     // stepping with the change held back (see sceneHold)
static int target = -1;                          // where the held steps got to (-1: none)
static bool clipFull = false;
static int8_t clipsKnown = -1;                   // projectHasClips()'s answer for the stored scenes (-1: to find out)

int sceneCurrent() { return target >= 0 ? target : list.current; }
bool sceneHeld() { return target >= 0; }
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
    u.version = 3;                                 // (volume and balance in today's steps: UNIT_VERSION)
    u.filterHi = FILTER_MAX;
    u.volume = VOLUME_UNITY;
  }
  return s;
}

// The looper's selection (row, cell), from the app (scenesSelection()).
static void (*getSel)(int &, int &) = nullptr;
static void (*setSel)(int, int) = nullptr;
void scenesSelection(void (*get)(int &, int &), void (*set)(int, int)) {
  getSel = get;
  setSel = set;
}

static SceneInfo snapshot() {
  SceneInfo s = emptyScene();
  s.tempo = getTempo();
  for (int t = 0; t < NUM_TRACKS; t++) s.units[t] = unitOf(t);
  if (getSel) {
    int r, c;
    getSel(r, c);
    s.row = r + 1;
    s.cell = c + 1;
  }
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
  for (auto &u : s.units) upgradeUnit(u);          // (saved before volume's steps changed)
  return s;
}

// A new scene at position pos (1..count + 1), the ones from there on moving
// up; returns its slot, or -1 when the list is full.
static int insertAt(int pos) {
  clipsKnown = -1;
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
  clipsKnown = -1;
  if (list.slot[pos - 1] == recordSlot) recordSlot = -1;   // no need to write it now
  memmove(list.slot + pos - 1, list.slot + pos, (list.count - pos) * sizeof list.slot[0]);
  list.count--;
  listDirty = true;
}

// Saves the looper's state as the current scene: on 0* as a new scene 1, on
// N+1* as scene N+1.
static void commit() {
  if (!changed) return;
  clipsKnown = -1;
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

// Puts a scene on the looper; tempo > 0 (a project loading): that tempo too,
// after the tracks (a scene keeps the tempo: it's the project's). While
// playing, all of it changes on the next bar line with time to load (quick:
// the next beat).
static void apply(const SceneInfo &s, bool quick, bool restart = false, float tempo = 0) {
  loading = true;
  int32_t at = beginSceneChange(quick, unitsPrepared(), restart);
  for (int t = 0; t < NUM_TRACKS; t++) setTrackUnit(t, s.units[t]);   // (they take the prepared files)
  prepareUnits(nullptr);
  if (tempo > 0) {
    setTempo(tempo);
    // A tempo change on its way (this one, or one still waiting) goes with it.
    transport.spbAtBeat = transport.pendingSpb ? at : -1;
  }
  endSceneChange();
  loading = false;
}

static bool busy() { return recording.mode != Recording::Idle; }

// To scene pos. While SHIFT is held only the label moves (see sceneHold), unless
// quick: then at once, switching on the next beat.
// The current scene (a stored one) keeps the selection there now (leaving
// it, or the selection moved: written with the record).
static void keepSceneSelection() {
  if (!getSel || list.current < 1 || list.current > list.count) return;
  int r, c;
  getSel(r, c);
  SceneInfo s = sceneAt(list.current);
  if (s.row == r + 1 && s.cell == c + 1) return;
  s.row = r + 1;
  s.cell = c + 1;
  int slot = list.slot[list.current - 1];
  if (recordSlot >= 0 && recordSlot != slot) writeRecordNow();
  record = s;
  recordSlot = slot;
}

// Arriving on scene s: its selection, if it has one.
static void takeSceneSelection(const SceneInfo &s) {
  if (setSel && s.row && s.cell) setSel(s.row - 1, s.cell - 1);
}

static bool go(int pos, bool quick = false) {
  pos = constrain(pos, 0, list.count ? list.count + 1 : 0);   // (no scenes: no N+1*, a copy of nothing)
  if (pos == 0 && !holding) quick = true;          // to the empty scene: the next beat (SHIFT held: once it's let go)
  if (holding && !quick) {                       // only the label moves for now, the scene is prepared
    if (busy() || pos == sceneCurrent()) return false;
    target = pos == list.current ? -1 : pos;
    if (target >= 0) prepareUnits(sceneAt(min(target, (int)list.count)).units);   // N+1*: a copy of N
    else prepareUnits(nullptr);
    return true;
  }
  if (quick && target >= 0) {                   // a held selection: passed over
    if (busy()) return false;
    target = -1;
  }
  if (quick) prepareUnits(nullptr);                // (files prepared for another scene: not these)
  commit();
  if (busy() || pos == list.current) return false;
  keepSceneSelection();
  list.current = pos;
  listDirty = true;
  SceneInfo s = sceneAt(min(pos, (int)list.count));   // N+1*: a copy of N
  apply(s, quick);
  takeSceneSelection(s);
  return true;
}

// Steps to and from N+1* (the copy of the last scene) are quick.
bool sceneStep(int dir) {
  int from = sceneCurrent(), end = list.count + 1;
  return go(from + dir, from == end || from + dir == end);
}

// Let go: the held selection launches once its samples are open (at once if
// they are; otherwise from serviceScenes(), the label flashing meanwhile).
static bool launchHeld() {
  if (holding || target < 0 || !unitsPrepared()) return false;
  int pos = target;
  target = -1;
  return go(pos);
}

bool sceneHold(bool on) {
  holding = on;
  return launchHeld();
}

// A held selection is taken first (cut, paste and duplicate act on it).
static void takeHeld() {
  if (target < 0) return;
  int pos = target;
  target = -1;
  go(pos, true);
}
bool sceneGoEmpty() { return go(0, true); }
bool sceneGoEnd() { return go(list.count + 1, true); }

bool sceneCut() {
  takeHeld();
  commit();
  if (busy()) return false;
  clipFull = true;
  if (list.current == 0) {                       // the empty scene: copied, stays
    clip = emptyScene();
    return true;
  }
  clip = snapshot();
  if (list.current <= list.count) removeAt(list.current);   // N+1* was only a copy
  go(list.current - 1, true);
  return true;
}

bool scenePaste() {
  takeHeld();
  commit();
  if (busy() || !clipFull) return false;
  int pos = list.current == list.count + 1 ? list.count + 1 : list.current + 1;   // N+1*: replaced
  int slot = insertAt(pos);
  if (slot < 0) return false;
  if (recordSlot >= 0) writeRecordNow();
  record = clip;
  recordSlot = slot;
  list.current = pos;
  apply(clip, true);
  return true;
}

bool sceneDuplicate() {
  takeHeld();
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

bool sceneFromResample(int t) {
  takeHeld();
  commit();                                        // (the current scene, the resampling on it)
  SceneInfo from = snapshot(), to = emptyScene();
  to.tempo = from.tempo;
  to.units[t] = from.units[t];                     // (on the same track)
  from.units[t] = emptyScene().units[t];
  for (int u = 0; u < NUM_TRACKS; u++)              // the muted clips come along, still muted (and stay here too)
    if (u != t && from.units[u].sample && from.units[u].muted) {
      to.units[u] = from.units[u];
      hearAgain(u);                                  // (silenced with the others: its clip doesn't change, so it's heard once unmuted)
    }
  int pos = list.current == list.count + 1 ? list.count + 1 : list.current + 1;
  if (list.count >= MAX_SCENES) return false;
  if (list.current >= 1 && list.current <= list.count) {   // the current scene, without it
    int slot = list.slot[list.current - 1];
    if (recordSlot >= 0 && recordSlot != slot) writeRecordNow();
    record = from;
    recordSlot = slot;
  }
  int slot = insertAt(pos);
  if (recordSlot >= 0) writeRecordNow();
  record = to;
  recordSlot = slot;
  list.current = pos;
  apply(to, true);                                 // on the next beat
  return true;
}

static bool copyOn();
static void stepCopy();

void serviceScenes() {
  launchHeld();                                    // let go before it was prepared: now it is
  commit();
  if (copyOn()) {                                  // a project copy: a block now and then
    if (!transport.running) {
      while (copyOn()) stepCopy();
    } else if (storageMayWrite()) {
      stepCopy();
      return;                                      // (one write per call)
    }
  }
  if (!file || (recordSlot < 0 && !listDirty) || !storageMayWrite()) return;
  if (recordSlot >= 0) {                          // one write per call
    writeRecordNow();
  } else {
    writeBlock(0, &list, sizeof list);
    listDirty = false;
  }
}

// ---- Projects

static void projectPath(char *path, size_t n, int p) {   // p: 0-11, CLIP, DUMMY
  if (p < PROJECTS) snprintf(path, n, "/L7/projects/%d.dat", p + 1);
  else snprintf(path, n, p == CLIP ? "/L7/projects/clip.dat" : "/L7/projects/none.dat");
}

static FsFile openProject(int p) {
  char path[40];
  projectPath(path, sizeof path, p);
  return SD.sdfs.open(path, O_RDWR);
}

static SceneList newList() {
  SceneList l;
  memset(&l, 0, sizeof l);
  memcpy(l.magic, "M8SL", 4);
  l.version = 1;
  l.tempo10 = tenths(newTempo ? newTempo() : 120.0f);   // a new project: the last tempo set by hand
  return l;
}

// A file's list (false: an empty slot).
static bool readList(FsFile &f, SceneList &l) {
  memset(&l, 0, sizeof l);
  if (!f || !f.seekSet(0) || f.read(&l, sizeof l) != (int)sizeof l) return false;
  return !memcmp(l.magic, "M8SL", 4) && l.count <= MAX_SCENES;
}

static bool writeBlockTo(FsFile &f, uint32_t block, const void *data, size_t n) {
  uint8_t b[BLOCK] = {};
  memcpy(b, data, n);
  debugStage = "SD write (projects)";
  bool ok = f.seekSet((uint64_t)block * BLOCK) && f.write(b, BLOCK) == (int)BLOCK;
  return f.sync() && ok;
}

// The loaded project's changes, all written now.
static void flushProject() {
  commit();
  writeRecordNow();
  if (file) writeBlock(0, &list, sizeof list);
  listDirty = false;
}

// Copying a project (cut and paste): its scenes' records, then the list
// (written last: a copy cut short leaves `to` empty), then for a cut the
// source emptied. Playing, it goes on in the background, a block per pass of
// serviceScenes() when the buffers allow; stopped, at once.
static struct {
  bool on = false, clearFrom = false;
  int from = 0, to = 0, i = 0;                   // i: the next record to copy
  SceneList l;
  FsFile src, dst;
} job;

static void endCopy(bool ok) {
  if (!ok && job.clearFrom && job.from < PROJECTS) filled[job.from] = true;   // (a cut that failed: still there)
  if (ok && job.to == CLIP) projectClip = true;
  job.on = false;
  job.src.close();
  job.dst.close();
}

static void stepCopy() {
  if (job.i < job.l.count) {
    uint8_t b[BLOCK];
    uint64_t at = (uint64_t)(1 + job.l.slot[job.i++]) * BLOCK;
    bool ok = job.src.seekSet(at) && job.src.read(b, BLOCK) == (int)BLOCK;
    debugStage = "SD write (projects)";
    ok = ok && job.dst.seekSet(at) && job.dst.write(b, BLOCK) == (int)BLOCK && job.dst.sync();
    if (!ok) endCopy(false);
    return;
  }
  bool ok = writeBlockTo(job.dst, 0, &job.l, sizeof job.l);
  if (ok && job.clearFrom) writeBlockTo(job.src, 0, "", 0);
  endCopy(ok);
}

static bool startCopy(int from, int to, bool clearFrom) {
  if (job.on) return false;
  if (from == project) flushProject();
  job.src = openProject(from);
  job.dst = openProject(to);
  if (!readList(job.src, job.l) || !job.dst) {
    job.src.close();
    job.dst.close();
    return false;
  }
  job.on = true;
  job.from = from;
  job.to = to;
  job.i = 0;
  job.clearFrom = clearFrom;
  if (to == CLIP) projectClip = false;           // (being replaced)
  while (job.on && !transport.running) stepCopy();   // stopped: all of it now
  return true;
}

bool projectCopying(int p) { return job.on && (job.from == p || job.to == p); }
static bool copyOn() { return job.on; }

static void clearProject(int p) {
  FsFile f = openProject(p);
  if (f) writeBlockTo(f, 0, "", 0);
}

void flushScenes() { flushProject(); }

int projectCurrent() { return project == DUMMY ? -1 : project; }
bool projectFilled(int p) { return p >= 0 && p < PROJECTS && filled[p]; }
bool projectClipboardFull() { return projectClip; }
bool projectHasClips() {
  if (project == DUMMY) return false;
  for (int t = 0; t < NUM_TRACKS; t++)
    if (unitOf(t).sample) return true;              // (on the tracks now)
  if (clipsKnown < 0) {                            // the stored scenes, read once after each change
    clipsKnown = 0;
    for (int pos = 1; pos <= list.count && !clipsKnown; pos++) {
      SceneInfo s = sceneAt(pos);
      for (const auto &u : s.units)
        if (u.sample) clipsKnown = 1;
    }
  }
  return clipsKnown;
}

bool projectsMayCopy() { return !busy() && !job.on; }

void projectSelection(int &row, int &cell) {
  if (list.current >= 1 && list.current <= list.count) {   // its scene's own, if it has one
    SceneInfo s = sceneAt(list.current);
    if (s.row && s.cell) {
      row = s.row - 1;
      cell = s.cell - 1;
      return;
    }
  }
  row = list.row ? list.row - 1 : -1;
  cell = list.cell ? list.cell - 1 : -1;
}

void projectSetSelection(int row, int cell) {
  uint8_t r = row < 0 ? 0 : row + 1, c = cell < 0 ? 0 : cell + 1;
  keepSceneSelection();                            // (the scene's own too)
  if (r == list.row && c == list.cell) return;
  list.row = r;
  list.cell = c;
  listDirty = true;
}

// The looper on project p (its file open as `file`, its list in `list`): an
// empty slot gets a new project (no scenes: the empty scene, the tempo last
// set by hand), and so does DUMMY every time. Its tempo goes on the looper
// (one saved before projects kept a tempo: its current scene's, kept now);
// apply: its current scene too.
static bool openAs(int p, bool apply_) {
  clipsKnown = -1;
  FsFile f = openProject(p);
  if (!f) return false;
  SceneList l;
  if (p == DUMMY || !readList(f, l)) {
    l = newList();
    if (!writeBlockTo(f, 0, &l, sizeof l)) return false;
  }
  l.current = min(l.current, (uint16_t)(l.count + 1));
  file = std::move(f);
  list = l;
  project = p;
  setRecordingLetter(p < PROJECTS ? (char)('A' + p) : 0);   // its recordings are named after it
  if (p < PROJECTS) filled[p] = true;
  recordSlot = -1;
  listDirty = false;
  changed = false;
  SceneInfo s = sceneAt(min((int)list.current, (int)list.count));   // N+1*: a copy of N
  float tempo = list.tempo10 ? list.tempo10 / 10.0f : s.tempo > 0 ? s.tempo : getTempo();
  if (!list.tempo10) {
    list.tempo10 = tenths(tempo);
    listDirty = true;
  }
  loading = true;                                 // (the scene's units go on as they are)
  if (apply_) apply(s, false, true, tempo);       // from 1.1.1
  else setTempo(tempo);
  loading = false;
  return true;
}

void projectTempoChanged() {
  if (loading || tenths(getTempo()) == list.tempo10) return;
  list.tempo10 = tenths(getTempo());
  listDirty = true;
}

bool projectLoad(int p) {
  if (p < 0 || p >= PROJECTS || busy() || projectCopying(p)) return false;
  if (p == project) return true;
  target = -1;
  prepareUnits(nullptr);
  flushProject();
  return openAs(p, true);
}

bool projectCut(int p) {
  if (!projectsMayCopy() || !projectFilled(p)) return false;
  filled[p] = false;                               // (shown empty at once; emptied once copied)
  if (!startCopy(p, CLIP, true)) {
    filled[p] = true;
    return false;
  }
  if (p == project) {                              // the loaded one: the dummy project instead (empty)
    file.close();
    target = -1;
    prepareUnits(nullptr);
    openAs(DUMMY, false);
    loading = true;
    apply(emptyScene(), true);                     // silent from the next beat (the clock runs on)
    loading = false;
  }
  return true;
}

bool projectDelete(int p) {
  if (!projectFilled(p) || busy() || projectCopying(p)) return false;
  if (p == project) flushProject();
  clearProject(p);
  filled[p] = false;
  if (p == project) {                              // the loaded one: the dummy project instead (empty)
    file.close();
    target = -1;
    prepareUnits(nullptr);
    openAs(DUMMY, false);
    loading = true;
    apply(emptyScene(), false);
    loading = false;
  }
  return true;
}

bool projectPaste(int p) {
  if (!projectsMayCopy() || !projectClip || p < 0 || p >= PROJECTS || filled[p]) return false;
  if (!startCopy(CLIP, p, false)) return false;
  filled[p] = true;                                // (loadable once the copy is done)
  return true;
}

int deleteUnusedSamples() {
  if (transport.running || busy() || job.on) return 0;
  flushProject();
  prepareUnits(nullptr);                           // (no prepared sample files open)
  static uint8_t used[(9999 + 8) / 8];
  memset(used, 0, sizeof used);
  auto mark = [](const TrackUnit &u) { if (u.sample && u.sample <= 9999) used[u.sample / 8] |= 1 << (u.sample % 8); };
  for (int p = 0; p <= DUMMY; p++) {               // every project's scenes (the clipboard's and the dummy's too)
    FsFile f = openProject(p);
    SceneList l;
    if (!readList(f, l)) continue;
    for (int i = 0; i < l.count; i++) {
      SceneInfo s;
      if (f.seekSet((uint64_t)(1 + l.slot[i]) * BLOCK) && f.read(&s, sizeof s) == (int)sizeof s && !memcmp(s.magic, "M8SC", 4))
        for (const auto &u : s.units) mark(u);
    }
  }
  for (int t = 0; t < NUM_TRACKS; t++) mark(unitOf(t));   // the tracks now (muted ones too)
  if (clipFull)
    for (const auto &u : clip.units) mark(u);      // the scene clipboard
  return deleteSamplesExcept(used);
}

// Opens (or makes, at full size) project files 1-12, the clipboard's and the
// dummy project's; the
// scene list of the first version (/L7/scenes.dat, from /looper) becomes project 1.
static void makeProjectFiles() {
  SD.sdfs.mkdir("/L7/projects");
  char path[40];
  projectPath(path, sizeof path, 0);
  FsFile old = SD.sdfs.open("/L7/scenes.dat", O_RDONLY);
  if (old) {
    old.close();
    if (!SD.sdfs.open(path, O_RDONLY)) SD.sdfs.rename("/L7/scenes.dat", path);
  }
  for (int p = 0; p <= DUMMY; p++) {
    FsFile f = openProject(p);
    if (!f || f.fileSize() < FILE_BYTES) {          // first time: made at full size, empty
      f.close();
      projectPath(path, sizeof path, p);
      f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
      if (!f) continue;
      f.preAllocate(FILE_BYTES);
      uint8_t zero = 0;
      f.seekSet(FILE_BYTES - 1);
      f.write(&zero, 1);
      writeBlockTo(f, 0, "", 0);
    }
    SceneList l;
    bool full = readList(f, l);
    if (p < PROJECTS) filled[p] = full;
    else if (p == CLIP) projectClip = full;
  }
}

void scenesBegin(float (*getTempoFn)(), bool (*setTempoFn)(float), int loaded, float (*newTempoFn)()) {
  getTempo = getTempoFn;
  setTempo = setTempoFn;
  newTempo = newTempoFn;
  makeProjectFiles();
  loaded = loaded < 0 ? DUMMY : min(loaded, PROJECTS - 1);
  if (!openAs(loaded, false)) {
    memset(&list, 0, sizeof list);
    list = newList();
    project = loaded;
  }
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
