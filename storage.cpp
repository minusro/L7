// storage.cpp — see storage.h
//
// Files, in /L7 (an older /looper is renamed to it at boot), in four folders:
//   SAMPLES/A30001.wav ... every recording: the letter of the project loaded
//                         when it was recorded, the number of the track it
//                         was recorded on (1-7, as on the screen), then its
//                         number (0001-9999), counting per project and track:
//                         A13333, A23333 and B43333 can all be there (older
//                         ones: just a number, 0001.wav ...). Kept when
//                         tracks stop using it. 16-bit stereo WAV whose
//                         512-byte header carries, in its JUNK chunk, the
//                         loop's length and tempo, its waveform overview and
//                         its name (letter, track, number).
//   PROJECTS/A.L7P ...    the 12 projects, A-L (scenes.cpp); an empty slot has no file.
//   EXPORTS/              the Ableton projects (ableton.cpp).
//   DATA/                 the rest: recording space (the pool: NNNN.DAT, a
//                         recording's file until it's named and moved to SAMPLES:
//                         a recording gets its name when it starts (scenes
//                         refer to it by it), and its file is renamed, into
//                         SAMPLES, once cut down to size (stopped)),
//                         TRACK1.DAT ... each track: the sample it plays and
//                         its settings (loop range, mute, filter, volume and
//                         balance), 512 bytes; SESSION.DAT the tempo, the loaded
//                         project, the master volume, the menu open; CLIP.DAT and
//                         NONE.DAT (scenes.cpp).
// (Older cards: /L7/samples, /projects, /Ableton and the files of /L7 itself
// are moved into these at boot.) Older versions kept each track in a
// full-size track1.wav ... : at boot those become samples (and the empty ones
// recording space).
//
// Rule: nothing that changes the card's file system (create, resize, delete,
// rename) happens while the transport runs. On a slow card those take
// hundreds of ms to seconds and starve the track streams. So:
//   * recordings go into sample files made ahead of time at full size (a pool
//     of POOL_SIZE, refilled while stopped); a finished one is cut down to its
//     length the next time the transport stops. Only if the pool runs out
//     while playing is a file made then.
//   * settings are saved by overwriting a unit file's 512 bytes (and a
//     recording's details its sample's header), when the buffers are full
//   * tracks joining mid-play fill their buffer through the normal streaming
//     instead of a blocking burst of reads
#include "storage.h"
#include <utility>
#include <inttypes.h>

