// storage.cpp — see storage.h
//
// Files, in /looper:
//   samples/0001.wav ...  every recording, numbered in order (up to 9999) and
//                         kept when tracks stop using it. 16-bit stereo WAV
//                         whose 512-byte header carries, in its JUNK chunk,
//                         the loop's length and tempo and its waveform overview.
//   track1.unit ...       each track: the sample it plays and its settings
//                         (loop range, mute, filter, volume and balance). 512 bytes.
//   session.txt           the tempo.
// Older versions kept each track in a full-size track1.wav ... : at boot those
// become samples (and the empty ones recording space).
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
#include <inttypes.h>

namespace looper {

volatile const char *debugStage = "boot";
static uint32_t chunk[CHUNK_FRAMES];
static uint8_t headerDirty = 0;                  // bit t: track t's unit file needs saving
// A write is waiting for the playback buffers to be topped up (see serviceStreams).
static bool writePending = false;
static constexpr uint64_t SAMPLE_FILE_BYTES = WAV_DATA_OFFSET + (uint64_t)MAX_REC_FRAMES * 4;
static constexpr int POOL_SIZE = 4;              // empty sample files kept ready for recordings
static constexpr uint32_t MAX_SAMPLES = 9999;

static void samplePath(uint32_t n, char *buf, size_t size) { snprintf(buf, size, "/looper/samples/%04u.wav", (unsigned)n); }
static void unitPath(int t, char *buf, size_t size) { snprintf(buf, size, "/looper/track%d.unit", t + 1); }
static void legacyPath(int t, char *buf, size_t size) { snprintf(buf, size, "/looper/track%d.wav", t + 1); }

static int32_t posMod(int32_t a, int32_t m) { int32_t r = a % m; return r < 0 ? r + m : r; }

// Frames in q quarter beats (a beat's samples rarely divide by 4: rounded down
// within each beat).
static uint32_t quarterFrames(uint32_t q, uint32_t spb) { return (q / 4) * spb + (q % 4) * spb / 4; }

static bool streamsTopped();
static void startTrack(int t, int32_t beat0, bool fillNow);
static void startRecording(int t, uint32_t from, int32_t beat, int32_t maxBehind);

// A track to record next: pressed while another one was recording. Its
// sample file is opened right away (no SD work as it starts); it starts where
// that one ends, with no gap between them.
static int queuedTrack = -1;

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
};
// Version 2: order. Version 1 samples are ordered by their number.
static constexpr uint32_t INFO_OFFSET = 44, SAMPLE_VERSION = 2;
static constexpr uint32_t SAMPLE_OVERVIEW_OFFSET = INFO_OFFSET + sizeof(SampleInfo);
static_assert(SAMPLE_OVERVIEW_OFFSET + OVERVIEW_COLS <= WAV_DATA_OFFSET - 8, "the overview must fit in the header");

// The samples holding recordings, by number: the order they were recorded in
// (0: none). Order numbers stay below 65536: there are at most 9999 samples,
// and new ones count on from the highest. In RAM2 (20 KB).
DMAMEM static uint16_t sampleOrder[MAX_SAMPLES + 1];
static uint32_t sampleTotal = 0, lastOrder = 0;
static uint32_t nextSample = 1;                  // number for the next new file
static uint16_t pool[POOL_SIZE];                 // empty sample files, lowest number first
static int poolCount = 0;
static uint16_t untrimmed[64];                   // finished samples still at full size
static int untrimmedCount = 0;

static void noteSample(uint32_t n, uint32_t order) {
  if (n < 1 || n > MAX_SAMPLES || !order || order > 0xFFFF) return;
  if (!sampleOrder[n]) sampleTotal++;
  sampleOrder[n] = order;
  lastOrder = max(lastOrder, order);
}
uint32_t sampleCount() { return sampleTotal; }
bool sampleExists(uint32_t n) { return n >= 1 && n <= MAX_SAMPLES && sampleOrder[n]; }

