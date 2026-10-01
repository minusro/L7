// storage.cpp — see storage.h
//
// Rule: nothing that changes the card's file system (create, resize, delete,
// directory updates) happens while the transport runs. On a slow card those
// take hundreds of ms to seconds and starve the track streams. So:
//   * every track has a full-size WAV file, created once at boot; recording
//     only overwrites its audio, clearing only marks it empty
//   * a track's settings live in its WAV header (inside the JUNK chunk), so
//     saving them is a single 512-byte write, done when the buffers are full
//   * tracks joining mid-play fill their buffer through the normal streaming
//     instead of a blocking burst of reads
#include "storage.h"
#include <inttypes.h>

namespace looper {

volatile const char *debugStage = "boot";
static uint32_t chunk[CHUNK_FRAMES];
static uint8_t headerDirty = 0;                  // bit t: track t's header needs saving
// A write is waiting for the playback buffers to be topped up (see serviceStreams).
static bool writePending = false;
static constexpr uint64_t TRACK_FILE_BYTES = WAV_DATA_OFFSET + (uint64_t)MAX_REC_FRAMES * 4;

static void trackPath(int t, char *buf, size_t n) { snprintf(buf, n, "/looper/track%d.wav", t + 1); }

static int32_t posMod(int32_t a, int32_t m) { int32_t r = a % m; return r < 0 ? r + m : r; }

// Frames in q quarter beats (a beat's samples rarely divide by 4: rounded down
// within each beat).
static uint32_t quarterFrames(uint32_t q, uint32_t spb) { return (q / 4) * spb + (q % 4) * spb / 4; }

// ---- WAV header with the track's settings
// 16-bit stereo PCM; a JUNK chunk pads the header to 512 bytes so the audio is
// sector-aligned, and carries the settings below at offset 44.
struct TrackInfo {
  char magic[4];                                 // "M8LP"
  uint32_t version, used, spb, loopBars, recordedFrames, rangeStart, rangeLen, muted;
  uint32_t rangePhase;                           // position in the range at transport beat 0
};
// Version 5: waveform overview (OVERVIEW_COLS bytes) right after TrackInfo.
// Version 4: rangePhase in quarter beats. Version 3: rangePhase in beats.
// Version 2: no rangePhase. Version 1: range in bars. Older ones are converted
// (their overview is worked out at boot).
static constexpr uint32_t INFO_OFFSET = 44, INFO_VERSION = 5;
static constexpr uint32_t OVERVIEW_OFFSET = INFO_OFFSET + sizeof(TrackInfo);
static_assert(OVERVIEW_OFFSET + OVERVIEW_COLS <= WAV_DATA_OFFSET - 8, "overview must fit in the header");
static uint8_t needsOverview = 0;                // bit t: header predates overviews

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
    // Smallest power of two bars that holds what's recorded so far.
    uint32_t sofar = max<int32_t>(0, (int32_t)(transport.now - recording.startSample));
    bars = 1;
    while (bars < MAX_BARS && (uint64_t)bars * framesPerBar < sofar) bars *= 2;
  }
  binsToColumns(recPeaks, bars * framesPerBar, recordedFrames, cols);
  return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static bool writeHeader(int t) {
  const Track &tr = tracks[t];
  bool used = tr.state != TrackState::Empty;
  uint32_t frames = used ? tr.recordedFrames : 0;
  uint8_t h[WAV_DATA_OFFSET] = {};
  memcpy(h, "RIFF", 4);       put32(h + 4, WAV_DATA_OFFSET - 8 + frames * 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);  put32(h + 16, 16);
  put16(h + 20, 1);           put16(h + 22, 2);           // PCM, stereo
  put32(h + 24, 44100);       put32(h + 28, 44100 * 4);
  put16(h + 32, 4);           put16(h + 34, 16);
  memcpy(h + 36, "JUNK", 4);  put32(h + 40, WAV_DATA_OFFSET - 44 - 8);
  TrackInfo info = {{'M', '8', 'L', 'P'}, INFO_VERSION, used, tr.spbNative, tr.loopBars,
                    frames, tr.rangeStartBeat, tr.rangeBeats, tr.state == TrackState::Muted,
                    used ? (uint32_t)posMod(-tr.rangeAnchorQ, 4 * tr.rangeBeats) : 0};
  memcpy(h + INFO_OFFSET, &info, sizeof info);
  if (used) memcpy(h + OVERVIEW_OFFSET, tr.overview, OVERVIEW_COLS);
  memcpy(h + WAV_DATA_OFFSET - 8, "data", 4);
  put32(h + WAV_DATA_OFFSET - 4, frames * 4);
  debugStage = "SD write (track header)";
  Track &w = tracks[t];
  bool ok = w.file.seekSet(0) && w.file.write(h, sizeof h) == (int)sizeof h;
  // Also pushes out the end of a recording still held in SdFat's sector cache,
  // so it survives power-off (the file's size doesn't change: no FS update).
  return w.file.sync() && ok;
}

