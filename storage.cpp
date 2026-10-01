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

// ---- WAV header with the track's settings
// 16-bit stereo PCM; a JUNK chunk pads the header to 512 bytes so the audio is
// sector-aligned, and carries the settings below at offset 44.
struct TrackInfo {
  char magic[4];                                 // "M8LP"
  uint32_t version, used, spb, loopBars, recordedFrames, rangeStartBar, rangeBars, muted;
};
static constexpr uint32_t INFO_OFFSET = 44, INFO_VERSION = 1;

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
                    frames, tr.rangeStartBar, tr.rangeBars, tr.state == TrackState::Muted};
  memcpy(h + INFO_OFFSET, &info, sizeof info);
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
  if (memcmp(h, "RIFF", 4) || memcmp(info.magic, "M8LP", 4) || info.version != INFO_VERSION) return;
  if (!info.used || !info.spb || !info.loopBars || !info.rangeBars ||
      info.rangeStartBar + info.rangeBars > info.loopBars || info.recordedFrames > MAX_REC_FRAMES)
    return;
  tr.spbNative = info.spb;
  tr.loopBars = info.loopBars;
  tr.recordedFrames = info.recordedFrames;
  tr.rangeStartBar = info.rangeStartBar;
  tr.rangeBars = info.rangeBars;
  tr.anchorBeat = 0;
  tr.state = info.muted ? TrackState::Muted : TrackState::Playing;
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
static int32_t posMod(int32_t a, int32_t m) { int32_t r = a % m; return r < 0 ? r + m : r; }

// One SD read (or one block of padding silence) into the track's ring.
static void produce(Track &tr) {
  uint32_t free = RING_FRAMES - (tr.head - tr.tail);
  uint32_t rangeEnd = tr.rangeStartFrame() + tr.rangeFrames();
  uint32_t n = min(min(CHUNK_FRAMES, free), rangeEnd - tr.fileFrame);
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
  if (tr.state == TrackState::Empty) return;
  int32_t phase = posMod(beat0 - tr.anchorBeat, tr.rangeBars * BEATS_PER_BAR);
  tr.s0 = phase * tr.spbNative;
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
  tr.rangeStartBar = 0;
  tr.rangeBars = tr.loopBars;
  tr.anchorBeat = recording.startBeat;
  tr.state = TrackState::Playing;
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
  trackChanged(t);
}

bool setRange(int t, uint32_t startBar, uint32_t bars) {
  Track &tr = tracks[t];
  if (tr.state == TrackState::Empty || bars < 1 || startBar + bars > tr.loopBars) return false;
  tr.live = false;
  tr.armed = false;
  tr.rangeStartBar = startBar;
  tr.rangeBars = bars;
  trackChanged(t);
  if (transport.running) startTrack(t, transport.beat + 2, false);
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