// The sample recorded just after (dir 1) or before (-1) the one with order
// `from`, wrapping around; from 0 going forward: the oldest; from 0xFFFF+1
// going back: the newest. 0 if there's none other.
static uint32_t neighbourSample(uint32_t from, int dir) {
  uint32_t best = 0, wrap = 0;                   // nearest that way; furthest the other way (to wrap to)
  for (uint32_t n = 1; n <= MAX_SAMPLES; n++) {
    uint32_t o = sampleOrder[n];
    if (!o || o == from) continue;
    bool ahead = dir > 0 ? o > from : o < from;
    if (ahead && (!best || (dir > 0 ? o < sampleOrder[best] : o > sampleOrder[best]))) best = n;
    if (!wrap || (dir > 0 ? o < sampleOrder[wrap] : o > sampleOrder[wrap])) wrap = n;
  }
  return best ? best : wrap;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static bool writeSampleHeader(FsFile &f, uint32_t frames, uint32_t spb, uint32_t loopBars, const uint8_t *overview,
                              uint32_t order = 0) {
  uint8_t h[WAV_DATA_OFFSET] = {};
  memcpy(h, "RIFF", 4);       put32(h + 4, WAV_DATA_OFFSET - 8 + frames * 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);  put32(h + 16, 16);
  put16(h + 20, 1);           put16(h + 22, 2);           // PCM, stereo
  put32(h + 24, 44100);       put32(h + 28, 44100 * 4);
  put16(h + 32, 4);           put16(h + 34, 16);
  memcpy(h + 36, "JUNK", 4);  put32(h + 40, WAV_DATA_OFFSET - 44 - 8);
  SampleInfo info = {{'M', '8', 'S', 'M'}, SAMPLE_VERSION, frames, spb, loopBars, order};
  memcpy(h + INFO_OFFSET, &info, sizeof info);
  if (overview) memcpy(h + SAMPLE_OVERVIEW_OFFSET, overview, OVERVIEW_COLS);
  memcpy(h + WAV_DATA_OFFSET - 8, "data", 4);
  put32(h + WAV_DATA_OFFSET - 4, frames * 4);
  debugStage = "SD write (sample header)";
  bool ok = f.seekSet(0) && f.write(h, sizeof h) == (int)sizeof h;
  // Also pushes out the end of a recording still held in SdFat's sector cache,
  // so it survives power-off (the file's size doesn't change: no FS update).
  return f.sync() && ok;
}

static bool readSampleHeader(FsFile &f, SampleInfo &info, uint8_t *overview) {
  uint8_t h[WAV_DATA_OFFSET];
  if (!f.seekSet(0) || f.read(h, sizeof h) != (int)sizeof h) return false;
  memcpy(&info, h + INFO_OFFSET, sizeof info);
  if (memcmp(h, "RIFF", 4) || memcmp(info.magic, "M8SM", 4) || info.version < 1) return false;
  if (overview) memcpy(overview, h + SAMPLE_OVERVIEW_OFFSET, OVERVIEW_COLS);
  return true;
}

// A sample that can play: a recording of a sane length and tempo.
static bool playable(const SampleInfo &info) {
  return info.frames && info.frames <= MAX_REC_FRAMES && info.spb && info.loopBars && info.loopBars <= MAX_BARS;
}

// A new empty sample file at full size, for the pool (changes the file system).
static bool createPoolFile() {
  if (poolCount >= POOL_SIZE || nextSample > MAX_SAMPLES) return false;
  char path[40];
  samplePath(nextSample, path, sizeof path);
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
    return false;
  }
  pool[poolCount++] = nextSample++;
  return true;
}

// Cuts a finished sample down to its recording (changes the file system).
static void trimSample(uint32_t n) {
  char path[40];
  samplePath(n, path, sizeof path);
  FsFile f = SD.sdfs.open(path, O_RDWR);
  SampleInfo info;
  if (f && readSampleHeader(f, info, nullptr) && playable(info)) {
    debugStage = "SD trim (sample file)";
    f.truncate(WAV_DATA_OFFSET + (uint64_t)info.frames * 4);
  }
}

