// scenes.cpp — see scenes.h
//
// A project file (/L7/PROJECTS/A.L7P ... L.L7P, none for an empty slot;
// /L7/DATA/CLIP.DAT the clipboard, NONE.DAT the dummy project's):
// block 0 is the list (which record slot is at each position, how many, the
// current position, the selected row and cell, and the project's tempo and
// master volume: one for all its scenes); blocks 1.. are the
// scenes' records, in slots reused as scenes come and go. Inserting or
// removing a scene only rewrites the list block. All of them are made once at
// full size (a project's when its slot gets one).
#include "scenes.h"
#include "audio_follow.h"
#include "midi_in.h"
#include "storage.h"
#include <utility>

namespace looper {

struct SceneInfo {
  char magic[4];                                 // "M8SC"
  uint32_t version;
  float tempo;                                   // the scene's saved tempo: the one it was made at, then its latest
                                                 // recording's (sceneSavedTempo()); before, the tempo when it was last
                                                 // saved (an older project's own tempo comes from its current scene's)
  TrackUnit units[NUM_TRACKS];
  uint8_t row, cell;                             // (unused: the selection a scene kept, before it stopped keeping one)
};
struct SceneList {
  char magic[4];                                 // "M8SL"
  uint16_t version, count, current;
  uint8_t row, cell;                             // (unused: the selection a project kept, before it stopped keeping one)
  uint16_t slot[LIST_SLOTS];                     // record slot of each scene, in list order
  // Its master volume and balance: MASTER_KEPT, the volume's step << 6, the
  // balance + 32 (MASTER_KEPT not there: not kept yet, older files, where
  // these 2 bytes held the tempo its last recording was made at, before each
  // scene kept its own, and before that a master volume and a spare).
  uint16_t master;
  uint16_t tempo10;                              // the project's tempo, tenths of a BPM (0: not kept yet, older files)
};
static constexpr uint16_t MASTER_KEPT = 0xF000;   // (no tempo's tenths reach it)
static uint16_t masterOf(int volume, int balance) { return MASTER_KEPT | volume << 6 | (balance + 32); }
static_assert(MASTER_VOLUME_MAX < 32 && MASTER_BALANCE_MAX < 32, "the master volume and balance fit their bits");
static constexpr uint32_t BLOCK = 512;
static constexpr uint64_t FILE_BYTES = BLOCK * (1 + (uint64_t)LIST_SLOTS);
static_assert(sizeof(SceneList) <= BLOCK && sizeof(SceneInfo) <= BLOCK, "a block each");

static FsFile file;                              // the loaded project's
static SceneList list;
static constexpr int CLIP = PROJECTS, DUMMY = PROJECTS + 1;   // the clipboard's file, the dummy project's
static int project = 0;                          // the loaded one (0-11, or DUMMY)
static bool filled[PROJECTS];                    // the slots holding a project
static bool projectClip = false;                 // the project clipboard holds one (always with clips)
static float (*getTempo)() = nullptr;
static bool (*setTempo)(float) = nullptr;        // a project's tempo, as it loads (not a change by hand)
static float (*newTempo)() = nullptr;            // new projects' tempo (the last one set by hand)
static uint16_t tenths(float b) { return (uint16_t)lroundf(b * 10); }
static bool changed = false;                     // the looper changed: the current scene must follow
static bool loading = false;                     // a scene is being put on the looper
static bool listDirty = false;
static SceneInfo record;                         // waiting to be written to recordSlot
static int recordSlot = -1;
static int madeSlot = -1;                         // the scene just made from N+1* (cut there and then: back to N+1*)
static SceneInfo clip;                           // the scene clipboard
static bool holding = false;                     // stepping with the change held back (see sceneHold)
static int target = -1;                          // where the held steps got to (-1: none)
static bool clipFull = false;
static int8_t clipsKnown = -1;                   // projectHasClips()'s answer for the stored scenes (-1: to find out)
// The scene on the looper: its saved tempo (0: none yet, on 0* or N+1*: made
// a scene, it takes the tempo then, or its recording's); a resampling's,
// waiting for the scene it moves to (sceneFromResample()).
static float sceneTempo = 0, resampleTempo = 0;

int sceneCurrent() { return target >= 0 ? target : list.current; }
bool sceneHeld() { return target >= 0; }
static TrackUnit heldUnits[NUM_TRACKS];          // the held selection's tracks (shown before it plays)
bool sceneHeldUnit(int t, TrackUnit &u) {
  if (target < 0) return false;
  u = heldUnits[t];
  return true;
}
bool scenePreviewing() { return target >= 0 || sceneChangeWaiting(); }
int sceneCount() { return list.count; }
bool sceneClipboardFull() { return clipFull; }
static uint32_t edits = 0;                       // the loaded project's edits so far (projectEdits())
void sceneStateChanged() {
  if (loading) return;
  changed = true;
  edits++;
}
uint32_t projectEdits() { return edits; }

static SceneInfo emptyScene() {
  SceneInfo s;
  memset(&s, 0, sizeof s);
  memcpy(s.magic, "M8SC", 4);
  s.version = 1;
  for (auto &u : s.units) {
    memcpy(u.magic, "M8TU", 4);
    u.version = 6;                                 // (volume and balance in today's steps: UNIT_VERSION)
    u.filterHi = FILTER_MAX;
    u.volume = VOLUME_UNITY;
  }
  return s;
}

static SceneInfo snapshot() {
  SceneInfo s = emptyScene();
  s.tempo = sceneTempo > 0 ? sceneTempo : getTempo();
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
  for (auto &u : s.units) upgradeUnit(u);          // (saved before volume's steps changed)
  return s;
}

// A new scene at position pos (1..count + 1), the ones from there on moving
// up; returns its slot, or -1 when the list is full.
static int insertAt(int pos) {
  clipsKnown = -1;
  edits++;
  if (list.count >= MAX_SCENES) return -1;
  bool used[LIST_SLOTS] = {};
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
  edits++;
  if (list.slot[pos - 1] == recordSlot) recordSlot = -1;   // no need to write it now
  if (list.slot[pos - 1] == madeSlot) madeSlot = -1;
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
    madeSlot = pos == list.count ? list.slot[pos - 1] : -1;
    if (sceneTempo <= 0) sceneTempo = getTempo();   // made now: the tempo now (a recording made it: its own, already)
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
static void apply(const SceneInfo &s, bool quick, bool restart = false, float tempo = 0, bool clockToo = true) {
  debugStage = "scene apply";
  loading = true;
  int32_t at = beginSceneChange(quick, unitsPrepared(), restart, clockToo);
  for (int t = 0; t < NUM_TRACKS; t++) setTrackUnit(t, s.units[t]);   // (they take the prepared files)
  prepareUnits(nullptr);
  if (tempo > 0 && syncSource() == SYNC_NONE) {   // (following an external tempo: theirs, the scene's kept for later)
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

static bool go(int pos, bool quick = false) {
  pos = constrain(pos, 0, list.count ? list.count + 1 : 0);   // (no scenes: no N+1*, a copy of nothing)
  if (pos == 0 && !holding) quick = true;          // to the empty scene: the next beat (SHIFT held: once it's let go)
  if (holding && !quick) {                       // only the label moves for now, the scene is prepared
    if (busy() || pos == sceneCurrent()) return false;
    target = pos == list.current ? -1 : pos;
    if (target >= 0) {
      const SceneInfo s = sceneAt(min(target, (int)list.count));   // N+1*: a copy of N
      memcpy(heldUnits, s.units, sizeof heldUnits);
      prepareUnits(s.units);
    }
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
  const int from = list.current;
  madeSlot = -1;                                   // (left: no longer just made)
  list.current = pos;
  listDirty = true;
  SceneInfo s = sceneAt(min(pos, (int)list.count));   // N+1*: a copy of N
  sceneTempo = pos >= 1 && pos <= list.count ? s.tempo : 0;   // (0* and N+1*: none until made a scene)
  // Every clip from its start position (as long a range: as far from it as the
  // one before), the clock running on; between N and N+1* (its copy) nothing
  // starts over. Each scene its own tempo (0*, N+1*: as it is).
  const bool copyStep = (from == list.count && pos == list.count + 1) || (from == list.count + 1 && pos == list.count);
  apply(s, quick, !copyStep, sceneTempo, false);
  return true;
}

// Steps to and from N+1* (the copy of the last scene) are quick, once the
// modifier is let go (held: as any other, launchHeld()).
bool sceneStep(int dir) {
  int from = sceneCurrent(), end = list.count + 1;
  return go(from + dir, !holding && (from == end || from + dir == end));
}
// (Launched from a hold: quick to or from 0* or N+1*, as a step there.)
static bool quickTo(int pos) { return pos == 0 || pos == list.count + 1 || list.current == list.count + 1; }

// Let go: the held selection launches once its samples are open (at once if
// they are; otherwise from serviceScenes(), the label flashing meanwhile).
static bool launchHeld() {
  if (holding || target < 0 || !unitsPrepared()) return false;
  int pos = target;
  target = -1;
  return go(pos, quickTo(pos));
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
bool sceneGoEmpty() { return go(0, !holding); }   // (held: once let go, as any step)

bool sceneNext() {
  int from = sceneCurrent(), end = list.count + 1;
  if (!list.count) return false;                   // (no scenes: only 0*)
  if (from >= (int)list.count) return go(1, from == end);   // the last, or N+1*: round to 1 (from N+1*: quick, as a step from it)
  return sceneStep(1);                             // (0*: to 1 too)
}
bool sceneGoEnd() { return go(list.count + 1, !holding); }

bool sceneCut() {
  takeHeld();
  commit();
  if (busy()) return false;
  if (list.current == 0) {                         // the empty scene: stays, the clipboard emptied
    clipFull = false;
    return true;
  }
  if (!liveEmpty()) {                            // (a scene with no clips: cut, but not to the clipboard, which keeps what it had)
    clip = snapshot();
    clipFull = true;
  }
  // Just made from N+1* (still the last): back to N+1*, a copy of the scene
  // before it; otherwise to the scene before.
  const bool made = list.current == list.count && madeSlot >= 0 && list.slot[list.current - 1] == madeSlot;
  if (list.current <= list.count) removeAt(list.current);   // N+1* was only a copy
  if (made) list.current--;                        // (as if from N: the step to N+1*)
  go(made ? list.current + 1 : list.current - 1, true);
  return true;
}

bool scenePaste() {
  takeHeld();
  commit();
  if (busy()) return false;
  int pos = list.current == list.count + 1 ? list.count + 1 : list.current + 1;   // N+1*: replaced
  int slot = insertAt(pos);
  if (slot < 0) return false;
  if (recordSlot >= 0) writeRecordNow();
  // The clipboard empty: an empty scene, at the tempo now.
  SceneInfo s = clipFull ? clip : emptyScene();
  if (!clipFull) s.tempo = getTempo();
  record = s;
  recordSlot = slot;
  list.current = pos;
  sceneTempo = s.tempo;                            // (the scene moved: its own)
  apply(s, true, false, sceneTempo);
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
  s.tempo = sceneTempo = getTempo();              // (a new scene: made at the tempo now)
  record = s;
  recordSlot = slot;
  list.current = pos;
  return true;
}

int sceneUnitState(int t) {
  if (t < 0 || t >= NUM_TRACKS || recordSlot < 0) return -1;
  return !record.units[t].sample ? 0 : record.units[t].muted ? 1 : 2;
}

bool sceneClear() {
  takeHeld();
  commit();
  if (busy() || list.current < 1 || list.current > list.count) return false;
  SceneInfo e = emptyScene();
  e.tempo = sceneTempo;                            // (its tempo kept)
  int slot = list.slot[list.current - 1];
  if (recordSlot >= 0 && recordSlot != slot) writeRecordNow();
  record = e;
  recordSlot = slot;
  apply(e, true);                                  // on the next beat
  return true;
}

FLASHMEM int sceneFromResample(int t) {
  takeHeld();
  commit();                                        // (the current scene, the resampling on it)
  SceneInfo from = snapshot(), to = emptyScene();
  to.tempo = resampleTempo > 0 ? resampleTempo : getTempo();   // (made by the resampling: its tempo)
  resampleTempo = 0;
  // On track 1 (the bottom row), unless another recording runs there (then
  // its own); the muted clips come along, still muted (and stay here too),
  // in their order, on the tracks right after it (2, 3 ...: no gaps; the
  // track another recording runs on stepped over).
  const int one = NUM_TRACKS - 1;
  const int rec = recording.mode != Recording::Idle ? recording.track : -1;
  const int d = rec == one && t != one ? t : one;
  if (list.count >= MAX_SCENES) return -1;
  to.units[d] = from.units[t];
  // Its volume: as loud as what it resampled. The mix it recorded already had
  // the tracks' headroom (masterGain, -3.75 dB) in it, and playing it back
  // adds that again: two steps up (+3.75 dB), exactly.
  to.units[d].volume = VOLUME_UNITY + 2;
  from.units[t] = emptyScene().units[t];
  int next = d - 1;                                // (the next track up: its number one more)
  for (int u = NUM_TRACKS - 1; u >= 0; u--)        // (from track 1 up)
    if (u != t && from.units[u].sample && from.units[u].muted) {
      while (next == rec) next--;
      if (next < 0) break;
      to.units[next] = from.units[u];
      hearAgain(next);                             // (silenced with the others: heard once unmuted)
      next--;
    }
  int pos = list.current == list.count + 1 ? list.count + 1 : list.current + 1;
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
  sceneTempo = to.tempo;
  apply(to, true, false, sceneTempo);              // on the next beat (its tempo)
  return d;
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
  if (p < PROJECTS) snprintf(path, n, "/L7/PROJECTS/%c.L7P", 'A' + p);
  else snprintf(path, n, p == CLIP ? "/L7/DATA/CLIP.DAT" : "/L7/DATA/NONE.DAT");
}

static FsFile openProject(int p) {
  char path[40];
  projectPath(path, sizeof path, p);
  return SD.sdfs.open(path, O_RDWR);
}

// A new project file at full size, empty (a slot's file is made when it's first
// used, and removed when it's emptied).
FLASHMEM static FsFile createProject(int p) {
  char path[40];
  projectPath(path, sizeof path, p);
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!f) return f;
  f.preAllocate(FILE_BYTES);
  uint8_t zero = 0;
  f.seekSet(FILE_BYTES - 1);
  f.write(&zero, 1);
  uint8_t b[BLOCK] = {};
  f.seekSet(0);
  f.write(b, BLOCK);
  f.sync();
  return f;
}

static void removeProject(int p) {                // (its handles closed first)
  char path[40];
  projectPath(path, sizeof path, p);
  SD.sdfs.remove(path);
}

static SceneList newList() {
  SceneList l;
  memset(&l, 0, sizeof l);
  memcpy(l.magic, "M8SL", 4);
  l.version = 1;
  l.tempo10 = tenths(newTempo ? newTempo() : 120.0f);   // a new project: the last tempo set by hand
  l.master = masterOf(MASTER_VOLUME_UNITY, 0);     // (and the master volume at 100 %, centred)
  return l;
}

// A file's list (false: an empty slot).
static bool readList(FsFile &f, SceneList &l) {
  memset(&l, 0, sizeof l);
  if (!f || !f.seekSet(0) || f.read(&l, sizeof l) != (int)sizeof l) return false;
  return !memcmp(l.magic, "M8SL", 4) && l.count <= LIST_SLOTS;   // (one with more than MAX_SCENES: as it is, none added)
}

// A project file holds a clip in one of its scenes (false: none, or an empty slot).
static bool fileHasClips(FsFile &f) {
  SceneList l;
  if (!readList(f, l)) return false;
  for (int i = 0; i < l.count; i++) {
    SceneInfo s;
    if (f.seekSet((uint64_t)(1 + l.slot[i]) * BLOCK) && f.read(&s, sizeof s) == (int)sizeof s &&
        !memcmp(s.magic, "M8SC", 4))
      for (const auto &u : s.units)
        if (u.sample) return true;
  }
  return false;
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
  if (!ok && job.to < PROJECTS) {                // a paste that failed: no half project
    removeProject(job.to);
    filled[job.to] = false;
  }
  if (ok && job.clearFrom && job.from < PROJECTS) removeProject(job.from);   // a cut: the source goes
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
  endCopy(ok);
}

FLASHMEM static bool startCopy(int from, int to, bool clearFrom) {
  if (job.on) return false;
  if (from == project) flushProject();
  job.src = openProject(from);
  job.dst = openProject(to);
  if (!job.dst && to < PROJECTS) job.dst = createProject(to);   // (an empty slot has no file yet)
  if (!readList(job.src, job.l) || !job.dst) {
    job.src.close();
    job.dst.close();
    if (to < PROJECTS) removeProject(to);        // (the empty file just made)
    return false;
  }
  if (clearFrom && from == project) file.close();   // (the loaded one cut: its file goes at the end)
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

void flushScenes() { flushProject(); }

int projectCurrent() { return project == DUMMY ? -1 : project; }
bool projectFilled(int p) { return p >= 0 && p < PROJECTS && filled[p]; }
bool projectClipboardFull() { return projectClip; }

void clearClipboards() {                           // the scene and project clipboards emptied (scenes.h)
  clipFull = false;
  if (projectClip && !job.on) {
    FsFile f = openProject(CLIP);
    SceneList l = newList();
    if (f) writeBlockTo(f, 0, &l, sizeof l);
    projectClip = false;
  }
}
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

// The looper on project p (its file open as `file`, its list in `list`): an
// empty slot gets a new project (no scenes: the empty scene, the tempo last
// set by hand), and so does DUMMY every time. Its tempo goes on the looper
// (one saved before projects kept a tempo: its current scene's, kept now);
// apply: its current scene too.
FLASHMEM static bool openAs(int p, bool apply_) {
  clipsKnown = -1;
  FsFile f = openProject(p);
  if (!f && p < PROJECTS) f = createProject(p);   // an empty slot: its file made now
  if (!f) return false;
  SceneList l;
  if (p == DUMMY || !readList(f, l)) {
    l = newList();
    if (!writeBlockTo(f, 0, &l, sizeof l)) return false;
  }
  l.current = min(l.current, (uint16_t)(l.count + 1));
  file = std::move(f);
  list = l;
  madeSlot = -1;
  project = p;
  setRecordingLetter(p < PROJECTS ? (char)('A' + p) : 0);   // its recordings are named after it
  if (p < PROJECTS) filled[p] = true;
  recordSlot = -1;
  listDirty = false;
  changed = false;
  SceneInfo s = sceneAt(min((int)list.current, (int)list.count));   // N+1*: a copy of N
  sceneTempo = list.current >= 1 && list.current <= list.count ? s.tempo : 0;
  resampleTempo = 0;
  float tempo = sceneTempo > 0 ? sceneTempo : list.tempo10 ? list.tempo10 / 10.0f : s.tempo > 0 ? s.tempo : getTempo();   // (its scene's)
  if (!list.tempo10) {
    list.tempo10 = tenths(tempo);
    listDirty = true;
  }
  // Its master volume and balance (one saved before projects kept them: the
  // one playing, the app's as it was, kept now).
  if ((list.master & MASTER_KEPT) != MASTER_KEPT) {
    list.master = masterOf(masterVolume(), masterBalance());
    listDirty = true;
  }
  setMasterVolume(list.master >> 6 & 31);
  setMasterBalance((list.master & 63) - 32);
  loading = true;                                 // (the scene's units go on as they are)
  if (apply_) apply(s, false, true, tempo);       // from 1.1.1
  else setTempo(tempo);
  loading = false;
  return true;
}


bool projectInfo(int p, int &scenes, float &tempo, int &current) {
  if (p < 0 || p >= PROJECTS) return false;
  if (p == project) flushProject();
  FsFile f = openProject(p);
  SceneList l;
  if (!readList(f, l)) return false;
  scenes = l.count;
  current = min((int)l.current, (int)l.count);    // (N+1*: a copy of N)
  tempo = l.tempo10 ? l.tempo10 / 10.0f : getTempo();   // (not kept yet: the tempo now)
  return true;
}

float projectSceneTempo(int p, int pos) {
  FsFile f = openProject(p);
  SceneList l;
  if (!readList(f, l) || pos < 1 || pos > l.count) return 0;
  SceneInfo s;
  if (!f.seekSet((uint64_t)(1 + l.slot[pos - 1]) * BLOCK) || f.read(&s, sizeof s) != (int)sizeof s || memcmp(s.magic, "M8SC", 4)) return 0;
  return s.tempo >= MIN_BPM && s.tempo <= MAX_BPM ? s.tempo : 0;
}

bool projectScene(int p, int pos, TrackUnit *units) {
  FsFile f = openProject(p);
  SceneList l;
  if (!readList(f, l) || pos < 1 || pos > l.count) return false;
  SceneInfo s;
  if (!f.seekSet((uint64_t)(1 + l.slot[pos - 1]) * BLOCK) || f.read(&s, sizeof s) != (int)sizeof s ||
      memcmp(s.magic, "M8SC", 4))
    return false;
  for (int t = 0; t < NUM_TRACKS; t++) {
    units[t] = s.units[t];
    upgradeUnit(units[t]);
  }
  return true;
}

void sceneRecorded(float bpm, bool resample) {
  if (syncSource() != SYNC_NONE) return;           // (an external tempo: the scene's own kept)
  if (resample) {                                  // (it moves to a scene of its own: that one's)
    resampleTempo = bpm;
    return;
  }
  sceneTempo = bpm;
  if (list.current >= 1 && list.current <= list.count) changed = true;   // (saved with the scene; 0*, N+1*: it's made a scene with it)
}

float sceneSavedTempo() {
  float b = list.current >= 1 && list.current <= list.count ? sceneTempo
          : list.current == list.count + 1 && list.count ? sceneAt(list.count).tempo : 0;   // (N+1*: the copy of N's)
  return b >= MIN_BPM && b <= MAX_BPM ? b : 0;
}

void projectMasterChanged() {
  const uint16_t m = masterOf(masterVolume(), masterBalance());
  if (m == list.master) return;
  list.master = m;
  listDirty = true;
  edits++;
}

void projectTempoChanged() {
  if (loading || syncSource() != SYNC_NONE) return;   // (an external tempo: not the scene's)
  if (list.current >= 1 && list.current <= list.count && sceneTempo != getTempo()) {   // (set by hand: the scene's tempo)
    sceneTempo = getTempo();
    changed = true;
  } else if ((list.current == 0 || list.current == list.count + 1) && !liveEmpty()) {   // (0*, N+1*: a change to it, made a scene with this tempo)
    sceneTempo = getTempo();
    changed = true;
    edits++;
  }
  if (tenths(getTempo()) == list.tempo10) return;
  list.tempo10 = tenths(getTempo());
  listDirty = true;
  edits++;
}

bool projectLoad(int p) {
  if (p < 0 || p >= PROJECTS || busy() || projectCopying(p)) return false;
  if (p == project) return true;
  transportForgetPosition();                       // (SHIFT+START: from 1.1.1 in it)
  target = -1;
  debugStage = "project load: closing prepared files";
  prepareUnits(nullptr);
  debugStage = "project load: saving the old one";
  flushProject();
  debugStage = "project load: opening";
  bool ok = openAs(p, true);
  debugStage = "project load: done";
  return ok;
}

bool projectHasClipsIn(int p) {
  if (p == project) return projectHasClips();
  if (!projectFilled(p)) return false;
  FsFile f = openProject(p);
  return fileHasClips(f);
}

FLASHMEM bool projectCut(int p) {
  if (!projectsMayCopy() || !projectFilled(p)) return false;
  bool clips = p == project;
  if (clips) clips = projectHasClips();
  else {
    FsFile f = openProject(p);
    clips = fileHasClips(f);
  }
  if (!clips) return projectDelete(p);             // no clips: emptied, not to the clipboard (it keeps what it had)
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
  if (p == project) {
    flushProject();
    file.close();
  }
  removeProject(p);
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

// Marks every recording a project uses (scenes.h deleteUnusedSamples()).
static void markUsedSamples() {
  flushProject();
  prepareUnits(nullptr);                           // (no prepared sample files open)
  clearSampleMarks();
  auto mark = [](const TrackUnit &u) { markSample(u.sample); };
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
}

FLASHMEM int deleteEmptySamples() {
  if (transport.running || busy() || job.on) return 0;
  uint32_t ids[16];
  int n = takeEmptySamples(ids, 16);
  if (!n) return 0;
  markUsedSamples();
  int deleted = deleteUnmarkedSamples(ids, n);
  return deleted;
}

int deleteUnusedSamples(bool silentToo) {
  if (transport.running || busy() || job.on) return 0;
  // The silent recordings first: off every scene of every project (their
  // tracks emptied there) and off the tracks now, so they go with the unused.
  flushProject();
  static uint32_t silent[256];
  const int ns = silentToo ? silentSamples(silent, 256) : 0;
  auto isSilent = [&](uint32_t id) { for (int i = 0; i < ns; i++) if (id && silent[i] == id) return true; return false; };
  if (ns) {
    for (int t = 0; t < NUM_TRACKS; t++) if (isSilent(tracks[t].sample)) clearTrack(t);
    flushProject();                                // (the current scene: written with them gone)
    for (int p = 0; p <= DUMMY; p++) {
      FsFile f = openProject(p);
      if (!f) continue;
      for (uint32_t b = 1; b <= LIST_SLOTS; b++) {
        SceneInfo s;
        if (!f.seekSet((uint64_t)b * BLOCK) || f.read(&s, sizeof s) != (int)sizeof s || memcmp(s.magic, "M8SC", 4)) continue;
        bool hit = false;
        for (int t = 0; t < NUM_TRACKS; t++)
          if (isSilent(s.units[t].sample)) { s.units[t] = emptyScene().units[t]; hit = true; }
        if (hit) { f.seekSet((uint64_t)b * BLOCK); f.write(&s, sizeof s); }
      }
      f.close();
    }
  }
  markUsedSamples();
  return deleteUnmarkedSamples();
}

// The project files: those of an older card moved into the folders (1.dat ...
// 12.dat of /L7/projects become A.L7P ... L.L7P; the first version's
// /L7/scenes.dat project A), the clipboard's and the dummy project's made at
// full size if missing, and a slot's file with no project in it removed.
FLASHMEM static void makeProjectFiles() {
  char from[40], to[40];
  projectPath(to, sizeof to, 0);
  moveFile("/L7/scenes.dat", to);
  for (int p = 0; p < PROJECTS; p++) {
    snprintf(from, sizeof from, "/L7/PROJECTS/%d.dat", p + 1);
    projectPath(to, sizeof to, p);
    moveFile(from, to);
  }
  moveFile("/L7/PROJECTS/clip.dat", "/L7/DATA/CLIP.DAT");
  moveFile("/L7/PROJECTS/none.dat", "/L7/DATA/NONE.DAT");
  for (int p = 0; p <= DUMMY; p++) {
    FsFile f = openProject(p);
    if (p >= PROJECTS && (!f || f.fileSize() < FILE_BYTES)) {   // first time: made at full size, empty
      f.close();
      f = createProject(p);
    }
    SceneList l;
    bool full = readList(f, l);
    bool clips = p == CLIP && fileHasClips(f);
    f.close();
    if (p < PROJECTS) {
      filled[p] = full;
      if (!full) removeProject(p);                 // (an empty one of an older card, or a cut short)
    } else if (p == CLIP) {
      projectClip = clips;                         // (one with no clips, cut before they were kept out: none)
    }
  }
}

// Drops the loaded project and everything read from the project files, without
// touching the card (see storageForget()); scenesBegin() then loads it again.
void scenesForget() {
  file = FsFile();
  job.on = false;
  job.src = FsFile();
  job.dst = FsFile();
  recordSlot = -1;
  listDirty = false;
  changed = false;
  loading = false;
  holding = false;
  target = -1;
  clipFull = false;
  projectClip = false;
  clipsKnown = -1;
  project = 0;
  for (bool &f : filled) f = false;
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