namespace looper {

volatile const char *debugStage = "boot";
static uint32_t chunk[CHUNK_FRAMES];
static uint8_t headerDirty = 0;                  // bit t: track t's unit file needs saving
// A write is waiting for the playback buffers to be topped up (see serviceStreams).
static bool writePending = false;
static constexpr uint64_t SAMPLE_FILE_BYTES = WAV_DATA_OFFSET + (uint64_t)MAX_REC_FRAMES * 4;
static constexpr int POOL_SIZE = 16;             // empty sample files kept ready for recordings (~360 MB):
                                                 // making one takes ~0.7 s, so never while playing if it can be helped
static constexpr uint32_t MAX_NUMBER = 9999;     // sample numbers: 0001-9999
static constexpr int MAX_SAMPLES = 5000;          // recordings kept track of (sampleIds: 20 KB of RAM1)
// A sample's id: its name tag (the project's letter and the track's number:
// (letter - 'A' + 1) << 3 | track) and its number, tag << 14 | number:
// "A30012.wav". Numbers count per tag (A13333, A23333 and B43333 can all be
// there). Without a tag, just the number: recordings from before ("0012.wav")
// and recording space. Ids sort as the names do.
static constexpr int NUMBER_BITS = 14;
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint8_t tagOf(char letter, int track) { return letter >= 'A' && letter <= 'O' && track >= 1 && track <= 7 ? (uint8_t)((letter - 'A' + 1) << 3 | track) : 0; }
static char tagLetter(uint8_t tag) { return tag ? (char)('A' + (tag >> 3) - 1) : 0; }
static int tagTrack(uint8_t tag) { return tag & 7; }
static uint32_t idOf(uint8_t tag, uint32_t number) { return (uint32_t)tag << NUMBER_BITS | number; }
static uint8_t idTag(uint32_t id) { return (uint8_t)(id >> NUMBER_BITS); }
static uint32_t idNumber(uint32_t id) { return id & ((1u << NUMBER_BITS) - 1); }
static uint16_t nextNumber[128];                 // each tag's next number (0: 1)
static int screenTrack(int t) { return NUM_TRACKS - t; }   // a track's number on the screen (1: the bottom one)
static char recordLetter = 0;                    // the loaded project's: recordings made now get it
void setRecordingLetter(char letter) { recordLetter = letter; }

// Recordings not named yet: still in their recording-space file ("0016.wav"),
// renamed once cut down to size (stopped).
struct Unnamed { uint32_t id; uint16_t file; };
static Unnamed unnamed[64];
static int unnamedCount = 0;
static int unnamedIndex(uint32_t id) {
  for (int i = 0; i < unnamedCount; i++) if (unnamed[i].id == id) return i;
  return -1;
}
// Recordings named by the version before this one: a tag in the name, but
// numbered with all the others, their id the number alone (as projects refer
// to them).
struct Older { uint16_t number; uint8_t tag; };
static Older older[128];
static int olderCount = 0;
static uint8_t olderTag(uint32_t n) {
  for (int i = 0; i < olderCount; i++) if (older[i].number == n) return older[i].tag;
  return 0;
}

// Sample id's name ("A30012", "0012"), as it ends up (once named).
static void sampleName(uint32_t id, char *buf, size_t size) {
  uint8_t tag = idTag(id);
  uint32_t n = idNumber(id);
  if (!tag) tag = olderTag(n);
  if (tag) snprintf(buf, size, "%c%d%04u", tagLetter(tag), tagTrack(tag), (unsigned)n);
  else snprintf(buf, size, "%04u", (unsigned)n);
}
// Its file: where it is now (named: as it ends up).
static void samplePath(uint32_t id, char *buf, size_t size, bool named = false) {
  int u = named ? -1 : unnamedIndex(id);
  char name[16];
  if (u >= 0) {                                  // a recording not named yet: in its recording space
    snprintf(buf, size, "/L7/DATA/%04u.DAT", unnamed[u].file);
    return;
  }
  sampleName(id, name, sizeof name);
  snprintf(buf, size, "/L7/SAMPLES/%s.wav", name);
}
void sampleFile(uint32_t id, char *path, size_t size) { samplePath(id, path, size); }
// Recording space number n's file (empty, or holding the recording not named yet).
static void poolPath(uint32_t n, char *buf, size_t size) { snprintf(buf, size, "/L7/DATA/%04u.DAT", (unsigned)n); }
static void unitPath(int t, char *buf, size_t size) { snprintf(buf, size, "/L7/DATA/TRACK%d.DAT", t + 1); }
static void legacyPath(int t, char *buf, size_t size) { snprintf(buf, size, "/L7/track%d.wav", t + 1); }

static int32_t posMod(int32_t a, int32_t m) { int32_t r = a % m; return r < 0 ? r + m : r; }

// Frames in q quarter beats (a beat's samples rarely divide by 4: rounded down
// within each beat).
static uint32_t quarterFrames(uint32_t q, uint32_t spb) { return (q / 4) * spb + (q % 4) * spb / 4; }

static bool streamsTopped();
static void startTrack(int t, int32_t beat0, bool fillNow);
static void startRecording(int t, uint32_t from, int32_t beat, int32_t maxBehind, bool resample = false);

// A track to record next: pressed while another one was recording. Its
// sample file is opened right away (no SD work as it starts); it starts where
// that one ends, with no gap between them.
static int queuedTrack = -1;
static bool queuedResample = false;              // it resamples (records the output)
static int finishedResample = -1;                // a resampling just saved: its track (takeResampled())

// A clip change on a playing track (a scene change, or another sample):
// planned for transport quarter beat q, stream frame switchS. The old clip
// plays up to it; the new one, its file already open, carries on from there
// with no gap. Applied once the stream reaches switchS (see applyPlan).
struct Plan {
  bool on = false;
  bool restart = false;                          // the new clip from its 1.1.1 position at q (a project switch)
  bool stop = false;                             // the track goes empty there (nothing new to read)
  uint32_t outBeats = 0;                         // the range playing before the switch: its length
  int32_t outAnchorQ = 0;                        // and anchor (a new range as long carries on from it)
  int32_t q = 0;                                 // kept after applying: the interrupt may not be there yet
  uint32_t switchS = 0;
  TrackUnit u;
  FsFile file;                                   // the new sample (none: the same one)
  uint32_t spb = 0, loopBars = 0, frames = 0, preroll = 0;
  uint8_t overview[OVERVIEW_COLS];
  // The old clip's frames just before the switch, as they were before the
  // crossfade into the new one (a replanned switch crossfades again from them).
  uint32_t before[XFADE_FRAMES];
  bool beforeSaved = false;
};
DMAMEM static Plan plans[NUM_TRACKS];             // (RAM2: only loop() uses them, never DMA)
static uint32_t prerollFrames[NUM_TRACKS];        // each track's sample: its stored pre-roll (see SampleInfo)
static void applyPlan(int t);
static void dropPlan(int t);
static void crossfadeInto(Track &tr, uint32_t at, uint32_t entry, uint32_t *saved, bool &haveSaved);

// A change of a track's unit while playing (a scene change, or another
// sample), due on transport quarter beat q: the clip switches there (a Plan),
// then its settings (mute, filter, volume) follow, or it goes empty.
struct Change {
  bool on = false;
  bool planned = false;                          // its clip switch is set up (or none is needed)
  bool scene = false;                            // part of a scene change
  bool restart = false;                          // ... that starts over from 1.1.1 at its switch point (a project)
  int32_t grid = 0;                              // its switch points (beats; 0: quarter beats)
  int32_t q = 0;
  TrackUnit u;
};
static Change changes[NUM_TRACKS];

// Takes tracks (bit t of mask) off, playing ones after the interrupt has
// faded them out (a few ms, waited for here): no click.
static void silenceTracks(uint8_t mask) {
  bool fading = false;
  for (int t = 0; t < NUM_TRACKS; t++)
    if ((mask >> t & 1) && transport.running && tracks[t].live) {
      tracks[t].stopping = true;
      fading = true;
    }
  for (uint32_t t0 = micros(); fading && micros() - t0 < 30000;) {
    delayMicroseconds(250);
    fading = false;
    for (const auto &tr : tracks) if (tr.stopping && tr.live) fading = true;
  }
  for (int t = 0; t < NUM_TRACKS; t++)
    if (mask >> t & 1) {
      tracks[t].live = false;
      tracks[t].armed = false;
      tracks[t].stopping = false;
    }
}

// Recording, waiting to, or queued: its sample is spoken for.
static bool recordingTrack(int t) {
  return (recording.mode != Recording::Idle && recording.track == t) || queuedTrack == t;
}

// ---- Samples
// Header: RIFF/WAVE/fmt, then a JUNK chunk padding it to 512 bytes so the
// audio is sector-aligned, with SampleInfo at offset 44 and the overview after it.
struct SampleInfo {
  char magic[4];                                 // "M8SM"
  uint32_t version, frames, spb, loopBars;       // frames 0: empty (recording space)
  uint32_t order;                                // when it was recorded: 1, 2, 3 ... (version 2)
  uint32_t preroll;                              // frames of the input before the loop's start (version 3)
};
// Version 2: order. Version 1 samples are ordered by their number.
// Version 3: preroll. The input just before the recording started is kept
// after the loop's frames (frames .. frames + preroll), for crossfading the
// loop's end into its start.
static constexpr uint32_t INFO_OFFSET = 44, SAMPLE_VERSION = 3;
static uint32_t prerollOf(const SampleInfo &info) { return info.version >= 3 ? info.preroll : 0; }
static constexpr uint32_t SAMPLE_OVERVIEW_OFFSET = INFO_OFFSET + sizeof(SampleInfo);
// After the overview: its name: the letter of the project it was recorded in,
// the number of the track, its number (16 bits; zeros: none, as in older
// samples; a letter and track but no number: named by the version before).
static constexpr uint32_t SAMPLE_TAG_OFFSET = SAMPLE_OVERVIEW_OFFSET + OVERVIEW_COLS;
static_assert(SAMPLE_TAG_OFFSET + 4 <= WAV_DATA_OFFSET - 8, "the overview and name must fit in the header");

// The samples holding recordings: their ids, sorted (the order of their
// names). In RAM1 (20 KB; RAM2 is left to the rings, and to the MTP buffers
// of storage mode).
static uint32_t sampleIds[MAX_SAMPLES];
static int sampleTotal = 0;
static uint32_t lastOrder = 0;                   // the latest recording's order (kept in its header)
static uint32_t nextSample = 1;                  // number for the next new file without a tag (recording space)
static uint16_t pool[POOL_SIZE];                 // empty sample files, lowest number first
static int poolCount = 0;
// pool[0] opened ahead (while stopped), so starting a recording while playing
// costs no SD access.
static FsFile poolNext;
static uint16_t poolNextSample = 0;
static void openPoolNext();
static bool takeRecordingSample(int t);
static uint32_t untrimmed[64];                   // finished samples still at full size (or not named yet)
static int untrimmedCount = 0;

// Where id is in sampleIds, or would go.
static int sampleIndex(uint32_t id) {
  int lo = 0, hi = sampleTotal;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (sampleIds[mid] < id) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
static void noteSample(uint32_t id) {
  int i = sampleIndex(id);
  if (!id || (i < sampleTotal && sampleIds[i] == id) || sampleTotal >= MAX_SAMPLES) return;
  memmove(sampleIds + i + 1, sampleIds + i, (sampleTotal - i) * sizeof sampleIds[0]);
  sampleIds[i] = id;
  sampleTotal++;
}
uint32_t sampleCount() { return sampleTotal; }
uint32_t sampleAt(int i) { return i >= 0 && i < sampleTotal ? sampleIds[i] : 0; }

static bool openSample(uint32_t n, FsFile &f, SampleInfo &info, uint8_t *overview);

bool sampleDetails(uint32_t n, SampleDetails &d) {
  FsFile f;
  SampleInfo info;
  if (!sampleExists(n) || !openSample(n, f, info, nullptr)) return false;
  d.frames = info.frames;
  d.spb = info.spb;
  d.loopBars = info.loopBars;
  d.bytes = f.fileSize();
  sampleName(n, d.name, sizeof d.name);
  return true;
}

bool copySample(uint32_t n, const char *to, void (*progress)()) {
  if (transport.running) return false;
  char path[40];
  samplePath(n, path, sizeof path);
  FsFile from = SD.sdfs.open(path, O_RDONLY);
  FsFile dst = SD.sdfs.open(to, O_RDWR | O_CREAT | O_TRUNC);
  if (!from || !dst) return false;
  uint64_t left = from.fileSize();
  dst.preAllocate(left);
  debugStage = "SD copy (export)";
  bool ok = from.seekSet(0);
  while (ok && left) {
    size_t n4 = left < sizeof chunk ? (size_t)left : sizeof chunk;
    ok = from.read(chunk, n4) == (int)n4 && dst.write(chunk, n4) == n4;
    left -= n4;
    if (progress) progress();
  }
  ok = dst.sync() && ok;
  dst.close();
  if (!ok) SD.sdfs.remove(to);
  return ok;
}
bool sampleExists(uint32_t id) {
  int i = sampleIndex(id);
  return id && i < sampleTotal && sampleIds[i] == id;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static bool writeSampleHeader(FsFile &f, uint32_t frames, uint32_t spb, uint32_t loopBars, const uint8_t *overview,
                              uint32_t order = 0, uint32_t preroll = 0, uint32_t id = 0) {
  uint8_t h[WAV_DATA_OFFSET] = {};
  memcpy(h, "RIFF", 4);       put32(h + 4, WAV_DATA_OFFSET - 8 + frames * 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);  put32(h + 16, 16);
  put16(h + 20, 1);           put16(h + 22, 2);           // PCM, stereo
  put32(h + 24, 44100);       put32(h + 28, 44100 * 4);
  put16(h + 32, 4);           put16(h + 34, 16);
  memcpy(h + 36, "JUNK", 4);  put32(h + 40, WAV_DATA_OFFSET - 44 - 8);
  SampleInfo info = {{'M', '8', 'S', 'M'}, SAMPLE_VERSION, frames, spb, loopBars, order, preroll};
  memcpy(h + INFO_OFFSET, &info, sizeof info);
  if (overview) memcpy(h + SAMPLE_OVERVIEW_OFFSET, overview, OVERVIEW_COLS);
  if (uint8_t tag = idTag(id)) {
    h[SAMPLE_TAG_OFFSET] = tagLetter(tag);
    h[SAMPLE_TAG_OFFSET + 1] = tagTrack(tag);
    h[SAMPLE_TAG_OFFSET + 2] = idNumber(id) & 0xFF;
    h[SAMPLE_TAG_OFFSET + 3] = idNumber(id) >> 8;
  }
  memcpy(h + WAV_DATA_OFFSET - 8, "data", 4);
  put32(h + WAV_DATA_OFFSET - 4, frames * 4);
  debugStage = "SD write (sample header)";
  bool ok = f.seekSet(0) && f.write(h, sizeof h) == (int)sizeof h;
  // Also pushes out the end of a recording still held in SdFat's sector cache,
  // so it survives power-off (the file's size doesn't change: no FS update).
  return f.sync() && ok;
}

static bool readSampleHeader(FsFile &f, SampleInfo &info, uint8_t *overview, uint8_t *tag = nullptr,
                             uint32_t *number = nullptr) {
  uint8_t h[WAV_DATA_OFFSET];
  if (!f.seekSet(0) || f.read(h, sizeof h) != (int)sizeof h) return false;
  memcpy(&info, h + INFO_OFFSET, sizeof info);
  if (memcmp(h, "RIFF", 4) || memcmp(info.magic, "M8SM", 4) || info.version < 1) return false;
  if (overview) memcpy(overview, h + SAMPLE_OVERVIEW_OFFSET, OVERVIEW_COLS);
  if (tag) *tag = tagOf((char)h[SAMPLE_TAG_OFFSET], h[SAMPLE_TAG_OFFSET + 1]);
  if (number) *number = h[SAMPLE_TAG_OFFSET + 2] | h[SAMPLE_TAG_OFFSET + 3] << 8;
  return true;
}

// A sample that can play: a recording of a sane length and tempo.
static bool playable(const SampleInfo &info) {
  return info.frames && info.frames <= MAX_REC_FRAMES && info.spb && info.loopBars && info.loopBars <= MAX_BARS;
}

// A new empty sample file at full size, for the pool (changes the file system).
// A pool file couldn't be made (the card full): not tried again while
// stopped until trimming a recording frees some space (a try takes ~1 s).
static bool poolBlocked = false;

static bool createPoolFile() {
  if (poolCount >= POOL_SIZE || nextSample > MAX_NUMBER) return false;
  char path[40];
  poolPath(nextSample, path, sizeof path);
  if (transport.running) Serial.printf("no recording space left: creating %s while playing\n", path);
  debugStage = "SD create (sample file)";
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!f) return false;
  f.preAllocate(SAMPLE_FILE_BYTES);
  uint8_t zero = 0;                              // give the file its full size
  bool ok = f.seekSet(SAMPLE_FILE_BYTES - 1) && f.write(&zero, 1) == 1;
  ok = writeSampleHeader(f, 0, 0, 0, nullptr) && ok;
  f.close();
  if (!ok) {                                     // probably no space: don't keep half a file
    SD.sdfs.remove(path);
    poolBlocked = true;
    Serial.printf("no space for more recording files (%d ready; %lu MB free, one needs %lu MB)\n", poolCount,
                  (unsigned long)((uint64_t)SD.sdfs.freeClusterCount() * SD.sdfs.bytesPerCluster() >> 20),
                  (unsigned long)(SAMPLE_FILE_BYTES >> 20));
    return false;
  }
  pool[poolCount++] = nextSample++;
  return true;
}

static void reopenSample(uint32_t n);

// Cuts a finished sample down to its recording, and names it after the
// project and track it was recorded in (changes the file system).
static void trimSample(uint32_t id) {
  char path[40];
  samplePath(id, path, sizeof path);
  FsFile f = SD.sdfs.open(path, O_RDWR);
  SampleInfo info;
  if (f && readSampleHeader(f, info, nullptr) && playable(info)) {
    debugStage = "SD trim (sample file)";
    f.truncate(WAV_DATA_OFFSET + (uint64_t)(info.frames + prerollOf(info)) * 4);
    poolBlocked = false;                         // (space freed)
  }
  f.close();
  int u = unnamedIndex(id);
  if (u >= 0) {                                  // "0016.wav" -> "A30012.wav"
    char to[40];
    samplePath(id, to, sizeof to, true);
    debugStage = "SD rename (sample file)";
    if (SD.sdfs.rename(path, to)) {
      unnamed[u] = unnamed[--unnamedCount];
      reopenSample(id);
    }
  }
}

// ---- Tracks and their unit files
static constexpr uint32_t UNIT_VERSION = 3;   // 3: volume and balance in today's steps (2: 6 a side, 6 dB)

void upgradeUnit(TrackUnit &u) {
  if (u.version >= UNIT_VERSION) return;
  // The old steps: version 1 volume 0-32 (unity 28, 1.5 dB), balance +-16;
  // version 2 volume 0-6 (unity 5, 6 dB), balance +-6.
  bool v1 = u.version < 2;
  float db = (u.volume - (v1 ? 28 : 5)) * (v1 ? 1.5f : 6.0f);
  if (u.volume > 0)                                // (a quiet one stays audible: at least step 1)
    u.volume = (int8_t)constrain((int)lroundf(VOLUME_UNITY + db / VOLUME_STEP_DB), 1, VOLUME_MAX);
  u.balance = (int8_t)lroundf(u.balance * (float)BALANCE_MAX / (v1 ? 16 : 6));
  u.version = UNIT_VERSION;
}

static TrackUnit clipboard;                       // sample 0: empty
static bool applyingUnit = false;                // setTrackUnit() at work: not a change of the scene

TrackUnit unitOf(int t) {
  const Track &tr = tracks[t];
  if (changes[t].on) return changes[t].u;        // a change on its way counts
  TrackUnit u = {{'M', '8', 'T', 'U'}, UNIT_VERSION, 0, 0, 0, 0, 0, tr.filterLo, tr.filterHi, tr.volume, tr.balance};
  if (tr.state == TrackState::Empty) return u;
  u.sample = tr.sample;
  trackRange(t, u.rangeStart, u.rangeLen);       // a queued range counts
  u.rangePhase = (uint32_t)posMod(-tr.rangeAnchorQ, 4 * u.rangeLen);
  u.muted = tr.state == TrackState::Muted;
  return u;
}

// Opens sample n, reading its header (and overview); false if it can't play.
static bool openSample(uint32_t n, FsFile &f, SampleInfo &info, uint8_t *overview) {
  char path[40];
  samplePath(n, path, sizeof path);
  debugStage = "SD open (sample file)";
  f = SD.sdfs.open(path, O_RDWR);
  if (!f || !readSampleHeader(f, info, overview) || !playable(info)) {
    f.close();
    return false;
  }
  return true;
}

// ---- Preparing a scene ahead (see storage.h): each track's sample file,
// opened in the background, one per loop() pass.
struct Prepared {
  uint32_t want = 0;                               // the sample to open; 0: none
  uint32_t sample = 0;                             // the one `file` holds; 0: none
  FsFile file;
  SampleInfo info;
  uint8_t overview[OVERVIEW_COLS];
};
DMAMEM static Prepared prepared[NUM_TRACKS];       // (RAM2: only loop() uses them, never DMA)
static bool preparing = false;

// Sample n was renamed (stopped): the handles open on it, opened again by its
// new name, each where it was.
static void reopenSample(uint32_t n) {
  char path[40];
  samplePath(n, path, sizeof path);
  auto reopen = [&](FsFile &f) {
    if (!f) return;
    uint64_t at = f.curPosition();
    f.close();
    f = SD.sdfs.open(path, O_RDWR);
    if (f) f.seekSet(at);
  };
  for (int t = 0; t < NUM_TRACKS; t++) {
    if (tracks[t].sample == n) reopen(tracks[t].file);
    if (plans[t].on && plans[t].u.sample == n) reopen(plans[t].file);
    if (prepared[t].sample == n) reopen(prepared[t].file);
  }
}

void prepareUnits(const TrackUnit *units) {
  preparing = units != nullptr;
  for (int t = 0; t < NUM_TRACKS; t++) {
    Prepared &p = prepared[t];
    p.want = units && units[t].sample != unitOf(t).sample ? units[t].sample : 0;   // (none for the same sample)
    if (p.sample != p.want) {
      p.file.close();
      p.sample = 0;
    }
  }
}

bool unitsPrepared() {
  if (!preparing) return false;
  for (const auto &p : prepared) if (p.want && p.sample != p.want) return false;
  return true;
}

// One file a pass, when the streams can afford it.
static void servicePrepare() {
  if (!preparing) return;
  for (auto &p : prepared) {
    if (!p.want || p.sample == p.want) continue;
    if (!storageMayWrite()) return;                // (the streams topped up first, as for a write)
    if (openSample(p.want, p.file, p.info, p.overview)) p.sample = p.want;
    else p.want = 0;                               // can't be read: the switch finds out too
    writePending = false;
    return;
  }
}

// Track t's prepared file for sample n, if there is one: moved to f.
static bool takePrepared(int t, uint32_t n, FsFile &f, SampleInfo &info, uint8_t *overview) {
  Prepared &p = prepared[t];
  if (!n || p.sample != n || !p.file) return false;
  f = std::move(p.file);                           // (SdFat copies: the copy left behind is let go)
  p.file.close();
  info = p.info;
  memcpy(overview, p.overview, OVERVIEW_COLS);
  p.sample = p.want = 0;
  return true;
}

// Opens sample n for track t and takes on its recording (length, tempo,
// overview). The track's settings are left to the caller.
static bool loadSample(int t, uint32_t n) {
  Track &tr = tracks[t];
  SampleInfo info;
  if (!takePrepared(t, n, tr.file, info, tr.overview) && !openSample(n, tr.file, info, tr.overview)) return false;
  tr.sample = n;
  prerollFrames[t] = prerollOf(info);
  tr.spbNative = info.spb;
  tr.loopBars = info.loopBars;
  tr.recordedFrames = info.frames;
  tr.overviewVersion++;
  return true;
}

// Takes on a unit's loop range (its sample already loaded).
static void applyRange(int t, const TrackUnit &u) {
  Track &tr = tracks[t];
  uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
  uint32_t len = u.rangeLen && u.rangeLen <= loopBeats ? u.rangeLen : loopBeats;
  tr.rangePending = false;
  tr.rangeStartBeat = u.rangeStart < loopBeats ? u.rangeStart : 0;
  tr.rangeBeats = len;
  tr.rangeAnchorQ = -(int32_t)(u.rangePhase % (4 * len));
}

// The range placed as starting from 1.1.1 at quarter beat q (a bar line):
// its start position (startPosition()) plays there.
static void anchorAsStart(int t, int32_t q) {
  Track &tr = tracks[t];
  uint32_t sp = startPosition(tr.loopBars * BEATS_PER_BAR, tr.rangeStartBeat, tr.rangeBeats);
  tr.rangeAnchorQ = q - 4 * (int32_t)posMod((int32_t)sp - (int32_t)tr.rangeStartBeat, tr.loopBars * BEATS_PER_BAR);
}

// ... and its settings.
static void applySettings(int t, const TrackUnit &u) {
  setTrackFilter(t, u.filterLo, u.filterHi);
  setTrackMix(t, u.volume, u.balance);
  tracks[t].state = u.muted ? TrackState::Muted : TrackState::Playing;
}

static void applyUnit(int t, const TrackUnit &u) {
  applyRange(t, u);
  applySettings(t, u);
}

static bool writeUnit(int t) {
  TrackUnit u = unitOf(t);
  uint8_t h[WAV_DATA_OFFSET] = {};
  memcpy(h, &u, sizeof u);
  debugStage = "SD write (track unit)";
  Track &tr = tracks[t];
  bool ok = tr.unitFile.seekSet(0) && tr.unitFile.write(h, sizeof h) == (int)sizeof h;
  return tr.unitFile.sync() && ok;
}

static void readUnit(int t) {
  Track &tr = tracks[t];
  tr.state = TrackState::Empty;
  tr.sample = 0;
  uint8_t h[WAV_DATA_OFFSET];
  TrackUnit u;
  if (!tr.unitFile.seekSet(0) || tr.unitFile.read(h, sizeof h) != (int)sizeof h) return;
  memcpy(&u, h, sizeof u);
  if (memcmp(u.magic, "M8TU", 4) || u.version < 1) return;
  upgradeUnit(u);
  setTrackFilter(t, u.filterLo, u.filterHi);
  setTrackMix(t, u.volume, u.balance);
  if (u.sample && loadSample(t, u.sample)) applyUnit(t, u);
}

void trackChanged(int t) {
  headerDirty |= 1 << t;
  if (applyingUnit) return;                      // (a scene being loaded)
  if (changes[t].on) {                           // edited before a change arrives: it keeps the edit
    const Track &tr = tracks[t];
    TrackUnit &u = changes[t].u;
    u.filterLo = tr.filterLo;
    u.filterHi = tr.filterHi;
    u.volume = tr.volume;
    u.balance = tr.balance;
  }
  sceneStateChanged();                           // the scene follows
}

void setTrackMuted(int t, bool muted) {
  Track &tr = tracks[t];
  if (tr.state != TrackState::Empty) tr.state = muted ? TrackState::Muted : TrackState::Playing;
  if (changes[t].on) changes[t].u.muted = muted; // a change on its way: comes muted (or not) too
  trackChanged(t);
}

bool trackMuted(int t) {
  return changes[t].on ? changes[t].u.muted : tracks[t].state == TrackState::Muted;
}

bool storageMayWrite() {
  if (!transport.running || streamsTopped()) return true;
  writePending = true;
  return false;
}

// Lead time for a change while playing: it goes on the first quarter beat
// (scene changes: bar line) at least this far ahead, leaving time to open the
// new samples and read their starts. A planned switch can still be changed
// up to PLAN_LATE_MS before it.
static constexpr uint32_t SWITCH_LEAD_MS = 150, PLAN_LATE_MS = 50;
static constexpr uint32_t PREPARED_LEAD_MS = 60;     // a scene prepared ahead: only the samples' starts to read
static constexpr int64_t msToSamples(uint32_t ms) { return (int64_t)ms * 441 / 10; }

// Transport position in samples (at the current tempo), and the time of quarter beat q.
static int64_t nowSamples(int64_t &spb) {
  noInterrupts();
  spb = transport.spb;
  int64_t now = (int64_t)transport.beat * spb + transport.sampleInBeat;
  interrupts();
  return now;
}
static int64_t quarterTime(int32_t q, int64_t spb) { return (int64_t)(q / 4) * spb + (q % 4) * spb / 4; }

// The first quarter beat (grid > 0: start of a beat that's a multiple of
// gridBeats) at least leadMs ahead.
static int32_t quarterAfterLead(int32_t gridBeats, uint32_t leadMs = SWITCH_LEAD_MS) {
  int64_t spb, now = nowSamples(spb) + msToSamples(leadMs);
  if (!gridBeats) return (int32_t)(now * 4 / spb) + 1;
  int64_t grid = gridBeats * spb;
  return (int32_t)((now + grid - 1) / grid) * 4 * gridBeats;
}

// The first beat at least SWITCH_LEAD_MS ahead (a track starting from silence).
static int32_t beatAfterLead() { return (quarterAfterLead(0) + 3) / 4; }

static bool sameClip(const TrackUnit &a, const TrackUnit &b) {
  return a.sample == b.sample && (!a.sample || (a.rangeStart == b.rangeStart && a.rangeLen == b.rangeLen &&
                                                a.rangePhase == b.rangePhase));
}

// The unit as the track has it (no change on its way).
static TrackUnit trackUnit(int t) {
  bool on = changes[t].on;
  changes[t].on = false;
  TrackUnit u = unitOf(t);
  changes[t].on = on;
  return u;
}

// What the stream heads for: the clip of a plan not reached yet, or the track's.
static TrackUnit streamUnit(int t) {
  const Plan &p = plans[t];
  if (!p.on) return trackUnit(t);
  TrackUnit u = p.u;
  if (p.stop) u.sample = 0;
  return u;
}

// Plans a gapless switch of playing track t to unit u's clip (none: the
// track stops) on quarter beat q (see Plan).
enum PlanResult { PLANNED, WAIT, FAILED };
static PlanResult planSwitch(int t, const TrackUnit &u, int32_t q, bool restart) {
  Track &tr = tracks[t];
  Plan &p = plans[t];
  p.restart = restart;
  noInterrupts();
  uint32_t tail = tr.tail;
  bool pending = p.on || tr.sampleSwitch;
  interrupts();
  uint32_t switchS;
  if (pending && p.q == q) {
    switchS = p.switchS;                         // replanned: the same switch point
  } else if (pending && q > p.q) {
    return WAIT;                                 // that switch first
  } else {
    // A new switch, or one moved earlier: the interrupt is still on the old
    // clip's mapping there (an earlier switch hasn't been reached).
    if (q < 4 * tr.startBeat) return FAILED;     // (only just started)
    switchS = tr.s0 + quarterFrames(q - 4 * tr.startBeat, tr.playSpb);
    if (!pending) {                              // (moved: still the clip playing before)
      p.outBeats = tr.rangeBeats;
      p.outAnchorQ = tr.rangeAnchorQ;
    }
    p.beforeSaved = false;                       // a new switch point
  }
  int64_t late = msToSamples(PLAN_LATE_MS) * tr.playSpb / transport.spb;   // in stream frames
  if ((int32_t)(switchS - tail) < late) return pending ? WAIT : FAILED;
  // The new sample, opened now (none needed for the one the stream has).
  bool stop = !u.sample;
  if (stop || u.sample == tr.sample) {
    p.file.close();
  } else if (!(p.on && p.file && p.u.sample == u.sample)) {
    FsFile f;
    SampleInfo info;
    if (!takePrepared(t, u.sample, f, info, p.overview) && !openSample(u.sample, f, info, p.overview)) return FAILED;
    p.file.close();
    p.file = std::move(f);
    p.spb = info.spb;
    p.loopBars = info.loopBars;
    p.frames = info.frames;
    p.preroll = prerollOf(info);
  }
  p.u = u;
  p.q = q;
  p.switchS = switchS;
  p.stop = stop;
  p.on = true;
  if (tr.rangePending && (int32_t)(tr.switchS - switchS) >= 0) tr.rangePending = false;   // replaced
  if ((int32_t)(tr.head - switchS) >= 0) {
    tr.head = switchS;                           // the old clip's buffered rest: dropped
    applyPlan(t);
  }                                              // otherwise when the stream gets there
  return PLANNED;
}

// The stream has reached the plan's switch point: the track takes on the new
// clip, and the interrupt is told where it starts (or that it stops there).
static void applyPlan(int t) {
  Track &tr = tracks[t];
  Plan &p = plans[t];
  if (!p.on) return;
  p.on = false;
  if (p.stop) {                                  // what follows in the stream isn't played
    crossfadeInto(tr, p.switchS, UINT32_MAX, p.before, p.beforeSaved);   // fades out
    noInterrupts();
    tr.switchAt = p.switchS;
    tr.nextStartBeat = (p.q + 3) / 4;            // (the beat it stops on: with a clip held a beat or more
    tr.nextSpb = 0;                              //  ahead in the ring this was applied early, and the beat
    tr.sampleSwitch = true;                      //  line check took a stale one at once)
    interrupts();
    return;
  }
  if (p.file) {
    tr.file.close();
    tr.file = std::move(p.file);                 // (SdFat copies: the copy left behind is let go)
    p.file.close();
    tr.sample = p.u.sample;
    tr.spbNative = p.spb;
    tr.loopBars = p.loopBars;
    tr.recordedFrames = p.frames;
    prerollFrames[t] = p.preroll;
    memcpy(tr.overview, p.overview, OVERVIEW_COLS);
    tr.overviewVersion++;
  }
  applyRange(t, p.u);
  // A range as long as the one playing carries on from the same spot in it
  // (a project switch: from its 1.1.1 position instead).
  if (p.restart) anchorAsStart(t, p.q);
  else if (tr.rangeBeats == p.outBeats) tr.rangeAnchorQ = p.outAnchorQ;
  if (tr.state == TrackState::Empty) tr.state = TrackState::Muted;   // (its settings follow at the switch)
  int32_t phaseQ = posMod(p.q - tr.rangeAnchorQ, 4 * tr.rangeBeats);
  tr.fileFrame = tr.rangeFrame(quarterFrames(phaseQ, tr.spbNative));
  crossfadeInto(tr, p.switchS, tr.fileFrame, p.before, p.beforeSaved);
  // The next beat line after the switch, and the stream frame there.
  int32_t beat = (p.q + 3) / 4;
  uint32_t s0 = p.switchS + quarterFrames(phaseQ + 4 * beat - p.q, tr.spbNative) - quarterFrames(phaseQ, tr.spbNative);
  noInterrupts();
  tr.switchAt = p.switchS;
  tr.nextS0 = s0;
  tr.nextStartBeat = beat;
  tr.nextSpb = tr.spbNative;
  tr.sampleSwitch = true;
  interrupts();
}

// A plan not carried out (the track stops or changes otherwise).
static void dropPlan(int t) {
  plans[t].on = false;
  plans[t].file.close();
  tracks[t].sampleSwitch = false;
}

// Track t to unit u right away: playing, from the next beat with time to
// fill. False if its sample can't be read (the track ends up empty).
static bool setUnitNow(int t, const TrackUnit &u) {
  Track &tr = tracks[t];
  TrackUnit now = trackUnit(t);
  if (!u.sample) {
    clearTrack(t);
    applySettings(t, u);
    tr.state = TrackState::Empty;
    return true;
  }
  if (sameClip(now, u) && tr.state != TrackState::Empty) {
    applySettings(t, u);
    trackChanged(t);
    return true;
  }
  dropPlan(t);
  silenceTracks(1 << t);
  tr.rangePending = false;
  if (tr.state == TrackState::Empty || now.sample != u.sample || !tr.file) {
    tr.file.close();
    if (!loadSample(t, u.sample)) {
      clearTrack(t);                             // its sample is gone
      return false;
    }
  }
  applyUnit(t, u);
  if (transport.running) startTrack(t, beatAfterLead(), false);
  trackChanged(t);
  return true;
}

// A change taken on at once (the transport stops, or jumps).
static void settleNow(int t) {
  Change &c = changes[t];
  if (!c.on) return;
  c.on = false;
  dropPlan(t);
  bool was = applyingUnit;
  applyingUnit = true;                           // (already part of the scene)
  setUnitNow(t, c.u);
  applyingUnit = was;
}

// Moves a change along: sets up its clip switch, then, once the transport is
// at its quarter beat and the stream has switched, applies its settings.
static void serviceChange(int t) {
  Change &c = changes[t];
  if (!c.on || (recording.mode != Recording::Idle && recording.track == t)) return;
  if (!transport.running) {
    settleNow(t);
    return;
  }
  Track &tr = tracks[t];
  if (!c.planned) {
    int64_t spb, now = nowSamples(spb);
    if (quarterTime(c.q, spb) - now < msToSamples(PLAN_LATE_MS))
      c.q = quarterAfterLead(c.grid);            // waited for another switch: the next one
    bool switchElsewhere = (plans[t].on || tr.sampleSwitch) && plans[t].q != c.q;   // (moved earlier)
    bool startElsewhere = tr.armed && !tr.live && tr.startBeat != (c.q + 3) / 4;
    if (sameClip(streamUnit(t), c.u) && !switchElsewhere && !startElsewhere && !(c.restart && c.u.sample)) {
      // The stream has that clip already (or both are empty).
    } else if (tr.live || plans[t].on || tr.sampleSwitch) {
      PlanResult r = planSwitch(t, c.u, c.q, c.restart);
      if (r == WAIT) return;
      if (r == FAILED) {                         // restarts on the next beat instead
        settleNow(t);
        return;
      }
    } else if (c.u.sample) {                     // from silence: starts on the beat of q
      dropPlan(t);
      tr.armed = false;
      if (tr.state == TrackState::Empty || tr.sample != c.u.sample || !tr.file) {
        tr.file.close();
        if (!loadSample(t, c.u.sample)) {
          c.on = false;
          clearTrack(t);
          return;
        }
      }
      applyRange(t, c.u);
      if (c.restart) anchorAsStart(t, 4 * ((c.q + 3) / 4));
      if (tr.state == TrackState::Empty) tr.state = TrackState::Muted;   // (until the settings follow)
      startTrack(t, (c.q + 3) / 4, false);
    }
    c.planned = true;
  }
  if (plans[t].on || tr.sampleSwitch) return;    // the stream isn't there yet
  int64_t spb, now = nowSamples(spb);
  if (now < quarterTime(c.q, spb)) return;
  c.on = false;
  bool was = applyingUnit;
  applyingUnit = true;
  if (!c.u.sample) {
    clearTrack(t);
    applySettings(t, c.u);
    tr.state = TrackState::Empty;
  } else {
    applySettings(t, c.u);
  }
  trackChanged(t);                               // (the unit file)
  applyingUnit = was;
}

static void serviceChanges() {
  for (int t = 0; t < NUM_TRACKS; t++) serviceChange(t);
}

// Scene changes (see storage.h).
static int32_t sceneQ = -1;                      // the latest one's switch point, in quarter beats
static int32_t sceneGrid = BEATS_PER_BAR;        // and its grid
static bool inSceneChange = false, sceneRestart = false;
// The clock's 1.1.1: transport beat clockOrigin (a project switch while
// playing moves it to its switch point, from clockOriginNext on).
static int32_t clockOrigin = 0, clockOriginNext = -1;

int32_t transportOrigin() {
  if (clockOriginNext >= 0 && transport.beat >= clockOriginNext) {
    clockOrigin = clockOriginNext;
    clockOriginNext = -1;
  }
  return clockOrigin;
}

static bool sceneChangeOn() {
  for (const auto &c : changes) if (c.on && c.scene) return true;
  return false;
}

int32_t beginSceneChange(bool quick, bool prepared, bool restart) {
  inSceneChange = true;
  sceneRestart = restart && transport.running;
  if (!transport.running) return -1;
  int64_t spb, now = nowSamples(spb);
  int32_t grid = quick ? 1 : BEATS_PER_BAR;
  int32_t q = quarterAfterLead(grid, prepared ? PREPARED_LEAD_MS : SWITCH_LEAD_MS);
  // Stepping on before the last one happened: the same point, if there's time
  // (and it isn't later than this one's would be).
  bool reuse = sceneChangeOn() && sceneQ >= 0 && quarterTime(sceneQ, spb) - now >= msToSamples(PLAN_LATE_MS) &&
               sceneQ <= q;
  if (!reuse) {
    sceneQ = q;
    sceneGrid = grid;
    // The waiting changes come along (the whole scene switches together).
    for (auto &c : changes)
      if (c.on && c.scene && c.q != q) {
        c.q = q;
        c.grid = grid;
        c.planned = false;
      }
  }
  if (sceneRestart) {                            // every change of this scene restarts (also ones carried along)
    for (auto &c : changes)
      if (c.on && c.scene) c.restart = true;
    clockOriginNext = sceneQ / 4;
  }
  return sceneQ / 4;
}

void endSceneChange() { inSceneChange = sceneRestart = false; }

bool sceneChangeWaiting() { return transport.running && sceneChangeOn(); }

// A change on its way to track t (unit u, quarter beat q).
static void requestChange(int t, const TrackUnit &u, int32_t q, bool scene) {
  Change &c = changes[t];
  c.on = true;
  c.planned = false;
  c.scene = scene;
  c.restart = scene && sceneRestart;
  c.q = q;
  c.u = u;
  serviceChange(t);
}

bool setTrackUnit(int t, const TrackUnit &u) {
  if (recording.mode != Recording::Idle && recording.track == t) return false;
  TrackUnit now = unitOf(t);
  bool restart = sceneRestart && u.sample;         // (a project switch: even the same clip starts over)
  if (!memcmp(&now, &u, sizeof u) && !restart) return true;   // nothing to do
  bool was = applyingUnit;
  applyingUnit = true;
  bool ok = true;
  if (!transport.running) {
    changes[t].on = false;
    ok = setUnitNow(t, u);
  } else {
    requestChange(t, u, inSceneChange ? sceneQ : quarterAfterLead(0), inSceneChange);
    changes[t].grid = inSceneChange ? sceneGrid : 0;
    trackChanged(t);                             // (the unit file: the change counts)
  }
  applyingUnit = was;
  return ok;
}

// Writes pending unit files, but only when the streams can afford a pause.
static void serviceHeaders() {
  if (!headerDirty) return;
  if (transport.running && !streamsTopped()) {
    writePending = true;
    return;
  }
  for (int t = 0; t < NUM_TRACKS; t++) {
    if (!(headerDirty & (1 << t))) continue;
    if (recording.track == t && recording.mode != Recording::Idle) continue;  // written at the end
    headerDirty &= ~(1 << t);
    writeUnit(t);
    writePending = false;
    return;                                      // one per pass
  }
}

// ---- Waveform overview
// Peaks per PEAK_BIN_FRAMES of the recording in progress (max |sample|, both
// channels), turned into the track's overview when it finishes.
static uint16_t recPeaks[MAX_REC_FRAMES / PEAK_BIN_FRAMES + 2];

static void addPeaks(const uint32_t *frames, uint32_t n, uint32_t firstFrame, uint16_t *bins) {
  for (uint32_t i = 0; i < n; i++) {
    int16_t l = (int16_t)(frames[i] & 0xFFFF), r = (int16_t)(frames[i] >> 16);
    uint16_t p = max(abs(l), abs(r));
    uint16_t &b = bins[(firstFrame + i) / PEAK_BIN_FRAMES];
    if (p > b) b = p;
  }
}

// Columns span loopFrames (anything after recordedFrames stays flat).
static void binsToColumns(const uint16_t *bins, uint32_t loopFrames, uint32_t recordedFrames, uint8_t *out) {
  uint32_t lastBin = recordedFrames ? (recordedFrames - 1) / PEAK_BIN_FRAMES : 0;
  for (int c = 0; c < OVERVIEW_COLS; c++) {
    uint32_t f0 = (uint64_t)loopFrames * c / OVERVIEW_COLS;
    uint32_t f1 = (uint64_t)loopFrames * (c + 1) / OVERVIEW_COLS;
    uint16_t peak = 0;
    if (recordedFrames && f0 < recordedFrames) {
      uint32_t b1 = min((max(f1, f0 + 1) - 1) / PEAK_BIN_FRAMES, lastBin);
      for (uint32_t b = f0 / PEAK_BIN_FRAMES; b <= b1; b++) peak = max(peak, bins[b]);
    }
    out[c] = (uint8_t)min(255.0f, sqrtf(peak / 32768.0f) * 255.0f + 0.5f);
  }
}

static void buildOverview(Track &tr, const uint16_t *bins, uint32_t recordedFrames) {
  binsToColumns(bins, tr.loopBars * BEATS_PER_BAR * tr.spbNative, recordedFrames, tr.overview);
  tr.overviewVersion++;
}

bool recordingWaveform(uint8_t *cols, uint32_t &bars, uint32_t &framesPerBar, uint32_t &recordedFrames) {
  if (recording.mode != Recording::Running && recording.mode != Recording::Finishing) return false;
  framesPerBar = BEATS_PER_BAR * transport.spb;
  recordedFrames = recording.written;
  if (recording.mode == Recording::Finishing) {
    bars = recording.loopBars;
  } else {
    // Room for the bars started so far, in powers of two (a stop can still
    // end it on any bar).
    uint32_t sofar = max<int32_t>(0, (int32_t)(transport.now - recording.startSample));
    uint32_t started = (sofar + framesPerBar - 1) / framesPerBar;
    bars = 1;
    while (bars < started) bars *= 2;
    bars = min(bars, MAX_BARS);
  }
  binsToColumns(recPeaks, bars * framesPerBar, recordedFrames, cols);
  return true;
}

// ---- Older versions: a full-size track1.wav ... per track, its settings in
// the header (versions 1-9). Recordings become samples, empty files recording
// space; the settings go to the track's unit file.
struct LegacyInfo {
  char magic[4];                                 // "M8LP"
  uint32_t version, used, spb, loopBars, recordedFrames, rangeStart, rangeLen, muted;
  uint32_t rangePhase;
};

static void migrateLegacy(int t, void (*progress)(const char *)) {
  char from[40], to[40];
  legacyPath(t, from, sizeof from);
  FsFile f = SD.sdfs.open(from, O_RDWR);
  if (!f) return;
  uint8_t h[WAV_DATA_OFFSET];
  LegacyInfo info = {};
  bool read = f.seekSet(0) && f.read(h, sizeof h) == (int)sizeof h;
  if (read) memcpy(&info, h + INFO_OFFSET, sizeof info);
  bool ok = read && !memcmp(h, "RIFF", 4) && !memcmp(info.magic, "M8LP", 4) && info.version >= 1 && info.version <= 9;
  if (ok && info.version == 1) { info.rangeStart *= BEATS_PER_BAR; info.rangeLen *= BEATS_PER_BAR; }
  if (ok && info.version < 3) info.rangePhase = 0;
  if (ok && info.version < 4) info.rangePhase *= 4;
  uint32_t loopBeats = info.loopBars * BEATS_PER_BAR;
  bool used = ok && info.used && info.spb && info.loopBars && info.loopBars <= MAX_BARS && info.rangeLen &&
              info.rangeStart + info.rangeLen <= loopBeats && info.recordedFrames && info.recordedFrames <= MAX_REC_FRAMES;
  if (!used) {                                   // empty: keep it as recording space if it's full size
    bool space = f.fileSize() == SAMPLE_FILE_BYTES && poolCount < POOL_SIZE && nextSample <= MAX_NUMBER;
    if (space) writeSampleHeader(f, 0, 0, 0, nullptr);
    f.close();
    poolPath(nextSample, to, sizeof to);
    if (space && SD.sdfs.rename(from, to)) pool[poolCount++] = nextSample++;
    else SD.sdfs.remove(from);
    return;
  }
  char msg[48];
  snprintf(msg, sizeof msg, "converting track %d to a sample...", t + 1);
  progress(msg);
  // The overview: from the header (version 6 on), else from the audio.
  Track &tr = tracks[t];
  tr.loopBars = info.loopBars;
  tr.spbNative = info.spb;
  if (info.version >= 6) {
    memcpy(tr.overview, h + INFO_OFFSET + sizeof(LegacyInfo), OVERVIEW_COLS);
  } else {
    memset(recPeaks, 0, sizeof recPeaks);
    for (uint32_t fr = 0; fr < info.recordedFrames; fr += CHUNK_FRAMES) {
      uint32_t n = min(CHUNK_FRAMES, info.recordedFrames - fr);
      if (!f.seekSet(WAV_DATA_OFFSET + (uint64_t)fr * 4) || f.read(chunk, n * 4) != (int)(n * 4)) break;
      addPeaks(chunk, n, fr, recPeaks);
    }
    buildOverview(tr, recPeaks, info.recordedFrames);
  }
  TrackUnit u = {{'M', '8', 'T', 'U'}, UNIT_VERSION, nextSample, info.rangeStart, info.rangeLen, info.rangePhase,
                info.muted, 0, FILTER_MAX, VOLUME_UNITY, 0};
  const uint32_t FILTER_AT = INFO_OFFSET + sizeof(LegacyInfo) + OVERVIEW_COLS, MIX_AT = FILTER_AT + 2;
  if (info.version >= 8) { u.filterLo = (int8_t)h[FILTER_AT]; u.filterHi = (int8_t)h[FILTER_AT + 1]; }
  else if (info.version == 7) { u.filterLo = h[FILTER_AT]; u.filterHi = h[FILTER_AT] + h[FILTER_AT + 1]; }
  if (info.version >= 9) {                         // (in the old steps)
    u.volume = (int8_t)h[MIX_AT];
    u.balance = (int8_t)h[MIX_AT + 1];
    u.version = 1;
    upgradeUnit(u);
  }
  writeSampleHeader(f, info.recordedFrames, info.spb, info.loopBars, tr.overview, lastOrder + 1);
  f.truncate(WAV_DATA_OFFSET + (uint64_t)info.recordedFrames * 4);
  f.close();
  samplePath(nextSample, to, sizeof to);
  if (!SD.sdfs.rename(from, to)) return;
  noteSample(nextSample++);
  lastOrder++;
  tr.file.close();
  if (loadSample(t, u.sample)) applyUnit(t, u);
  writeUnit(t);
}

// ---- Clipboard
// Marks on the samples (by their place in sampleIds): the ones in use.
static uint8_t sampleMarks[(MAX_SAMPLES + 7) / 8];
void clearSampleMarks() { memset(sampleMarks, 0, sizeof sampleMarks); }
void markSample(uint32_t id) {
  int i = sampleIndex(id);
  if (id && i < sampleTotal && sampleIds[i] == id) sampleMarks[i / 8] |= 1 << (i % 8);
}
bool sampleMarked(int i) { return i >= 0 && i < sampleTotal && (sampleMarks[i / 8] >> (i % 8) & 1); }

// Deletes the recordings not marked, keeping what the tracks, the clipboard
// and a recording use; empty recording space (the pool) isn't touched. Only
// stopped and idle (it changes the file system). The marks are cleared.
int deleteUnmarkedSamples() {
  if (transport.running || recording.mode != Recording::Idle) return 0;
  int deleted = 0, kept = 0;
  for (int i = 0; i < sampleTotal; i++) {
    uint32_t id = sampleIds[i];
    bool keep = sampleMarked(i) || id == clipboard.sample;
    for (int t = 0; t < NUM_TRACKS && !keep; t++)
      if (tracks[t].sample == id || changes[t].u.sample == id || (plans[t].on && plans[t].u.sample == id)) keep = true;
    char path[40];
    samplePath(id, path, sizeof path);
    debugStage = "SD remove (unused sample)";
    if (keep || !SD.sdfs.remove(path)) {
      sampleIds[kept++] = id;                    // (in place: kept <= i)
      continue;
    }
    for (int k = 0; k < untrimmedCount; k++)
      if (untrimmed[k] == id) untrimmed[k--] = untrimmed[--untrimmedCount];
    int u = unnamedIndex(id);
    if (u >= 0) unnamed[u] = unnamed[--unnamedCount];
    deleted++;
  }
  sampleTotal = kept;
  clearSampleMarks();
  if (deleted) Serial.printf("deleted %d unused recording(s)\n", deleted);
  return deleted;
}

bool clipboardFull() { return clipboard.sample != 0; }

bool cutTrack(int t) {
  if (tracks[t].state == TrackState::Empty || recordingTrack(t)) return false;
  clipboard = unitOf(t);
  clearTrack(t);
  return true;
}

bool pasteTrack(int t) {
  Track &tr = tracks[t];
  if (!clipboard.sample || tr.state != TrackState::Empty || recordingTrack(t))
    return false;
  if (!loadSample(t, clipboard.sample)) return false;
  applyUnit(t, clipboard);                       // its alignment too: in step with the track it came from
  trackChanged(t);
  if (transport.running) startTrack(t, transport.beat + 2, false);
  return true;
}

// ---- Choosing a track's sample
bool stepSample(int t, int dir) {
  if (sampleTotal < 2 || recordingTrack(t)) return false;
  TrackUnit u = unitOf(t);                       // (a planned change counts)
  // In the order of the names from the track's sample, wrapping around: this
  // track's recordings in this project, then its other tracks', then other
  // projects'. An empty track starts from the first of its own (going
  // forward) or the last (going back).
  int i;
  if (u.sample) {
    i = sampleIndex(u.sample);                   // (its place, or where it would be)
    i = dir > 0 ? i + (i < sampleTotal && sampleIds[i] == u.sample) : i - 1;
  } else {
    uint32_t own = idOf(tagOf(recordLetter, screenTrack(t)), 0);
    i = dir > 0 ? sampleIndex(own + 1) : sampleIndex(own + (1u << NUMBER_BITS)) - 1;
  }
  uint32_t n = sampleIds[(i % sampleTotal + sampleTotal) % sampleTotal];
  if (n == u.sample) return false;
  // Volume, balance, filter and mute stay; the range is the whole loop again
  // (length 0), aligned to the bar counter (its start on 1.1.1).
  u.sample = n;
  u.rangeStart = u.rangeLen = u.rangePhase = 0;
  if (!transport.running) return setUnitNow(t, u);
  requestChange(t, u, quarterAfterLead(0), false);
  trackChanged(t);
  return true;
}

Session loadSession() {
  Session s;
  FsFile f = SD.sdfs.open("/L7/DATA/SESSION.DAT", O_RDONLY);
  char line[32];
  // (Parsed by hand: sscanf would pull ~26 KB of Unicode tables into RAM.)
  while (f && f.fgets(line, sizeof line) > 0) {
    if (!strncmp(line, "bpm=", 4)) s.bpm = strtof(line + 4, nullptr);
    else if (!strncmp(line, "newbpm=", 7)) s.newBpm = strtof(line + 7, nullptr);   // (new projects' tempo)
    else if (!strncmp(line, "row=", 4)) s.row = (int)strtol(line + 4, nullptr, 10);
    else if (!strncmp(line, "cell=", 5)) s.cell = (int)strtol(line + 5, nullptr, 10);
    // ("volume=": the master volume on today's scale; an older "master=" is left: 100% again)
    else if (!strncmp(line, "volume=", 7)) s.master = (int)strtol(line + 7, nullptr, 10);
    else if (!strncmp(line, "newvolume=", 10)) s.newMaster = (int)strtol(line + 10, nullptr, 10);   // (new projects')
    else if (!strncmp(line, "project=", 8)) s.project = (int)strtol(line + 8, nullptr, 10) - 1;
    else if (!strncmp(line, "menu=", 5)) s.menu = (int)strtol(line + 5, nullptr, 10);
  }
  f.close();
  s.bpm = constrain(s.bpm, MIN_BPM, MAX_BPM);
  s.newBpm = constrain(s.newBpm, MIN_BPM, MAX_BPM);
  s.master = constrain(s.master, 0, MASTER_VOLUME_MAX);
  s.newMaster = constrain(s.newMaster, 0, MASTER_VOLUME_MAX);
  s.project = constrain(s.project, -1, PROJECTS - 1);   // (-1, "project=0": the dummy project)
  return s;
}

void saveSession(const Session &s) {
  FsFile f = SD.sdfs.open("/L7/DATA/SESSION.DAT", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  f.printf("bpm=%.2f\nnewbpm=%.2f\nvolume=%d\nnewvolume=%d\nproject=%d\nmenu=%d\n", s.bpm, s.newBpm, s.master, s.newMaster,
           s.project + 1, s.menu);
  f.close();
}

// One folder's files. SAMPLES: the recordings (cut down to size if needed);
// DATA: recording space, empty or holding a recording not named yet. A card
// from before the folders had recording space in SAMPLES: those files are
// listed in `moves` (numbers), to be moved to DATA.
static constexpr int MAX_MOVES = 128;
static uint16_t moves[MAX_MOVES];
static int moveCount = 0;

static void scanFolder(const char *folder, bool data) {
  FsFile dir = SD.sdfs.open(folder, O_RDONLY);
  FsFile f;
  char name[32];
  while (dir && f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof name);
    unsigned n = 0;                              // "NNNN.wav" (in DATA: .DAT), or "XYNNNN.wav" (project letter, track)
    size_t len = strlen(name);
    uint8_t tag = len == 10 ? tagOf(name[0], name[1] - '0') : 0;
    if (data && tag) { f.close(); continue; }    // (DATA holds untagged names only)
    const char *num = name + (tag ? 2 : 0);
    bool digits = (len == 8 || tag) && !strcasecmp(num + 4, data ? ".DAT" : ".wav");
    for (int i = 0; digits && i < 4; i++) {
      if (num[i] < '0' || num[i] > '9') digits = false;
      n = n * 10 + (num[i] - '0');
    }
    bool numbered = !f.isDir() && digits && n >= 1 && n <= MAX_NUMBER;
    SampleInfo info;
    uint8_t headerTag = 0;
    uint32_t headerNumber = 0;
    bool ok = numbered && readSampleHeader(f, info, nullptr, &headerTag, &headerNumber);
    uint64_t size = f.fileSize();
    f.close();
    if (!numbered) continue;
    if (tag) nextNumber[tag] = umax(nextNumber[tag], n + 1);
    else nextSample = umax(nextSample, n + 1);
    bool recording = ok && playable(info) && !tag && headerTag && headerNumber;   // recorded, not named yet
    bool space = ok && !playable(info) && info.frames == 0 && size == SAMPLE_FILE_BYTES;
    if (!data && !tag && (recording || space)) {   // (an older card: recording space belongs in DATA)
      if (moveCount < MAX_MOVES) moves[moveCount++] = (uint16_t)n;
      continue;
    }
    if (ok && playable(info)) {
      uint32_t id = tag ? idOf(tag, n) : n;
      if (tag && !headerNumber && olderCount < (int)(sizeof older / sizeof older[0])) {
        id = n;                                  // named by the version before: known by its number
        older[olderCount++] = {(uint16_t)n, tag};
      } else if (recording) {
        if (unnamedCount >= (int)(sizeof unnamed / sizeof unnamed[0])) continue;
        id = idOf(headerTag, headerNumber);
        unnamed[unnamedCount++] = {id, (uint16_t)n};
        nextNumber[headerTag] = umax(nextNumber[headerTag], headerNumber + 1);
      } else if (data) {
        continue;                                // (an untagged sample in DATA: not one of ours)
      }
      noteSample(id);
      lastOrder = max(lastOrder, info.version >= 2 && info.order ? info.order : n);
      if ((size > WAV_DATA_OFFSET + (uint64_t)info.frames * 4 || unnamedIndex(id) >= 0) && untrimmedCount < 64)
        untrimmed[untrimmedCount++] = id;
    } else if (data && space && poolCount < POOL_SIZE) {
      int i = poolCount++;                       // keep the pool sorted
      while (i > 0 && pool[i - 1] > n) { pool[i] = pool[i - 1]; i--; }
      pool[i] = n;
    }
  }
}

// Recording space of the layout before DATA had .DAT files (NNNN.wav) becomes NNNN.DAT.
static void renameDataWavs() {
  uint16_t nums[MAX_MOVES];
  int count = 0;
  {
    FsFile dir = SD.sdfs.open("/L7/DATA", O_RDONLY), f;
    char name[32];
    while (dir && count < MAX_MOVES && f.openNext(&dir, O_RDONLY)) {
      f.getName(name, sizeof name);
      bool file = !f.isDir();
      f.close();
      unsigned n = 0;
      bool ok = file && strlen(name) == 8 && !strcasecmp(name + 4, ".wav");
      for (int i = 0; ok && i < 4; i++) {
        ok = name[i] >= '0' && name[i] <= '9';
        n = n * 10 + (name[i] - '0');
      }
      if (ok) nums[count++] = (uint16_t)n;
    }
  }
  for (int i = 0; i < count; i++) {
    char from[40], to[40];
    snprintf(from, sizeof from, "/L7/DATA/%04u.wav", nums[i]);
    poolPath(nums[i], to, sizeof to);
    SD.sdfs.rename(from, to);
  }
}

// Finds the samples: which hold recordings, which are recording space, and
// the next free number.
static void scanSamples() {
  sampleTotal = 0;
  lastOrder = 0;
  poolCount = 0;
  untrimmedCount = 0;
  unnamedCount = 0;
  olderCount = 0;
  moveCount = 0;
  memset(nextNumber, 0, sizeof nextNumber);
  scanFolder("/L7/SAMPLES", false);
  for (int i = 0; i < moveCount; i++) {          // recording space from an older card, into DATA
    char from[40], to[40];
    snprintf(from, sizeof from, "/L7/SAMPLES/%04u.wav", moves[i]);
    poolPath(moves[i], to, sizeof to);
    SD.sdfs.rename(from, to);
  }
  renameDataWavs();
  scanFolder("/L7/DATA", true);
  for (int i = 0; i < olderCount; i++)          // (new files without a tag numbered past those)
    nextSample = umax(nextSample, older[i].number + 1u);
}

// Recording space numbered below a recording (left from converting an older
// version's track files) is renamed to the next free numbers, so new
// recordings are numbered in the order they're made.
static void renumberPool() {
  uint32_t highest = 0;
  for (int i = 0; i < sampleTotal; i++) if (!idTag(sampleIds[i])) highest = max(highest, sampleIds[i]);
  for (int i = 0; i < poolCount; i++) {
    if (pool[i] > highest || nextSample > MAX_NUMBER) continue;
    char from[40], to[40];
    poolPath(pool[i], from, sizeof from);
    poolPath(nextSample, to, sizeof to);
    if (SD.sdfs.rename(from, to)) pool[i] = nextSample++;
  }
  for (int i = 1; i < poolCount; i++)            // keep it sorted
    for (int j = i; j > 0 && pool[j - 1] > pool[j]; j--) { uint16_t x = pool[j]; pool[j] = pool[j - 1]; pool[j - 1] = x; }
}

bool moveFile(const char *from, const char *to) {
  return SD.sdfs.exists(from) && !SD.sdfs.exists(to) && SD.sdfs.rename(from, to);
}

// A folder of the app's, made if missing, with its name's letter case as given
// (a FAT card keeps the case a folder was made with; the older ones were lower case).
static void ensureFolder(const char *path) {
  FsFile d = SD.sdfs.open(path, O_RDONLY);
  if (!d) {
    SD.sdfs.mkdir(path);
    return;
  }
  char name[32];
  d.getName(name, sizeof name);
  d.close();
  const char *want = strrchr(path, '/') + 1;
  if (!strcmp(name, want)) return;
  char tmp[48];
  snprintf(tmp, sizeof tmp, "%.*s/_%s", (int)(want - path - 1), path, want);
  if (SD.sdfs.rename(path, tmp)) SD.sdfs.rename(tmp, path);
}

bool storageBegin(void (*progress)(const char *)) {
  // DMA mode: every read/write is one self-contained transfer done by the SD
  // controller. The default FIFO mode leaves a multi-sector transfer open
  // between calls and has the CPU poll the FIFO; on the M8 it stalled for
  // seconds at a time (1 s timeouts, retried) with the audio engine running.
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) return false;
  if (!SD.sdfs.exists("/L7") && SD.sdfs.exists("/looper")) SD.sdfs.rename("/looper", "/L7");   // (the app's folder, renamed)
  SD.sdfs.mkdir("/L7");
  if (!SD.sdfs.exists("/L7/EXPORTS") && SD.sdfs.exists("/Ableton")) SD.sdfs.rename("/Ableton", "/L7/EXPORTS");   // (the exports' folder, moved)
  ensureFolder("/L7/SAMPLES");
  ensureFolder("/L7/PROJECTS");
  ensureFolder("/L7/EXPORTS");
  ensureFolder("/L7/DATA");
  moveFile("/L7/session.txt", "/L7/DATA/SESSION.DAT");   // (the files of /L7 itself, an older card's, into DATA)
  moveFile("/L7/DATA/SESSION.TXT", "/L7/DATA/SESSION.DAT");
  for (int t = 0; t < NUM_TRACKS; t++) {
    char from[40], to[40];
    unitPath(t, to, sizeof to);
    snprintf(from, sizeof from, "/L7/track%d.unit", t + 1);
    moveFile(from, to);
    snprintf(from, sizeof from, "/L7/DATA/TRACK%d.UNIT", t + 1);
    moveFile(from, to);
  }
  scanSamples();
  while (untrimmedCount) trimSample(untrimmed[--untrimmedCount]);
  bool ok = true;
  for (int t = 0; t < NUM_TRACKS; t++) {
    char path[40];
    snprintf(path, sizeof path, "/L7/track%d.txt", t + 1);
    SD.sdfs.remove(path);                        // settings files of an early version
    Track &tr = tracks[t];
    unitPath(t, path, sizeof path);
    tr.unitFile = SD.sdfs.open(path, O_RDWR);
    if (!tr.unitFile) tr.unitFile = SD.sdfs.open(path, O_RDWR | O_CREAT);
    ok &= (bool)tr.unitFile;
    tr.file.close();
    readUnit(t);
    migrateLegacy(t, progress);                  // older version: its track file becomes a sample
  }
  renumberPool();
  while (poolCount < POOL_SIZE) {                // (serial only: on screen it showed on every start with the card full)
    Serial.printf("preparing recording space (%d of %d)...\n", poolCount + 1, POOL_SIZE);
    if (!createPoolFile()) break;
  }
  openPoolNext();
  return ok && poolCount > 0;
}

// Slow or failed SD transfers go to Serial with the card's error code. Writes
// of 50-200 ms are normal for SD cards and covered by the buffers (~280 ms of
// playback per track, ~740 ms of input while recording); only longer ones are
// worth reporting.
static constexpr uint32_t SLOW_SD_MS = 150;
static void logSd(const char *op, uint32_t ms, int got, uint32_t want) {
  SdCard *card = SD.sdfs.card();
  Serial.printf("slow SD %s: %lu ms, %d/%lu bytes, card error 0x%02X data 0x%lX\n", op, ms, got, want,
                card ? card->errorCode() : 0, card ? card->errorData() : 0);
}

// ---- Recovery from a card that stops responding
// When a transfer fails, reads return silence and writes wait, and
// serviceStorage() re-initialises the card and reopens the files.
static bool sdFailed = false;

static void recoverSd() {
  // After a failed attempt, wait a second before the next; after a successful
  // one, recover again at once (a wait would lose recording input).
  static uint32_t lastFailedTry = 0;
  static bool lastTryFailed = false;
  if (lastTryFailed && millis() - lastFailedTry < 1000) return;
  debugStage = "SD recovery";
  Serial.println("SD card stopped responding: re-initialising it");
  // Drop the handles without closing them: closing would talk to the card.
  for (auto &tr : tracks) { tr.file = FsFile(); tr.unitFile = FsFile(); }
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) {
    Serial.println("SD re-init failed, retrying");
    lastTryFailed = true;
    lastFailedTry = millis();
    return;
  }
  lastTryFailed = false;
  char path[40];
  for (int t = 0; t < NUM_TRACKS; t++) {
    Track &tr = tracks[t];
    unitPath(t, path, sizeof path);
    tr.unitFile = SD.sdfs.open(path, O_RDWR);
    if (!tr.sample) continue;
    samplePath(tr.sample, path, sizeof path);
    tr.file = SD.sdfs.open(path, O_RDWR);
  }
  sdFailed = false;
  Serial.println("SD card back");
}