// ---- Tracks and their unit files
static constexpr uint32_t UNIT_VERSION = 1;

static TrackUnit clipboard;                       // sample 0: empty
static bool applyingUnit = false;                // setTrackUnit() at work: not a change of the scene

TrackUnit unitOf(int t) {
  const Track &tr = tracks[t];
  TrackUnit u = {{'M', '8', 'T', 'U'}, UNIT_VERSION, 0, 0, 0, 0, 0, tr.filterLo, tr.filterHi, tr.volume, tr.balance};
  if (tr.state == TrackState::Empty) return u;
  u.sample = tr.sample;
  trackRange(t, u.rangeStart, u.rangeLen);       // a queued range counts
  u.rangePhase = (uint32_t)posMod(-tr.rangeAnchorQ, 4 * u.rangeLen);
  u.muted = tr.state == TrackState::Muted;
  return u;
}

// Opens sample n for track t and takes on its recording (length, tempo,
// overview). The track's settings are left to the caller.
static bool loadSample(int t, uint32_t n) {
  Track &tr = tracks[t];
  char path[40];
  samplePath(n, path, sizeof path);
  tr.file = SD.sdfs.open(path, O_RDWR);
  SampleInfo info;
  if (!tr.file || !readSampleHeader(tr.file, info, tr.overview) || !playable(info)) {
    tr.file.close();
    return false;
  }
  tr.sample = n;
  tr.spbNative = info.spb;
  tr.loopBars = info.loopBars;
  tr.recordedFrames = info.frames;
  tr.overviewVersion++;
  return true;
}

// Takes on a unit's settings (its sample already loaded).
static void applyUnit(int t, const TrackUnit &u) {
  Track &tr = tracks[t];
  uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
  uint32_t len = u.rangeLen && u.rangeLen <= loopBeats ? u.rangeLen : loopBeats;
  tr.rangePending = false;
  tr.rangeStartBeat = u.rangeStart < loopBeats ? u.rangeStart : 0;
  tr.rangeBeats = len;
  tr.rangeAnchorQ = -(int32_t)(u.rangePhase % (4 * len));
  setTrackFilter(t, u.filterLo, u.filterHi);
  setTrackMix(t, u.volume, u.balance);
  tr.state = u.muted ? TrackState::Muted : TrackState::Playing;
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
  setTrackFilter(t, u.filterLo, u.filterHi);
  setTrackMix(t, u.volume, u.balance);
  if (u.sample && loadSample(t, u.sample)) applyUnit(t, u);
}

void trackChanged(int t) {
  headerDirty |= 1 << t;
  if (!applyingUnit) sceneStateChanged();        // the scene follows (not while it's being loaded)
}

bool storageMayWrite() {
  if (!transport.running || streamsTopped()) return true;
  writePending = true;
  return false;
}

