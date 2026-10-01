// storage.cpp — see storage.h
#include "storage.h"
#include <inttypes.h>

namespace looper {

volatile const char *debugStage = "boot";
static uint32_t chunk[CHUNK_FRAMES];
static FsFile recFile;

static void trackPath(int t, const char *ext, char *buf, size_t n) {
  snprintf(buf, n, "/looper/track%d.%s", t + 1, ext);
}

// ---- WAV header: 16-bit stereo, audio at WAV_DATA_OFFSET (a JUNK chunk pads
// the header out to 512 bytes so every read of the audio is sector-aligned).
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void writeWavHeader(FsFile &f, uint32_t frames) {
  uint8_t h[WAV_DATA_OFFSET] = {};
  memcpy(h, "RIFF", 4);       put32(h + 4, WAV_DATA_OFFSET - 8 + frames * 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);  put32(h + 16, 16);
  put16(h + 20, 1);           put16(h + 22, 2);           // PCM, stereo
  put32(h + 24, 44100);       put32(h + 28, 44100 * 4);
  put16(h + 32, 4);           put16(h + 34, 16);
  memcpy(h + 36, "JUNK", 4);  put32(h + 40, WAV_DATA_OFFSET - 44 - 8);
  memcpy(h + WAV_DATA_OFFSET - 8, "data", 4);
  put32(h + WAV_DATA_OFFSET - 4, frames * 4);
  f.seekSet(0);
  f.write(h, sizeof h);
}

// ---- Track info (key=value text next to the WAV)
void saveTrackInfo(int t) {
  const Track &tr = tracks[t];
  char path[32];
  trackPath(t, "txt", path, sizeof path);
  FsFile f = SD.sdfs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  debugStage = "save track info";
  f.printf("spb=%lu\nbars=%lu\nrecorded=%lu\nrangeStart=%lu\nrangeBars=%lu\nmuted=%d\n",
           tr.spbNative, tr.loopBars, tr.recordedFrames, tr.rangeStartBar, tr.rangeBars,
           tr.state == TrackState::Muted);
  f.close();
}

static bool loadTrackInfo(int t) {
  Track &tr = tracks[t];
  char path[32], line[48];
  trackPath(t, "txt", path, sizeof path);
  FsFile f = SD.sdfs.open(path, O_RDONLY);
  if (!f) return false;
  uint32_t spb = 0, bars = 0, rec = 0, rs = 0, rb = 0, muted = 0;
  while (f.available()) {
    int n = f.fgets(line, sizeof line);
    if (n <= 0) break;
    sscanf(line, "spb=%" SCNu32, &spb);
    sscanf(line, "bars=%" SCNu32, &bars);
    sscanf(line, "recorded=%" SCNu32, &rec);
    sscanf(line, "rangeStart=%" SCNu32, &rs);
    sscanf(line, "rangeBars=%" SCNu32, &rb);
    sscanf(line, "muted=%" SCNu32, &muted);
  }
  f.close();
  if (!spb || !bars || !rb || rs + rb > bars) return false;

  trackPath(t, "wav", path, sizeof path);
  tr.file = SD.sdfs.open(path, O_RDONLY);
  if (!tr.file || tr.file.fileSize() < WAV_DATA_OFFSET + (uint64_t)rec * 4) return false;
  tr.spbNative = spb;
  tr.loopBars = bars;
  tr.recordedFrames = rec;
  tr.rangeStartBar = rs;
  tr.rangeBars = rb;
  tr.anchorBeat = 0;
  tr.state = muted ? TrackState::Muted : TrackState::Playing;
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

bool storageBegin() {
  // DMA mode: every read/write is one self-contained transfer done by the SD
  // controller. The default FIFO mode leaves a multi-sector transfer open
  // between calls and has the CPU poll the FIFO; on the M8 it stalled for
  // seconds at a time (1 s timeouts, retried) with the audio engine running.
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) return false;
  SD.sdfs.mkdir("/looper");
  for (int t = 0; t < NUM_TRACKS; t++) {
    if (!loadTrackInfo(t)) {
      tracks[t].file.close();
      tracks[t].state = TrackState::Empty;
    }
  }
  return true;
}

// Slow or failed SD transfers go to Serial with the card's error code.
static void logSd(const char *op, uint32_t ms, int got, uint32_t want) {
  SdCard *card = SD.sdfs.card();
  Serial.printf("slow SD %s: %lu ms, %d/%lu bytes, card error 0x%02X data 0x%lX\n", op, ms, got, want,
                card ? card->errorCode() : 0, card ? card->errorData() : 0);
}

// ---- Recovery from a card that stops responding
// Seen on the M8: after a run of slow writes, every read failed with a
// command timeout (CMD18, 1-2 s each) until the card was set up again. When a
// transfer fails, reads return silence and writes wait, and serviceStorage()
// re-initialises the card and reopens the files where they were.
static bool sdFailed = false;

static void reopenFiles() {
  char path[32];
  for (int t = 0; t < NUM_TRACKS; t++) {
    if (tracks[t].state == TrackState::Empty) continue;
    trackPath(t, "wav", path, sizeof path);
    tracks[t].file = SD.sdfs.open(path, O_RDONLY);
  }
  if (recording.mode == Recording::Running || recording.mode == Recording::Finishing) {
    trackPath(recording.track, "wav", path, sizeof path);
    recFile = SD.sdfs.open(path, O_RDWR);
    recFile.seekSet(WAV_DATA_OFFSET + (uint64_t)recording.written * 4);
  }
}

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
  recFile = FsFile();
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO))) {
    Serial.println("SD re-init failed, retrying");
    lastTryFailed = true;
    lastFailedTry = millis();
    return;
  }
  lastTryFailed = false;
  reopenFiles();
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
    if (ms > 50 || got != (int)(n * 4))
      logSd("read", ms, got, n * 4);
  }
  uint32_t head = tr.head;
  for (uint32_t i = 0; i < n; i++) tr.ring[(head + i) % RING_FRAMES] = chunk[i];
  tr.head = head + n;
  tr.fileFrame += n;
  if (tr.fileFrame >= rangeEnd) tr.fileFrame = tr.rangeStartFrame();
}