// ---- Streaming

// ---- Declicking: crossfades where the stream jumps
// Where a stream jumps (a range's end back to its start, the loop's end back
// to frame 0, a switch to another range or clip), the XFADE_FRAMES before the
// jump are crossfaded into the frames that precede the jump's target in the
// recording, so the target follows on smoothly and keeps its attack. Before
// frame 0 that's the recording's stored pre-roll; without one (older samples,
// a recording still being saved) silence: a short fade out.

static uint32_t mixFrames(uint32_t a, uint32_t b, float w) {   // (1 - w) a + w b, per channel
  float al = (int16_t)(a & 0xFFFF), ar = (int16_t)(a >> 16), bl = (int16_t)(b & 0xFFFF), br = (int16_t)(b >> 16);
  int16_t l = (int16_t)lroundf(al + (bl - al) * w), r = (int16_t)lroundf(ar + (br - ar) * w);
  return (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
}

// The XFADE_FRAMES of track t's sample before loop frame `at` (UINT32_MAX:
// silence), into out.
static void readFramesBefore(Track &tr, uint32_t at, uint32_t *out) {
  memset(out, 0, XFADE_FRAMES * 4);
  if (at == UINT32_MAX || sdFailed) return;
  uint32_t from, n = XFADE_FRAMES;
  if (at == 0) {
    if (prerollFrames[&tr - tracks] < XFADE_FRAMES) return;
    from = tr.recordedFrames;                    // the pre-roll, kept after the loop
  } else if (at >= XFADE_FRAMES) {
    from = at - XFADE_FRAMES;
    if (from >= tr.recordedFrames) return;       // the loop's silent end
    n = min(n, tr.recordedFrames - from);
  } else {
    return;                                      // (ranges start on beats: not used)
  }
  debugStage = "SD read (crossfade)";
  if (!tr.file.seekSet(WAV_DATA_OFFSET + (uint64_t)from * 4) || tr.file.read(out, n * 4) != (int)(n * 4))
    memset(out, 0, XFADE_FRAMES * 4);
}

// The frames before each jump target a track keeps hitting (a range's start,
// frame 0), so wrapping costs no read: two per track.
struct FramesBefore { uint32_t sample = 0; uint32_t at = 0, preroll = 0; uint32_t frames[XFADE_FRAMES]; };
DMAMEM static FramesBefore framesBefore[NUM_TRACKS][2];   // (RAM2, as plans)
static uint8_t framesBeforeNext[NUM_TRACKS];

static const uint32_t *cachedFramesBefore(Track &tr, uint32_t at) {
  int t = &tr - tracks;
  for (auto &c : framesBefore[t])
    if (c.sample == tr.sample && c.at == at && c.preroll == prerollFrames[t]) return c.frames;
  FramesBefore &c = framesBefore[t][framesBeforeNext[t]++ & 1];
  readFramesBefore(tr, at, c.frames);
  c.sample = tr.sample;
  c.at = at;
  c.preroll = prerollFrames[t];
  return c.frames;
}

// A switch at stream frame `at` (ring head) into the clip now set up from loop
// frame `entry` (UINT32_MAX: into silence): the ring's last XFADE_FRAMES
// before it crossfade into what precedes the entry. `saved` keeps those
// frames as they were, so a switch replanned to the same point starts over
// from them.
static void crossfadeInto(Track &tr, uint32_t at, uint32_t entry, uint32_t *saved, bool &haveSaved) {
  uint32_t into[XFADE_FRAMES];
  readFramesBefore(tr, entry, into);
  noInterrupts();
  uint32_t tail = tr.tail;
  interrupts();
  for (uint32_t i = 0; i < XFADE_FRAMES; i++) {
    uint32_t s = at - XFADE_FRAMES + i;
    uint32_t &f = tr.ring[s % RING_FRAMES];
    if (haveSaved) f = saved[i];
    else saved[i] = f;
    if ((int32_t)(s - tail) < (int32_t)(2 * AUDIO_BLOCK_SAMPLES)) continue;   // already playing
    f = mixFrames(f, into[i], (i + 1.0f) / (XFADE_FRAMES + 1));
  }
  haveSaved = true;
}

// The stream has reached the queued range's switch point. If the new range
// contains the spot that would have played next, carry on from it (the range
// is re-anchored around the playhead); otherwise start the new range in
// phase with the existing anchor.
static void applyQueuedRange(Track &tr) {
  uint32_t before[XFADE_FRAMES];
  bool saved = false;
  tr.rangeStartBeat = tr.pendingStartBeat;
  tr.rangeBeats = tr.pendingBeats;
  tr.rangePending = false;
  uint32_t offset = tr.rangeOffset(tr.fileFrame);
  if (offset < tr.rangeFrames()) {
    // Playhead position in the new range, in quarter beats (switches fall on
    // quarter beats; round away the per-quarter rounding of frames).
    int32_t posQ = (int32_t)(((uint64_t)offset * 4 + tr.spbNative / 2) / tr.spbNative);
    tr.rangeAnchorQ = tr.switchQ - posQ;
  } else {
    int32_t phaseQ = posMod(tr.switchQ - tr.rangeAnchorQ, 4 * tr.rangeBeats);
    tr.fileFrame = tr.rangeFrame(quarterFrames(phaseQ, tr.spbNative));
    crossfadeInto(tr, tr.head, tr.fileFrame, before, saved);   // (the switch point: head)
  }
  trackChanged(&tr - tracks);
}

// A track whose recording is still being saved, and that has caught up with
// it: nothing to read yet.
static bool waitingForRecording(const Track &tr) {
  return recording.mode != Recording::Idle && recording.track == &tr - tracks &&
         tr.fileFrame < tr.recordedFrames && tr.fileFrame >= recording.written;
}

// One SD read (or one block of padding silence) into the track's ring.
static void produce(Track &tr) {
  if (tr.rangePending && tr.head == tr.switchS) applyQueuedRange(tr);
  Plan &plan = plans[&tr - tracks];
  if (plan.on && tr.head == plan.switchS) applyPlan(&tr - tracks);
  uint32_t free = RING_FRAMES - (tr.head - tr.tail);
  // Up to the range's end or the loop's end, whichever comes first (a range
  // can run past the loop's end and carry on from its start).
  uint32_t toRangeEnd = tr.rangeFrames() - tr.rangeOffset(tr.fileFrame);
  uint32_t n = min(min(CHUNK_FRAMES, free), min(toRangeEnd, tr.loopFrames() - tr.fileFrame));
  if (tr.rangePending) n = min(n, tr.switchS - tr.head);      // stop exactly at the switch
  if (plan.on) n = min(n, plan.switchS - tr.head);
  if (tr.fileFrame >= tr.recordedFrames || sdFailed) {
    memset(chunk, 0, n * 4);                     // padding after the recording (or no card)
  } else {
    n = min(n, tr.recordedFrames - tr.fileFrame);
    if (waitingForRecording(tr)) return;
    if (recording.mode != Recording::Idle && recording.track == &tr - tracks)
      n = min(n, recording.written - tr.fileFrame);   // still being recorded: only what's saved
    debugStage = "SD read (track stream)";
    uint32_t t0 = millis();
    tr.file.seekSet(WAV_DATA_OFFSET + (uint64_t)tr.fileFrame * 4);
    int got = tr.file.read(chunk, n * 4);
    uint32_t ms = millis() - t0;
    if (got != (int)(n * 4)) {
      memset(chunk, 0, n * 4);
      // Only a card error means the card stopped responding; a short read
      // without one is just the end of the file.
      if (got < 0 || SD.sdfs.card()->errorCode()) sdFailed = true;
    }
    if (ms > SLOW_SD_MS || got != (int)(n * 4))
      logSd("read", ms, got, n * 4);
  }
  // Frames close to the next jump crossfade into what precedes its target.
  uint32_t toLoopEnd = tr.loopFrames() - tr.fileFrame, toJump = min(toRangeEnd, toLoopEnd);
  if (toJump - n < XFADE_FRAMES) {
    uint32_t target = toRangeEnd <= toLoopEnd ? tr.rangeStartFrame() : 0;
    const uint32_t *into = cachedFramesBefore(tr, target);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t d = toJump - i;                     // frames to the jump (1: the last before it)
      if (d <= XFADE_FRAMES) {
        uint32_t k = XFADE_FRAMES - d;
        chunk[i] = mixFrames(chunk[i], into[k], (k + 1.0f) / (XFADE_FRAMES + 1));
      }
    }
  }
  uint32_t head = tr.head;
  for (uint32_t i = 0; i < n; i++) tr.ring[(head + i) % RING_FRAMES] = chunk[i];
  tr.head = head + n;
  if (n == toRangeEnd) tr.fileFrame = tr.rangeStartFrame();
  else tr.fileFrame = (tr.fileFrame + n) % tr.loopFrames();
}