bool setTrackUnit(int t, const TrackUnit &u) {
  Track &tr = tracks[t];
  if (recording.mode != Recording::Idle && recording.track == t) return false;
  TrackUnit now = unitOf(t);
  if (!memcmp(&now, &u, sizeof u)) return true;  // nothing to do
  applyingUnit = true;
  bool sameClip = u.sample && now.sample == u.sample && now.rangeStart == u.rangeStart &&
                  now.rangeLen == u.rangeLen && now.rangePhase == u.rangePhase;
  bool ok = true;
  if (sameClip) {                                // keeps playing: just its settings
    setTrackFilter(t, u.filterLo, u.filterHi);
    setTrackMix(t, u.volume, u.balance);
    tr.state = u.muted ? TrackState::Muted : TrackState::Playing;
    trackChanged(t);
  } else if (!u.sample) {
    clearTrack(t);
    setTrackFilter(t, u.filterLo, u.filterHi);
    setTrackMix(t, u.volume, u.balance);
  } else {
    tr.live = false;
    tr.armed = false;
    tr.rangePending = false;
    if (now.sample != u.sample) {
      tr.file.close();
      ok = loadSample(t, u.sample);
    }
    if (ok) {
      applyUnit(t, u);
      if (transport.running) startTrack(t, transport.beat + 2, false);
      trackChanged(t);
    } else {
      clearTrack(t);                             // its sample is gone
    }
  }
  applyingUnit = false;
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
    // The bars started so far.
    uint32_t sofar = max<int32_t>(0, (int32_t)(transport.now - recording.startSample));
    bars = constrain((sofar + framesPerBar - 1) / framesPerBar, 1u, MAX_BARS);
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
    bool space = f.fileSize() == SAMPLE_FILE_BYTES && poolCount < POOL_SIZE && nextSample <= MAX_SAMPLES;
    if (space) writeSampleHeader(f, 0, 0, 0, nullptr);
    f.close();
    samplePath(nextSample, to, sizeof to);
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
  if (info.version >= 9) { u.volume = (int8_t)h[MIX_AT]; u.balance = (int8_t)h[MIX_AT + 1]; }
  writeSampleHeader(f, info.recordedFrames, info.spb, info.loopBars, tr.overview, lastOrder + 1);
  f.truncate(WAV_DATA_OFFSET + (uint64_t)info.recordedFrames * 4);
  f.close();
  samplePath(nextSample, to, sizeof to);
  if (!SD.sdfs.rename(from, to)) return;
  noteSample(nextSample++, lastOrder + 1);
  tr.file.close();
  if (loadSample(t, u.sample)) applyUnit(t, u);
  writeUnit(t);
}

// ---- Clipboard
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
  Track &tr = tracks[t];
  if (sampleTotal < 2 || recordingTrack(t)) return false;
  bool empty = tr.state == TrackState::Empty;
  // In recording order from the track's sample, wrapping around; an empty
  // track starts from the oldest (going forward) or newest (going back).
  uint32_t from = empty ? (dir > 0 ? 0 : 0x10000) : sampleOrder[tr.sample];
  uint32_t n = neighbourSample(from, dir);
  if (!n || n == tr.sample) return false;
  TrackState state = empty ? TrackState::Playing : tr.state;
  tr.live = false;
  tr.armed = false;
  tr.rangePending = false;
  tr.file.close();
  if (!loadSample(t, n)) {                       // unreadable: the track ends up empty
    tr.state = TrackState::Empty;
    tr.sample = 0;
    trackChanged(t);
    return false;
  }
  // Volume, balance, filter and mute stay; the range is the whole loop again,
  // aligned to the bar counter (its start on 1.1.1).
  tr.rangeStartBeat = 0;
  tr.rangeBeats = tr.loopBars * BEATS_PER_BAR;
  tr.rangeAnchorQ = 0;
  tr.state = state;
  trackChanged(t);
  if (transport.running) startTrack(t, transport.beat + 2, false);
  return true;
}

float loadTempo() {
  FsFile f = SD.sdfs.open("/looper/session.txt", O_RDONLY);
  float bpm = 120.0f;
  char line[32];
  if (f) {
    if (f.fgets(line, sizeof line) > 0) sscanf(line, "bpm=%f", &bpm);
    f.close();
  }
  return constrain(bpm, MIN_BPM, MAX_BPM);
}