// Fill the ring so the track joins in at transport beat beat0, in phase with
// its anchor and loop range.
static void startTrack(int t, int32_t beat0) {
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
  while (RING_FRAMES - (tr.head - tr.tail) >= CHUNK_FRAMES) produce(tr);
  tr.startBeat = beat0;
  tr.armed = true;
}

static void serviceStreams() {
  for (int pass = 0; pass < 4; pass++) {
    Track *best = nullptr;
    uint32_t bestFill = RING_FRAMES;
    for (auto &tr : tracks) {
      if (!(tr.live || tr.armed)) continue;
      uint32_t fill = tr.head - tr.tail;
      if (RING_FRAMES - fill >= CHUNK_FRAMES && fill < bestFill) { best = &tr; bestFill = fill; }
    }
    if (!best) return;
    produce(*best);
  }
}

// ---- Recording
static uint32_t snapBars(double beats) {
  if (beats < BEATS_PER_BAR) return 1;
  uint32_t p = 1;
  while (p * 2 * BEATS_PER_BAR <= beats && p * 2 <= MAX_BARS) p *= 2;
  if (beats - p * BEATS_PER_BAR < 1.0 || p == MAX_BARS) return p;  // less than a beat over: shrink
  return p * 2;                                                     // otherwise round up
}

static void stopRecording() {
  noInterrupts();
  uint32_t stopSample = transport.now + LATENCY_FRAMES;
  uint32_t spb = transport.spb;
  interrupts();
  uint32_t length = stopSample - recording.startSample;
  recording.loopBars = snapBars((double)length / spb);
  recording.targetFrames = min(length, recording.loopBars * BEATS_PER_BAR * spb);
  recording.mode = Recording::Finishing;
}