static void readHeader(int t) {
  Track &tr = tracks[t];
  tr.state = TrackState::Empty;
  uint8_t h[WAV_DATA_OFFSET];
  if (!tr.file.seekSet(0) || tr.file.read(h, sizeof h) != (int)sizeof h) return;
  TrackInfo info;
  memcpy(&info, h + INFO_OFFSET, sizeof info);
  if (memcmp(h, "RIFF", 4) || memcmp(info.magic, "M8LP", 4) || info.version < 1 || info.version > 5) return;
  if (info.version == 1) { info.rangeStart *= BEATS_PER_BAR; info.rangeLen *= BEATS_PER_BAR; }
  if (info.version < 3) info.rangePhase = 0;
  if (info.version < 4) info.rangePhase *= 4;
  uint32_t loopBeats = info.loopBars * BEATS_PER_BAR;
  if (!info.used || !info.spb || !info.loopBars || !info.rangeLen ||
      info.rangeStart + info.rangeLen > loopBeats || info.recordedFrames > MAX_REC_FRAMES)
    return;
  while (info.rangeLen & (info.rangeLen - 1)) info.rangeLen &= info.rangeLen - 1;   // power of two
  tr.spbNative = info.spb;
  tr.loopBars = info.loopBars;
  tr.recordedFrames = info.recordedFrames;
  tr.rangeStartBeat = info.rangeStart;
  tr.rangeBeats = info.rangeLen;
  tr.rangeAnchorQ = -(int32_t)(info.rangePhase % (4 * info.rangeLen));
  tr.state = info.muted ? TrackState::Muted : TrackState::Playing;
  if (info.version >= 5) memcpy(tr.overview, h + OVERVIEW_OFFSET, OVERVIEW_COLS);
  else needsOverview |= 1 << t;
  tr.overviewVersion++;
}

// Older headers have no overview: read the recording once (at boot, before
// anything plays) and save one.
static void computeOverviewFromFile(int t) {
  Track &tr = tracks[t];
  memset(recPeaks, 0, sizeof recPeaks);
  for (uint32_t f = 0; f < tr.recordedFrames; f += CHUNK_FRAMES) {
    uint32_t n = min(CHUNK_FRAMES, tr.recordedFrames - f);
    if (!tr.file.seekSet(WAV_DATA_OFFSET + (uint64_t)f * 4) || tr.file.read(chunk, n * 4) != (int)(n * 4)) break;
    addPeaks(chunk, n, f, recPeaks);
  }
  buildOverview(tr, recPeaks, tr.recordedFrames);
  writeHeader(t);
}

void trackChanged(int t) { headerDirty |= 1 << t; }

static bool streamsTopped();

// Writes pending headers, but only when the streams can afford a pause.
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
    writeHeader(t);
    writePending = false;
    return;                                      // one per pass
  }
}