void saveTempo(float bpm) {
  FsFile f = SD.sdfs.open("/looper/session.txt", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  f.printf("bpm=%.2f\n", bpm);
  f.close();
}

// Finds the samples: which hold recordings (cut down to size if needed),
// which are recording space, and the next free number.
static void scanSamples() {
  memset(sampleOrder, 0, sizeof sampleOrder);
  sampleTotal = 0;
  lastOrder = 0;
  poolCount = 0;
  untrimmedCount = 0;
  FsFile dir = SD.sdfs.open("/looper/samples", O_RDONLY);
  FsFile f;
  char name[32];
  while (dir && f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof name);
    unsigned n = 0;
    bool numbered = !f.isDir() && sscanf(name, "%4u.wav", &n) == 1 && n >= 1 && n <= MAX_SAMPLES;
    SampleInfo info;
    bool ok = numbered && readSampleHeader(f, info, nullptr);
    uint64_t size = f.fileSize();
    f.close();
    if (!numbered) continue;
    if (n >= nextSample) nextSample = n + 1;
    if (ok && playable(info)) {
      noteSample(n, info.version >= 2 && info.order ? info.order : n);
      if (size > WAV_DATA_OFFSET + (uint64_t)info.frames * 4 && untrimmedCount < 64) untrimmed[untrimmedCount++] = n;
    } else if (ok && info.frames == 0 && size == SAMPLE_FILE_BYTES && poolCount < POOL_SIZE) {
      int i = poolCount++;                       // keep the pool sorted
      while (i > 0 && pool[i - 1] > n) { pool[i] = pool[i - 1]; i--; }
      pool[i] = n;
    }
  }
}

// Recording space numbered below a recording (left from converting an older
// version's track files) is renamed to the next free numbers, so new
// recordings are numbered in the order they're made.
static void renumberPool() {
  uint32_t highest = 0;
  for (uint32_t n = 1; n <= MAX_SAMPLES; n++) if (sampleOrder[n]) highest = n;
  for (int i = 0; i < poolCount; i++) {
    if (pool[i] > highest || nextSample > MAX_SAMPLES) continue;
    char from[40], to[40];
    samplePath(pool[i], from, sizeof from);
    samplePath(nextSample, to, sizeof to);
    if (SD.sdfs.rename(from, to)) pool[i] = nextSample++;
  }
  for (int i = 1; i < poolCount; i++)            // keep it sorted
    for (int j = i; j > 0 && pool[j - 1] > pool[j]; j--) { uint16_t x = pool[j]; pool[j] = pool[j - 1]; pool[j - 1] = x; }
}