// Sets the track up to join in at transport beat beat0, in phase with its
// anchor and loop range. With fillNow the ring is filled right away (transport
// start); otherwise serviceStreams() fills it, emptiest track first.
static void startTrack(int t, int32_t beat0, bool fillNow) {
  Track &tr = tracks[t];
  silenceTracks(1 << t);                         // (if it was playing)
  tr.needsRestart = false;
  applyPlan(t);                                  // a planned clip: starts as that
  tr.sampleSwitch = false;
  if (tr.rangePending) {                         // restarting: take the queued range now
    tr.rangeStartBeat = tr.pendingStartBeat;
    tr.rangeBeats = tr.pendingBeats;
    tr.rangePending = false;
    trackChanged(t);
  }
  if (tr.state == TrackState::Empty) return;
  memset(tr.filterZ, 0, sizeof tr.filterZ);      // not live: the interrupt isn't using them
  int32_t phaseQ = posMod(4 * beat0 - tr.rangeAnchorQ, 4 * tr.rangeBeats);
  tr.s0 = quarterFrames(phaseQ, tr.spbNative);
  tr.tail = tr.s0;
  tr.head = tr.s0;
  tr.fileFrame = tr.rangeFrame(tr.s0);
  if (fillNow)
    while (RING_FRAMES - (tr.head - tr.tail) >= CHUNK_FRAMES) produce(tr);
  tr.startBeat = beat0;
  tr.playSpb = tr.spbNative;
  tr.armed = true;
}