// ---- Track files: created once, full size, never resized while playing
static bool prepareTrackFile(int t, void (*progress)(const char *)) {
  Track &tr = tracks[t];
  char path[32];
  trackPath(t, path, sizeof path);
  tr.file = SD.sdfs.open(path, O_RDWR);
  if (tr.file && tr.file.fileSize() == TRACK_FILE_BYTES) {
    readHeader(t);
    return true;
  }
  tr.file.close();
  char msg[48];
  snprintf(msg, sizeof msg, "preparing track %d file (once)...", t + 1);
  progress(msg);
  SD.sdfs.remove(path);
  tr.file = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!tr.file) return false;
  tr.file.preAllocate(TRACK_FILE_BYTES);
  uint8_t zero = 0;                              // give the file its full size
  tr.file.seekSet(TRACK_FILE_BYTES - 1);
  tr.file.write(&zero, 1);
  tr.file.sync();
  tr.state = TrackState::Empty;
  return writeHeader(t);
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

bool storageBegin(void (*progress)(const char *)) {
  // DMA mode: every read/write is one self-contained transfer done by the SD
  // controller. The default FIFO mode leaves a multi-sector transfer open
  // between calls and has the CPU poll the FIFO; on the M8 it stalled for
  // seconds at a time (1 s timeouts, retried) with the audio engine running.
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) return false;
  SD.sdfs.mkdir("/looper");
  bool ok = true;
  for (int t = 0; t < NUM_TRACKS; t++) {
    char path[32];
    snprintf(path, sizeof path, "/looper/track%d.txt", t + 1);
    SD.sdfs.remove(path);                        // settings files of the previous version
    ok &= prepareTrackFile(t, progress);
  }
  for (int t = 0; t < NUM_TRACKS; t++) {
    if (!(needsOverview & (1 << t)) || tracks[t].state == TrackState::Empty) continue;
    char msg[48];
    snprintf(msg, sizeof msg, "drawing track %d waveform (once)...", t + 1);
    progress(msg);
    computeOverviewFromFile(t);
  }
  needsOverview = 0;
  return ok;
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
  for (auto &tr : tracks) tr.file = FsFile();
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) {
    Serial.println("SD re-init failed, retrying");
    lastTryFailed = true;
    lastFailedTry = millis();
    return;
  }
  lastTryFailed = false;
  char path[32];
  for (int t = 0; t < NUM_TRACKS; t++) {
    trackPath(t, path, sizeof path);
    tracks[t].file = SD.sdfs.open(path, O_RDWR);
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
  uint32_t start = tr.rangeStartFrame();
  if (tr.fileFrame >= start && tr.fileFrame < start + tr.rangeFrames()) {
    // Playhead position in the new range, in quarter beats (switches fall on
    // quarter beats; round away the per-quarter rounding of frames).
    int32_t posQ = (int32_t)(((uint64_t)(tr.fileFrame - start) * 4 + tr.spbNative / 2) / tr.spbNative);
    tr.rangeAnchorQ = tr.switchQ - posQ;
  } else {
    int32_t phaseQ = posMod(tr.switchQ - tr.rangeAnchorQ, 4 * tr.rangeBeats);
    tr.fileFrame = start + quarterFrames(phaseQ, tr.spbNative);
  }
  trackChanged(&tr - tracks);
}