bool storageBegin(void (*progress)(const char *)) {
  // DMA mode: every read/write is one self-contained transfer done by the SD
  // controller. The default FIFO mode leaves a multi-sector transfer open
  // between calls and has the CPU poll the FIFO; on the M8 it stalled for
  // seconds at a time (1 s timeouts, retried) with the audio engine running.
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) return false;
  SD.sdfs.mkdir("/looper");
  SD.sdfs.mkdir("/looper/samples");
  scanSamples();
  while (untrimmedCount) trimSample(untrimmed[--untrimmedCount]);
  bool ok = true;
  for (int t = 0; t < NUM_TRACKS; t++) {
    char path[40];
    snprintf(path, sizeof path, "/looper/track%d.txt", t + 1);
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
  while (poolCount < POOL_SIZE) {
    char msg[64];
    snprintf(msg, sizeof msg, "preparing recording space (%d of %d, once)...", poolCount + 1, POOL_SIZE);
    progress(msg);
    if (!createPoolFile()) break;
  }
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

// The stream has reached the queued range's switch point. If the new range
// contains the spot that would have played next, carry on from it (the range
// is re-anchored around the playhead); otherwise start the new range in
// phase with the existing anchor.
static void applyQueuedRange(Track &tr) {
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
  uint32_t free = RING_FRAMES - (tr.head - tr.tail);
  // Up to the range's end or the loop's end, whichever comes first (a range
  // can run past the loop's end and carry on from its start).
  uint32_t toRangeEnd = tr.rangeFrames() - tr.rangeOffset(tr.fileFrame);
  uint32_t n = min(min(CHUNK_FRAMES, free), min(toRangeEnd, tr.loopFrames() - tr.fileFrame));
  if (tr.rangePending) n = min(n, tr.switchS - tr.head);      // stop exactly at the switch
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
  tr.live = false;
  tr.armed = false;
  tr.needsRestart = false;
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
  tr.armed = true;
}

// A write is waiting (recording or a header): top the buffers up with
// half-chunk reads too, so the write starts with as much playback in reserve
// as possible. Otherwise read whole chunks only (cheaper per byte).
static uint32_t refillThreshold() { return writePending ? CHUNK_FRAMES / 2 : CHUNK_FRAMES; }

static void serviceStreams() {
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
  tr.armed = true;
}

static void stopRecording() {
  noInterrupts();
  uint32_t stopSample = transport.now + LATENCY_FRAMES;
  uint32_t spb = transport.spb;
  interrupts();
  uint32_t length = min(stopSample - recording.startSample, maxRecordingFrames());
  recording.loopBars = targetBars((double)length / spb);
  // Shorter than `length`: the end is trimmed. Longer: recording continues
  // until it is reached (serviceRecorder keeps writing the input until then).
  recording.targetFrames = min(recording.loopBars * BEATS_PER_BAR * spb, maxRecordingFrames());
  recording.mode = Recording::Finishing;
  armRecordedLoop(recording.targetFrames > length);
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
  writeSampleHeader(tr.file, tr.recordedFrames, tr.spbNative, tr.loopBars, tr.overview, lastOrder + 1);
  noteSample(tr.sample, lastOrder + 1);
  if (untrimmedCount < 64) untrimmed[untrimmedCount++] = tr.sample;
  uint32_t end = recording.startSample + recording.targetFrames;
  int32_t endBeat = recording.startBeat + (int32_t)(recording.loopBars * BEATS_PER_BAR);
  recording.mode = Recording::Idle;
  recording.track = -1;
  trackChanged(t);                               // unit saved when the buffers allow
  if (transport.running && !tr.live && !tr.armed) startTrack(t, transport.beat + 2, false);
  if (queuedTrack >= 0) {                        // the next one takes over where this one ends
    int q = queuedTrack;
    queuedTrack = -1;
    startRecording(q, end, endBeat, INPUT_FRAMES - 2 * CHUNK_FRAMES);   // room left to catch up
  }
}

static void serviceRecorder() {
  if (recording.mode != Recording::Running && recording.mode != Recording::Finishing) return;
  uint32_t now = transport.now;
  uint32_t next = recording.startSample + recording.written;   // next input sample to save
  if ((int32_t)(now - next) > (int32_t)(INPUT_FRAMES - CHUNK_FRAMES)) {
    recording.overruns++;                        // SD fell too far behind: skip ahead
    recording.written += (now - next) - INPUT_FRAMES / 2;
    next = recording.startSample + recording.written;
  }
  int32_t avail = (int32_t)(now - next);
  if (recording.mode == Recording::Finishing)
    avail = min(avail, (int32_t)(recording.targetFrames - recording.written));
  else if ((int32_t)(now - recording.startSample) >= (int32_t)maxRecordingFrames())
    stopRecording();                             // longest loop reached
  // (Signed on purpose: a recording starts LATENCY_FRAMES in the future, so for
  // a moment `now` is before startSample and avail is negative.)
  if (avail <= 0) {
    if (recording.mode == Recording::Finishing && recording.written >= recording.targetFrames)
      finishRecording();
    return;
  }
  bool finalPart = recording.mode == Recording::Finishing &&
                   recording.written + avail >= recording.targetFrames;
  if (avail < (int32_t)CHUNK_FRAMES && !finalPart) return;
  if (sdFailed) return;                          // keep it in the input history for now
  // Write only once the playback buffers are topped up, unless the input
  // history (~740 ms) is within two chunks of overflowing.
  if (!streamsTopped() && avail < (int32_t)(INPUT_FRAMES - 2 * CHUNK_FRAMES)) {
    writePending = true;
    return;
  }
  uint32_t n = min((uint32_t)avail, CHUNK_FRAMES);
  for (uint32_t i = 0; i < n; i++) chunk[i] = inputRing[(next + i) % INPUT_FRAMES];
  debugStage = "SD write (recording)";
  FsFile &f = tracks[recording.track].file;
  uint32_t t0 = millis();
  // Seek every time: header writes can move the shared file position.
  bool ok = f.seekSet(WAV_DATA_OFFSET + (uint64_t)recording.written * 4);
  int put = ok ? f.write(chunk, n * 4) : -1;
  uint32_t ms = millis() - t0;
  if (ms > SLOW_SD_MS || put != (int)(n * 4)) logSd("write", ms, put, n * 4);
  if (put != (int)(n * 4)) {                     // retried after recovery
    sdFailed = true;
    return;
  }
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

// A recording goes into the lowest-numbered empty sample file (made now if
// none is ready).
static bool takeRecordingSample(int t) {
  if (!poolCount && !createPoolFile()) return false;
  char path[40];
  samplePath(pool[0], path, sizeof path);
  Track &tr = tracks[t];
  tr.file = SD.sdfs.open(path, O_RDWR);
  if (!tr.file) return false;
  tr.sample = pool[0];
  memmove(pool, pool + 1, --poolCount * sizeof pool[0]);
  return true;
}

// A recording cancelled before it started: its file goes back to the pool.
static void returnRecordingSample(int t) {
  Track &tr = tracks[t];
  tr.file.close();
  if (tr.sample && poolCount < POOL_SIZE) {
    memmove(pool + 1, pool, poolCount++ * sizeof pool[0]);
    pool[0] = tr.sample;
  }
  tr.sample = 0;
}

int recordQueued() { return queuedTrack; }

// Starts track t's recording (its sample file taken) at input sample `from`,
// bar line `beat`, if that is at most maxBehind samples ago (from the input
// history); otherwise at the next bar line (maxBehind < 0: always).
static void startRecording(int t, uint32_t from, int32_t beat, int32_t maxBehind) {
  recording.track = t;
  recording.written = 0;
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

bool recordPress(int t) {
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
      if (!takeRecordingSample(t)) return false;
      if (recording.mode == Recording::Running) stopRecording();
      queuedTrack = t;
      return true;
    }
  }
  switch (recording.mode) {
    case Recording::Idle: {
      if (tracks[t].state != TrackState::Empty || !takeRecordingSample(t)) return false;
      if (!transport.running) {
        startRecording(t, 0, 0, -1);             // starts with the transport, on bar 1
        transportStart();
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

void clearTrack(int t) {
  if (recordingTrack(t)) return;
  Track &tr = tracks[t];
  tr.live = false;
  tr.armed = false;
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
  // of its points (ranges are powers of two beats long); it may wrap past the
  // loop's end, where the loop (and a bar) starts again.
  uint32_t step = rangeBeats <= 1 ? 1 : rangeBeats < (uint32_t)BEATS_PER_BAR ? BEATS_PER_BAR / 2 : BEATS_PER_BAR;
  uint32_t first = (rangeStart + step - 1) / step * step;   // first grid point at or after the range start
  return first % loopBeats;
}

bool setRange(int t, uint32_t startBeat, uint32_t beats) {
  Track &tr = tracks[t];
  if (recording.mode != Recording::Idle && recording.track == t) return false;   // still finishing
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
    uint32_t sofar = transport.now + LATENCY_FRAMES - recording.startSample;
    recording.targetFrames = min(recording.targetFrames, max(sofar, recording.written));
  }
  transport.running = false;
  for (auto &tr : tracks) { tr.live = false; tr.armed = false; }
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
  // Paused while the tracks refill for the new position (as when starting).
  transport.running = false;
  for (auto &tr : tracks) { tr.live = false; tr.armed = false; }
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
  else if (poolCount < POOL_SIZE) createPoolFile();
}

void serviceStorage() {
  if (sdFailed) recoverSd();
  serviceFiles();
  serviceRecorder();
  for (int t = 0; t < NUM_TRACKS; t++)
    if (tracks[t].needsRestart && transport.running) startTrack(t, transport.beat + 2, false);
  serviceStreams();
  serviceRecorder();
  serviceHeaders();
}

}  // namespace looper