// A write is waiting (recording or a header): top the buffers up with
// half-chunk reads too, so the write starts with as much playback in reserve
// as possible. Otherwise read whole chunks only (cheaper per byte).
static uint32_t refillThreshold() { return writePending ? CHUNK_FRAMES / 2 : CHUNK_FRAMES; }

static void serviceStreams() {
  // Plans whose switch point the stream has reached (even with the ring full).
  for (int t = 0; t < NUM_TRACKS; t++)
    if (plans[t].on && tracks[t].head == plans[t].switchS) applyPlan(t);
  for (int pass = 0; pass < 4; pass++) {
    Track *best = nullptr;
    uint32_t bestFill = RING_FRAMES;
    for (auto &tr : tracks) {
      if (!(tr.live || tr.armed) || waitingForRecording(tr)) continue;
      uint32_t fill = tr.head - tr.tail;
      if (RING_FRAMES - fill >= refillThreshold() && fill < bestFill) { best = &tr; bestFill = fill; }
    }
    if (!best) return;
    produce(*best);
  }
}

// True when every playing track's buffer is topped up, so a slow write starts
// with the most playback in reserve.
static bool streamsTopped() {
  for (const auto &tr : tracks)
    if ((tr.live || tr.armed) && RING_FRAMES - (tr.head - tr.tail) >= CHUNK_FRAMES / 2) return false;
  return true;
}