static void finishRecording() {
  int t = recording.track;
  Track &tr = tracks[t];
  debugStage = "finish recording (header, truncate, reopen)";
  writeWavHeader(recFile, recording.targetFrames);
  recFile.truncate(WAV_DATA_OFFSET + (uint64_t)recording.targetFrames * 4);
  recFile.close();
  char path[32];
  trackPath(t, "wav", path, sizeof path);
  tr.file = SD.sdfs.open(path, O_RDONLY);
  tr.spbNative = transport.spb;
  tr.loopBars = recording.loopBars;
  tr.recordedFrames = recording.targetFrames;
  tr.rangeStartBar = 0;
  tr.rangeBars = tr.loopBars;
  tr.anchorBeat = recording.startBeat;
  tr.state = TrackState::Playing;
  recording.mode = Recording::Idle;
  recording.track = -1;
  saveTrackInfo(t);
  if (transport.running) startTrack(t, transport.beat + 2);
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
  else if ((int32_t)(now - recording.startSample) >= (int32_t)(MAX_BARS * BEATS_PER_BAR * transport.spb))
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
  uint32_t n = min((uint32_t)avail, CHUNK_FRAMES);
  for (uint32_t i = 0; i < n; i++) chunk[i] = inputRing[(next + i) % INPUT_FRAMES];
  debugStage = "SD write (recording)";
  uint32_t t0 = millis();
  int put = recFile.write(chunk, n * 4);
  uint32_t ms = millis() - t0;
  if (ms > 50 || put != (int)(n * 4)) logSd("write", ms, put, n * 4);
  if (put != (int)(n * 4)) {                     // retried after recovery
    sdFailed = true;
    return;
  }
  recording.written += n;
}

bool recordPress(int t) {
  switch (recording.mode) {
    case Recording::Idle: {
      if (tracks[t].state != TrackState::Empty) return false;
      char path[32];
      trackPath(t, "wav", path, sizeof path);
      debugStage = "start recording (create + preallocate file)";
      recFile = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
      if (!recFile) return false;
      uint32_t maxFrames = MAX_BARS * BEATS_PER_BAR * transport.spb;
      uint64_t maxBytes = WAV_DATA_OFFSET + (uint64_t)maxFrames * 4;
      recFile.preAllocate(maxBytes);
      // Give the file its full size now and save that, so that if the card has
      // to be re-initialised mid-recording the file can be reopened and written
      // at the right place. finishRecording() truncates it to the real length.
      uint8_t zero = 0;
      recFile.seekSet(maxBytes - 1);
      recFile.write(&zero, 1);
      recFile.sync();
      writeWavHeader(recFile, 0);
      recFile.seekSet(WAV_DATA_OFFSET);
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
      recFile.close();
      clearTrack(recording.track);
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
  debugStage = "clear track (delete files)";
  tr.file.close();
  char path[32];
  trackPath(t, "wav", path, sizeof path);
  SD.sdfs.remove(path);
  trackPath(t, "txt", path, sizeof path);
  SD.sdfs.remove(path);
}

bool setRange(int t, uint32_t startBar, uint32_t bars) {
  Track &tr = tracks[t];
  if (tr.state == TrackState::Empty || bars < 1 || startBar + bars > tr.loopBars) return false;
  tr.live = false;
  tr.armed = false;
  tr.rangeStartBar = startBar;
  tr.rangeBars = bars;
  saveTrackInfo(t);
  if (transport.running) startTrack(t, transport.beat + 2);
  return true;
}

// ---- Transport
void transportStart() {
  if (transport.running) return;
  transport.beat = 0;
  transport.sampleInBeat = 0;
  for (int t = 0; t < NUM_TRACKS; t++) startTrack(t, 0);
  midiSendNow(0xFA);                             // MIDI Start
  transport.running = true;
}

void transportStop() {
  if (recording.mode == Recording::WaitBar) recordPress(recording.track);   // cancel
  if (recording.mode == Recording::Running) stopRecording();
  transport.running = false;
  for (auto &tr : tracks) { tr.live = false; tr.armed = false; }
  midiSendNow(0xFC);                             // MIDI Stop
}

void serviceStorage() {
  if (sdFailed) recoverSd();
  serviceRecorder();
  for (int t = 0; t < NUM_TRACKS; t++)
    if (tracks[t].needsRestart && transport.running) startTrack(t, transport.beat + 2);
  serviceStreams();
  serviceRecorder();
}

}  // namespace looper