// One SD read (or one block of padding silence) into the track's ring.
static void produce(Track &tr) {
  if (tr.rangePending && tr.head == tr.switchS) applyQueuedRange(tr);
  uint32_t free = RING_FRAMES - (tr.head - tr.tail);
  uint32_t rangeEnd = tr.rangeStartFrame() + tr.rangeFrames();
  uint32_t n = min(min(CHUNK_FRAMES, free), rangeEnd - tr.fileFrame);
  if (tr.rangePending) n = min(n, tr.switchS - tr.head);      // stop exactly at the switch
  if (tr.fileFrame >= tr.recordedFrames || sdFailed) {
    memset(chunk, 0, n * 4);                     // padding after the recording (or no card)
  } else {
    n = min(n, tr.recordedFrames - tr.fileFrame);
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
  tr.fileFrame += n;
  if (tr.fileFrame >= rangeEnd) tr.fileFrame = tr.rangeStartFrame();
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
  int32_t phaseQ = posMod(4 * beat0 - tr.rangeAnchorQ, 4 * tr.rangeBeats);
  tr.s0 = quarterFrames(phaseQ, tr.spbNative);
  tr.tail = tr.s0;
  tr.head = tr.s0;
  tr.fileFrame = tr.rangeStartFrame() + tr.s0;
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
      if (!(tr.live || tr.armed)) continue;
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
// Loop length in bars for a recording stopped after `beats` beats: the power
// of two P just below, if the stop came at most SNAP_THRESHOLD_BEATS past it;
// otherwise the next one, 2P (the recording then carries on until it gets
// there). Under one bar it carries on to one bar.
static uint32_t targetBars(double beats) {
  uint32_t p = 1;
  while (p * 2 * BEATS_PER_BAR <= beats && p * 2 <= MAX_BARS) p *= 2;
  if (beats <= p * BEATS_PER_BAR + SNAP_THRESHOLD_BEATS || p == MAX_BARS) return p;
  return p * 2;
}

static uint32_t maxRecordingFrames() {
  return min(MAX_BARS * BEATS_PER_BAR * transport.spb, MAX_REC_FRAMES);
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
  recording.mode = Recording::Idle;
  recording.track = -1;
  trackChanged(t);                               // header saved when the buffers allow
  if (transport.running) startTrack(t, transport.beat + 2, false);
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
  recording.written += n;
  writePending = false;
}

bool recordPress(int t) {
  switch (recording.mode) {
    case Recording::Idle: {
      if (tracks[t].state != TrackState::Empty || !tracks[t].file) return false;
      recording.track = t;
      recording.written = 0;
      recording.overruns = 0;
      memset(recPeaks, 0, sizeof recPeaks);
      if (!transport.running) {
        recording.mode = Recording::WaitBar;     // starts with the transport, on bar 1
        transportStart();
        return true;
      }
      // Pressed shortly after a bar line: start at that bar line (from the
      // input history). Otherwise wait for the next one.
      noInterrupts();
      uint32_t sinceBar = transport.now - transport.lastBarSample;
      if (sinceBar <= PREROLL_FRAMES) {
        recording.startSample = transport.lastBarSample + LATENCY_FRAMES;
        recording.startBeat = transport.lastBarBeat;
        recording.mode = Recording::Running;
      } else {
        recording.mode = Recording::WaitBar;
      }
      interrupts();
      return true;
    }
    case Recording::WaitBar:                     // cancel before it started
      recording.mode = Recording::Idle;
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
  if (recording.track == t && recording.mode != Recording::Idle) return;
  Track &tr = tracks[t];
  tr.live = false;
  tr.armed = false;
  tr.state = TrackState::Empty;
  memset(tr.overview, 0, sizeof tr.overview);
  tr.overviewVersion++;
  trackChanged(t);
}

void trackRange(int t, uint32_t &startBeat, uint32_t &beats) {
  const Track &tr = tracks[t];
  startBeat = tr.rangePending ? tr.pendingStartBeat : tr.rangeStartBeat;
  beats = tr.rangePending ? tr.pendingBeats : tr.rangeBeats;
}

bool setRange(int t, uint32_t startBeat, uint32_t beats) {
  Track &tr = tracks[t];
  if (tr.state == TrackState::Empty || beats < 1 || (beats & (beats - 1)) ||
      startBeat + beats > tr.loopBars * BEATS_PER_BAR)
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
  for (int t = 0; t < NUM_TRACKS; t++) startTrack(t, 0, true);
  midiSendNow(0xFA);                             // MIDI Start
  transport.running = true;
}

void transportStop() {
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

void flushHeaders() {
  while (headerDirty) {
    int t = __builtin_ctz(headerDirty);
    headerDirty &= ~(1 << t);
    writeHeader(t);
  }
}

void serviceStorage() {
  if (sdFailed) recoverSd();
  serviceRecorder();
  for (int t = 0; t < NUM_TRACKS; t++)
    if (tracks[t].needsRestart && transport.running) startTrack(t, transport.beat + 2, false);
  serviceStreams();
  serviceRecorder();
  serviceHeaders();
}

}  // namespace looper