// ---- Recording
// Loop length in bars for a recording stopped after `beats` beats: the whole
// bars done, if the stop came at most SNAP_THRESHOLD_BEATS past the last bar
// line; otherwise one more (the recording then carries on to the end of that
// bar). Under one bar it carries on to one bar.
static uint32_t targetBars(double beats) {
  uint32_t p = min((uint32_t)(beats / BEATS_PER_BAR), MAX_BARS);   // whole bars done
  if (p >= 1 && (beats <= p * BEATS_PER_BAR + SNAP_THRESHOLD_BEATS || p == MAX_BARS)) return p;
  return p + 1;
}

static uint32_t maxRecordingFrames() {
  return min(MAX_BARS * BEATS_PER_BAR * transport.spb, MAX_REC_FRAMES);
}

// The recording's loop length is known: set its track up to play. If the
// loop's end is still ahead (recording carries on to it), the track joins
// exactly there, from the beginning already in its ring, so playback follows
// the recording with no gap. If it has passed (trimmed), it joins at the next
// beat, read from the file right away. Frames not yet saved are never read
// (produce() waits for them).
static void armRecordedLoop(bool endAhead) {
  int t = recording.track;
  Track &tr = tracks[t];
  tr.live = false;
  tr.armed = false;
  tr.needsRestart = false;
  tr.spbNative = transport.spb;
  tr.loopBars = recording.loopBars;
  tr.recordedFrames = recording.targetFrames;
  tr.rangeStartBeat = 0;
  tr.rangeBeats = tr.loopBars * BEATS_PER_BAR;
  tr.rangePending = false;
  tr.rangeAnchorQ = 4 * recording.startBeat;
  tr.state = TrackState::Playing;
  if (!transport.running) return;
  if (!endAhead) {
    startTrack(t, transport.beat + 1, true);
    return;
  }
  memset(tr.filterZ, 0, sizeof tr.filterZ);
  tr.s0 = 0;
  tr.tail = 0;
  tr.frac = 0;
  tr.head = min(recording.written, RING_FRAMES);  // the beginning, already in the ring
  tr.fileFrame = tr.head;
  tr.startBeat = recording.startBeat + (int32_t)tr.rangeBeats;
  tr.playSpb = tr.spbNative;
  tr.armed = true;
}

static void stopRecording() {
  noInterrupts();
  uint32_t stopSample = transport.now + recordingLatency();
  uint32_t spb = transport.spb;
  interrupts();
  uint32_t length = min(stopSample - recording.startSample, maxRecordingFrames());
  recording.loopBars = targetBars((double)length / spb);
  // Shorter than `length`: the end is trimmed. Longer: recording continues
  // until it is reached (serviceRecorder keeps writing the input until then).
  recording.targetFrames = min(recording.loopBars * BEATS_PER_BAR * spb, maxRecordingFrames());
  recording.mode = Recording::Finishing;
  if (recording.resample) {
    // The loop takes over from what it recorded: the other tracks playing go
    // quiet where it ends (until the scene change after it, see takeResampled()).
    uint8_t others = 0;
    for (int t = 0; t < NUM_TRACKS; t++)
      if (t != recording.track && (tracks[t].live || tracks[t].armed)) others |= 1 << t;
    silenceFrom = recording.startSample + recording.targetFrames;
    silencedTracks = others;
  }
  armRecordedLoop(recording.targetFrames > length);
}

// The input just before the recording's first frame (see SampleInfo),
// copied from the input history once it's there; written with the header.
static uint32_t prerollBuf[XFADE_FRAMES];
static bool prerollTaken = false, prerollKept = false;

static void takePreroll() {
  uint32_t now = transport.now, from = recording.startSample - XFADE_FRAMES;
  if (prerollTaken || (int32_t)(now - recording.startSample) < 0) return;   // (starts a little ahead)
  prerollTaken = true;
  prerollKept = now - from < INPUT_FRAMES - AUDIO_BLOCK_SAMPLES;
  for (uint32_t i = 0; i < XFADE_FRAMES; i++) prerollBuf[i] = inputRing[(from + i) % INPUT_FRAMES];
}

static void finishRecording() {
  int t = recording.track;
  Track &tr = tracks[t];
  tr.spbNative = transport.spb;
  tr.loopBars = recording.loopBars;
  tr.recordedFrames = recording.targetFrames;
  tr.rangeStartBeat = 0;
  tr.rangeBeats = tr.loopBars * BEATS_PER_BAR;
  tr.rangeAnchorQ = 4 * recording.startBeat;
  tr.state = TrackState::Playing;
  if (recording.written > tr.recordedFrames && tr.recordedFrames % PEAK_BIN_FRAMES) {
    // Trimmed: the last bin also saw audio past the new end. Re-measure it.
    uint32_t b = tr.recordedFrames / PEAK_BIN_FRAMES, f0 = b * PEAK_BIN_FRAMES;
    uint32_t n = tr.recordedFrames - f0;
    recPeaks[b] = 0;
    if (tr.file.seekSet(WAV_DATA_OFFSET + (uint64_t)f0 * 4) && tr.file.read(chunk, n * 4) == (int)(n * 4))
      addPeaks(chunk, n, f0, recPeaks);
  }
  buildOverview(tr, recPeaks, tr.recordedFrames);
  // The sample's header: its length, tempo and overview. One 512-byte write,
  // like a unit file's (the file is cut down to size once stopped).
  // The pre-roll after the loop's frames, then the header (512 bytes each).
  takePreroll();
  uint32_t preroll = 0;
  if (prerollKept && tr.file.seekSet(WAV_DATA_OFFSET + (uint64_t)tr.recordedFrames * 4) &&
      tr.file.write(prerollBuf, sizeof prerollBuf) == (int)sizeof prerollBuf)
    preroll = XFADE_FRAMES;
  writeSampleHeader(tr.file, tr.recordedFrames, tr.spbNative, tr.loopBars, tr.overview, lastOrder + 1, preroll, tr.sample);
  prerollFrames[t] = preroll;                    // its loop's end now crossfades into it
  noteSample(tr.sample);
  lastOrder++;
  if (untrimmedCount < 64) untrimmed[untrimmedCount++] = tr.sample;   // (cut down and named once stopped)
  uint32_t end = recording.startSample + recording.targetFrames;
  int32_t endBeat = recording.startBeat + (int32_t)(recording.loopBars * BEATS_PER_BAR);
  bool resampled = recording.resample;
  if (resampled) finishedResample = t;
  recording.mode = Recording::Idle;
  recording.resample = false;
  recording.track = -1;
  trackChanged(t);                               // unit saved when the buffers allow
  if (transport.running && !tr.live && !tr.armed) startTrack(t, transport.beat + 2, false);
  if (queuedTrack >= 0) {                        // the next one takes over where this one ends
    int q = queuedTrack;
    queuedTrack = -1;
    // (From the other kind of audio, input or output: the history past this
    // one's end holds this one's kind; it starts on the next bar line.)
    if (tracks[q].sample || takeRecordingSample(q)) {
      if (queuedResample == resampled) startRecording(q, end, endBeat, INPUT_FRAMES - 2 * CHUNK_FRAMES, resampled);   // room left to catch up
      else startRecording(q, 0, 0, -1, queuedResample);
    }
  }
}

// Parts of the recording lost when the input history overflowed (the card
// fell too far behind). Skipped at first; saved as silence once the card has
// time, and before the recording is finished, so they never play back as
// whatever the file held before.
struct Gap { uint32_t from, to; };
static Gap gaps[8];
static int gapCount = 0;

static bool writeRecordingChunk(uint32_t at, uint32_t n) {
  debugStage = "SD write (recording)";
  FsFile &f = tracks[recording.track].file;
  uint32_t t0 = millis();
  // Seek every time: header writes can move the shared file position.
  bool ok = f.seekSet(WAV_DATA_OFFSET + (uint64_t)at * 4);
  int put = ok ? f.write(chunk, n * 4) : -1;
  uint32_t ms = millis() - t0;
  if (ms > SLOW_SD_MS || put != (int)(n * 4)) logSd("write", ms, put, n * 4);
  if (put != (int)(n * 4)) sdFailed = true;      // retried after recovery
  return !sdFailed;
}

static void silenceGap() {
  Gap &g = gaps[0];
  uint32_t to = recording.mode == Recording::Finishing ? min(g.to, recording.targetFrames) : g.to;   // trimmed
  if (g.from >= to) {
    memmove(gaps, gaps + 1, --gapCount * sizeof gaps[0]);
    return;
  }
  g.to = to;
  uint32_t n = min(g.to - g.from, CHUNK_FRAMES - (WAV_DATA_OFFSET / 4 + g.from) % CHUNK_FRAMES);
  memset(chunk, 0, n * 4);
  if (!writeRecordingChunk(g.from, n)) return;
  g.from += n;
  if (g.from == g.to) memmove(gaps, gaps + 1, --gapCount * sizeof gaps[0]);
  writePending = false;
}

static void serviceRecorder() {
  if (recording.mode != Recording::Running && recording.mode != Recording::Finishing) return;
  takePreroll();
  uint32_t now = transport.now;
  uint32_t next = recording.startSample + recording.written;   // next input sample to save
  if ((int32_t)(now - next) > (int32_t)(INPUT_FRAMES - CHUNK_FRAMES)) {
    recording.overruns++;                        // SD fell too far behind: skip ahead
    uint32_t lost = (now - next) - INPUT_FRAMES / 2;
    Serial.printf("recording fell behind (%s): %u ms of input lost, saved as silence\n", (const char *)debugStage,
                  (unsigned)(lost / 44));
    if (gapCount == 8) gaps[7].to = recording.written + lost;   // rarely: merged with the last one
    else gaps[gapCount++] = {recording.written, recording.written + lost};
    if (recording.mode == Recording::Running) {  // the ring's copy of the loop's beginning too
      Track &tr = tracks[recording.track];
      for (uint32_t f = recording.written; f < recording.written + lost && f < RING_FRAMES; f++) tr.ring[f] = 0;
    }
    recording.written += lost;
    next = recording.startSample + recording.written;
  }
  int32_t avail = (int32_t)(now - next);
  if (recording.mode == Recording::Finishing)
    avail = min(avail, (int32_t)(recording.targetFrames - recording.written));
  else if ((int32_t)(now - recording.startSample) >= (int32_t)maxRecordingFrames())
    stopRecording();                             // longest loop reached
  // (Signed on purpose: a recording starts LATENCY_FRAMES in the future, so for
  // a moment `now` is before startSample and avail is negative.)
  bool done = recording.mode == Recording::Finishing && recording.written >= recording.targetFrames;
  bool finalPart = recording.mode == Recording::Finishing &&
                   recording.written + avail >= recording.targetFrames;
  // Writes end on CHUNK_FRAMES boundaries of the file (the audio starts 512
  // bytes in, so the first one is shorter): each covers whole flash pages of
  // the card, which keeps its slow writes rarer.
  uint32_t toBoundary = CHUNK_FRAMES - (WAV_DATA_OFFSET / 4 + recording.written) % CHUNK_FRAMES;
  if (avail <= 0 || (avail < (int32_t)toBoundary && !finalPart)) {
    // Nothing (much) to save: time for the gaps, then it can finish.
    if (gapCount && !sdFailed && (done || streamsTopped())) silenceGap();
    else if (done && !gapCount) finishRecording();
    return;
  }
  if (sdFailed) return;                          // keep it in the input history for now
  // Write only once the playback buffers are topped up, unless the input
  // history (~740 ms) is within two chunks of overflowing.
  if (!streamsTopped() && avail < (int32_t)(INPUT_FRAMES - 2 * CHUNK_FRAMES)) {
    writePending = true;
    return;
  }
  uint32_t n = min((uint32_t)avail, toBoundary);
  for (uint32_t i = 0; i < n; i++) chunk[i] = inputRing[(next + i) % INPUT_FRAMES];
  if (!writeRecordingChunk(recording.written, n)) return;
  addPeaks(chunk, n, recording.written, recPeaks);
  // The loop's beginning also goes into the track's ring (stream frame = loop
  // frame), so playback can start the moment the recording ends, without
  // reading it back first (see armRecordedLoop).
  if (recording.mode == Recording::Running) {
    Track &tr = tracks[recording.track];
    for (uint32_t i = 0; i < n && recording.written + i < RING_FRAMES; i++) tr.ring[recording.written + i] = chunk[i];
  }
  recording.written += n;
  writePending = false;
}

static void openPoolNext() {
  if (!poolCount || (poolNext && poolNextSample == pool[0])) return;
  poolNext.close();
  char path[40];
  poolPath(pool[0], path, sizeof path);
  debugStage = "SD open (sample file)";
  poolNext = SD.sdfs.open(path, O_RDWR);
  poolNextSample = poolNext ? pool[0] : 0;
}

// A recording goes into the lowest-numbered empty sample file (made now if
// none is ready).
static bool takeRecordingSample(int t) {
  if (!poolCount && !createPoolFile()) return false;
  openPoolNext();                                // already open, unless the pool just changed
  if (!poolNext) return false;
  Track &tr = tracks[t];
  tr.file = std::move(poolNext);                 // (SdFat copies: the copy left behind is let go)
  poolNext.close();
  poolNextSample = 0;
  // Its name now (scenes refer to it while it's recorded): the project's
  // letter, the track's number, the next number for those; in its recording
  // space's file until it's named, once stopped.
  uint32_t file = pool[0];
  uint8_t tag = tagOf(recordLetter, screenTrack(t));
  tr.sample = file;
  if (tag && nextNumber[tag] <= MAX_NUMBER && unnamedCount < (int)(sizeof unnamed / sizeof unnamed[0])) {
    tr.sample = idOf(tag, umax(nextNumber[tag], 1));
    nextNumber[tag] = idNumber(tr.sample) + 1;
    unnamed[unnamedCount++] = {tr.sample, (uint16_t)file};
  }
  memmove(pool, pool + 1, --poolCount * sizeof pool[0]);
  return true;
}

// A recording cancelled before it started: its file goes back to the pool,
// still open.
static void returnRecordingSample(int t) {
  Track &tr = tracks[t];
  uint32_t file = tr.sample;
  if (int u = unnamedIndex(tr.sample); u >= 0) {   // (its number free again if it was the latest)
    file = unnamed[u].file;
    uint8_t tag = idTag(tr.sample);
    if (idNumber(tr.sample) + 1 == nextNumber[tag]) nextNumber[tag]--;
    unnamed[u] = unnamed[--unnamedCount];
  }
  if (file && poolCount < POOL_SIZE) {
    memmove(pool + 1, pool, poolCount++ * sizeof pool[0]);
    pool[0] = file;
    poolNext.close();
    poolNext = std::move(tr.file);
    poolNextSample = poolNext ? file : 0;
  }
  tr.file.close();
  tr.sample = 0;
}

int recordQueued() { return queuedTrack; }
bool recordQueuedResample() { return queuedTrack >= 0 && queuedResample; }

// Starts track t's recording (its sample file taken) at input sample `from`,
// bar line `beat`, if that is at most maxBehind samples ago (from the input
// history); otherwise at the next bar line (maxBehind < 0: always).
static void startRecording(int t, uint32_t from, int32_t beat, int32_t maxBehind, bool resample) {
  recording.track = t;
  recording.resample = resample;                 // (before it starts: the interrupt captures by it)
  recording.written = 0;
  prerollTaken = prerollKept = false;
  prerollFrames[t] = 0;                          // (none until it's saved)
  gapCount = 0;
  recording.overruns = 0;
  memset(recPeaks, 0, sizeof recPeaks);
  noInterrupts();
  if (maxBehind >= 0 && (int32_t)(transport.now - from) <= maxBehind) {
    recording.startSample = from;
    recording.startBeat = beat;
    recording.mode = Recording::Running;
  } else {
    recording.mode = Recording::WaitBar;
  }
  interrupts();
}

int takeResampled() {
  int t = finishedResample;
  finishedResample = -1;
  return t;
}

void endSilencing() { silencedTracks = 0; }
void hearAgain(int t) { __atomic_fetch_and(&silencedTracks, (uint8_t)~(1 << t), __ATOMIC_RELAXED); }   // (the interrupt clears bits too)

bool recordPress(int t, bool resample) {
  if (t == queuedTrack) {                        // pressed again: no longer queued
    queuedTrack = -1;
    returnRecordingSample(t);
    return true;
  }
  if (recording.mode != Recording::Idle && t != recording.track) {
    if (tracks[t].state != TrackState::Empty) return false;
    if (recording.mode == Recording::WaitBar) {  // not started yet: wait on this one instead
      recordPress(recording.track);
    } else {
      if (queuedTrack >= 0) returnRecordingSample(queuedTrack);   // only one queued
      queuedTrack = -1;
      // No recording space ready: it's made once this recording is saved
      // (making it stalls the card for a while).
      if (poolCount && !takeRecordingSample(t)) return false;
      if (recording.mode == Recording::Running) stopRecording();
      queuedTrack = t;
      queuedResample = resample;
      return true;
    }
  }
  switch (recording.mode) {
    case Recording::Idle: {
      if (tracks[t].state != TrackState::Empty || !takeRecordingSample(t)) return false;
      if (!transport.running) {
        startRecording(t, 0, 0, -1, resample);   // starts with the transport, on bar 1
        transportStart();
        return true;
      }
      if (resample) {                            // the next bar line (the history holds the input)
        startRecording(t, 0, 0, -1, true);
        return true;
      }
      // Pressed shortly after a bar line: start at that bar line (from the
      // input history). Otherwise wait for the next one.
      noInterrupts();
      uint32_t lastBar = transport.lastBarSample;
      int32_t lastBeat = transport.lastBarBeat;
      interrupts();
      startRecording(t, lastBar + LATENCY_FRAMES, lastBeat, PREROLL_FRAMES + LATENCY_FRAMES);
      return true;
    }
    case Recording::WaitBar:                     // cancel before it started
      recording.mode = Recording::Idle;
      recording.resample = false;
      returnRecordingSample(recording.track);
      recording.track = -1;
      return true;
    case Recording::Running:
      stopRecording();
      return true;
    default:
      return true;                               // already finishing
  }
}

bool cancelRecording(int t) {
  if (t == queuedTrack) return recordPress(t);   // queued: just not any more
  if (recording.mode == Recording::Idle || recording.track != t) return false;
  noInterrupts();
  recording.mode = Recording::Idle;              // nothing more captured or written
  interrupts();
  recording.resample = false;
  silencedTracks = 0;                            // (a resampling's: the others heard again)
  recording.track = -1;
  gapCount = 0;
  writePending = false;
  // Finishing, its loop may already play: faded out, the track empty again.
  Track &tr = tracks[t];
  dropPlan(t);
  changes[t].on = false;
  silenceTracks(1 << t);
  tr.live = false;
  tr.armed = false;
  tr.state = TrackState::Empty;
  memset(tr.overview, 0, sizeof tr.overview);
  tr.overviewVersion++;
  returnRecordingSample(t);                      // its file back to the pool: the recording is gone
  trackChanged(t);
  if (queuedTrack >= 0) {                        // the next one: from the next bar line
    int q = queuedTrack;
    queuedTrack = -1;
    if (tracks[q].sample || takeRecordingSample(q)) startRecording(q, 0, 0, -1, queuedResample);
  }
  return true;
}

void clearTrack(int t) {
  if (recordingTrack(t)) return;
  Track &tr = tracks[t];
  dropPlan(t);
  changes[t].on = false;
  silenceTracks(1 << t);
  tr.state = TrackState::Empty;
  tr.file.close();                               // its sample stays on the card
  tr.sample = 0;
  setTrackFilter(t, 0, FILTER_MAX);                // flat again
  setTrackMix(t, VOLUME_UNITY, 0);
  memset(tr.overview, 0, sizeof tr.overview);
  tr.overviewVersion++;
  trackChanged(t);
}

void trackRange(int t, uint32_t &startBeat, uint32_t &beats) {
  const Track &tr = tracks[t];
  startBeat = tr.rangePending ? tr.pendingStartBeat : tr.rangeStartBeat;
  beats = tr.rangePending ? tr.pendingBeats : tr.rangeBeats;
}

uint32_t startPosition(uint32_t loopBeats, uint32_t rangeStart, uint32_t rangeBeats) {
  // The grid to snap to: beats, half bars or bars. A range always holds one
  // of its points (it's at least as long as the grid step: 1 beat, 2-3 beats,
  // a bar or more); it may wrap past the loop's end, where the loop (and a
  // bar) starts again. Loops whose range moves a beat at a time (up to
  // BEAT_STEP_MAX_BARS): the first half bar or bar line, whichever comes first.
  bool beatSteps = loopBeats <= BEAT_STEP_MAX_BARS * BEATS_PER_BAR;
  uint32_t step = rangeBeats <= 1 ? 1 : rangeBeats < (uint32_t)BEATS_PER_BAR || beatSteps ? BEATS_PER_BAR / 2 : BEATS_PER_BAR;
  uint32_t first = (rangeStart + step - 1) / step * step;   // first grid point at or after the range start
  return first % loopBeats;
}

bool setRange(int t, uint32_t startBeat, uint32_t beats) {
  Track &tr = tracks[t];
  if (recording.mode != Recording::Idle && recording.track == t) return false;   // still finishing
  if (changes[t].on || plans[t].on || tr.sampleSwitch) return false;   // a change on its way (a moment)
  if (tr.state == TrackState::Empty || beats < 1 ||
      startBeat >= tr.loopBars * BEATS_PER_BAR || beats > tr.loopBars * BEATS_PER_BAR)
    return false;
  if (!tr.live) {                                // not playing: just change it
    tr.rangePending = false;
    tr.rangeStartBeat = startBeat;
    tr.rangeBeats = beats;
    trackChanged(t);
    if (tr.armed) startTrack(t, tr.startBeat, false);   // joining soon: refill for the new range
    return true;
  }
  // Playing: queue it. The switch goes on the first quarter beat that the
  // stream hasn't buffered yet, so the old range plays right up to it with no
  // gap. Stream frame of transport quarter q: s0 + frames since startBeat.
  noInterrupts();
  int32_t q = transport.beat * 4 + (int32_t)((uint64_t)transport.sampleInBeat * 4 / transport.spb) + 1;
  interrupts();
  auto streamAt = [&](int32_t qq) { return tr.s0 + quarterFrames(qq - 4 * tr.startBeat, tr.spbNative); };
  while ((int32_t)(streamAt(q) - tr.head) < 0) q++;
  tr.pendingStartBeat = startBeat;
  tr.pendingBeats = beats;
  tr.switchQ = q;
  tr.switchS = streamAt(q);
  tr.rangePending = true;
  return true;
}

// ---- Transport
void transportStart() {
  if (transport.running) return;
  clockOrigin = 0;
  clockOriginNext = -1;
  transport.beat = 0;
  transport.sampleInBeat = 0;
  for (int t = 0; t < NUM_TRACKS; t++) {
    // Each track plays from its start position at 1.1.1 (beat 0).
    Track &tr = tracks[t];
    if (tr.state != TrackState::Empty) {
      uint32_t rs, rb;
      trackRange(t, rs, rb);                     // a queued range: startTrack() takes it now
      tr.rangeAnchorQ = -4 * (int32_t)(startPosition(tr.loopBars * BEATS_PER_BAR, rs, rb) - rs);
    }
    startTrack(t, 0, true);
  }
  midiSendNow(0xFA);                             // MIDI Start
  transport.running = true;
}

void transportStop() {
  if (queuedTrack >= 0) returnRecordingSample(queuedTrack);
  queuedTrack = -1;
  if (recording.mode == Recording::WaitBar) recordPress(recording.track);   // cancel
  if (recording.mode == Recording::Running) stopRecording();
  if (recording.mode == Recording::Finishing) {
    // Stopped while carrying on to the loop length: keep what's recorded so
    // far; the rest of the loop plays as silence.
    uint32_t sofar = transport.now + recordingLatency() - recording.startSample;
    recording.targetFrames = min(recording.targetFrames, max(sofar, recording.written));
  }
  silenceTracks(0xFF);                           // faded out, not cut off
  silencedTracks = 0;
  transport.running = false;
  transport.spbAtBeat = -1;                      // a scene's tempo: now
  for (int t = 0; t < NUM_TRACKS; t++) {
    Track &tr = tracks[t];
    tr.live = false;
    tr.armed = false;
    settleNow(t);                                // changes on their way: the tracks' now
    dropPlan(t);
  }
  midiSendNow(0xFC);                             // MIDI Stop
}

void transportSnapToBar() {
  if (!transport.running || recording.mode != Recording::Idle) return;   // a recording keeps its timing
  noInterrupts();
  int32_t beat = transport.beat;
  uint32_t into = (beat % BEATS_PER_BAR) * transport.spb + transport.sampleInBeat;   // samples into the bar
  uint32_t barLen = BEATS_PER_BAR * transport.spb;
  interrupts();
  int32_t target = (beat / BEATS_PER_BAR + (into >= barLen / 2 ? 1 : 0)) * BEATS_PER_BAR;
  for (int t = 0; t < NUM_TRACKS; t++) settleNow(t);   // changes on their way: now
  silenceTracks(0xFF);                           // faded out, then in again at the new position
  // Paused while the tracks refill for the new position (as when starting).
  transport.running = false;
  transport.beat = target;
  transport.sampleInBeat = 0;
  for (int t = 0; t < NUM_TRACKS; t++) startTrack(t, target, true);
  transport.running = true;
}

void transportRestart() {
  transportStop();                               // ends a recording as stopping would
  transportStart();
}

void flushHeaders() {
  while (headerDirty) {
    int t = __builtin_ctz(headerDirty);
    headerDirty &= ~(1 << t);
    writeUnit(t);
  }
}

// While stopped (and not recording): cut finished samples down to size, then
// top the pool of recording space up. One file per call.
static void serviceFiles() {
  if (transport.running || recording.mode != Recording::Idle || sdFailed) return;
  if (untrimmedCount) trimSample(untrimmed[--untrimmedCount]);
  else if (poolCount < POOL_SIZE && !poolBlocked) createPoolFile();
  else openPoolNext();
}

void serviceStorage() {
  if (sdFailed) recoverSd();
  serviceFiles();
  serviceChanges();
  servicePrepare();
  serviceRecorder();
  for (int t = 0; t < NUM_TRACKS; t++)
    if (tracks[t].needsRestart && transport.running) startTrack(t, transport.beat + 2, false);
  serviceStreams();
  serviceRecorder();
  serviceHeaders();
}

}  // namespace looper
