// sd_stress_test.ino — SD streaming stress test for the M8 looper
//
// Step 2 of the looper plan: can the SD card feed 8 stereo tracks at up to 2x
// speed while recording a 9th, with 32 KB of RAM buffer per track?
//
//   * 8 tracks of 16 bars at 120 BPM (stereo, 16-bit, 5.6 MB each) are
//     generated on the card once (/looptest/), each a different tone pulsing on
//     the beat, so you can hear dropouts
//   * every track loops its own range of bars, so playback keeps jumping back
//     inside the file, like the looper's loop ranges will
//   * each track plays at the same adjustable speed (linear interpolation)
//   * EDIT starts/stops recording the input to a 9th preallocated file
//   * the input is monitored to the output at all times
//
// Keys: UP/DOWN speed 0.5x..2.0x   LEFT/RIGHT number of tracks 1..8
//       EDIT record on/off          PLAY reset the statistics
//
// The screen shows, per track, dropouts and the lowest buffer level seen, plus
// the slowest SD read and write. Same numbers on Serial every second.
//
// Build: Teensyduino, board "Teensy 4.0", USB type "Serial".

#include <Audio.h>
#include <SD.h>
#include "M8Board.h"

m8::Board board;

// ---- Configuration
static constexpr int TRACKS = 8;
static constexpr float BPM = 120.0f;
static constexpr uint32_t FRAMES_PER_BAR = (uint32_t)(44100.0f * 60.0f / BPM * 4);
static constexpr uint32_t FILE_BARS = 16;
static constexpr uint32_t FILE_FRAMES = FRAMES_PER_BAR * FILE_BARS;
static constexpr uint32_t FILE_BYTES = FILE_FRAMES * 4;           // stereo int16
static constexpr uint32_t RING_FRAMES = 8192;                     // 32 KB per track
static constexpr uint32_t CHUNK_FRAMES = 4096;                    // 16 KB per SD call

// Stereo frames packed as uint32: left in the low half, right in the high half.
DMAMEM static uint32_t trackRing[TRACKS][RING_FRAMES];
DMAMEM static uint32_t recRing[RING_FRAMES];
static uint32_t chunk[CHUNK_FRAMES];

struct Track {
  // written by loop() (producer)
  volatile uint32_t head = 0;          // frames written into the ring, ever
  uint32_t filePos = 0;                // next frame to read from the file
  uint32_t rangeStart = 0, rangeEnd = 0;
  FsFile file;
  // written by the audio interrupt (consumer)
  volatile uint32_t tail = 0;          // frames consumed, ever
  float frac = 0;
  volatile uint32_t underruns = 0;
  volatile uint32_t minFill = RING_FRAMES;
};
static Track tracks[TRACKS];

static volatile float speed = 1.0f;
static volatile int activeTracks = TRACKS;
static volatile bool recording = false;
static volatile uint32_t recHead = 0, recOverruns = 0;      // written by the interrupt
static uint32_t recTail = 0, recFilePos = 0;
static FsFile recFile;

// ---- Audio: mix the tracks + monitor the input; feed the recorder
class StressMixer : public AudioStream {
public:
  StressMixer() : AudioStream(2, inputQueue_) {}
  void update() override {
    audio_block_t *inL = receiveReadOnly(0), *inR = receiveReadOnly(1);
    audio_block_t *outL = allocate(), *outR = allocate();
    if (outL && outR) {
      float mixL[AUDIO_BLOCK_SAMPLES] = {}, mixR[AUDIO_BLOCK_SAMPLES] = {};
      float sp = speed;
      int n = activeTracks;
      for (int t = 0; t < n; t++) playTrack(tracks[t], trackRing[t], sp, mixL, mixR);
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        float l = mixL[i] / TRACKS + (inL ? inL->data[i] : 0);
        float r = mixR[i] / TRACKS + (inR ? inR->data[i] : 0);
        outL->data[i] = (int16_t)constrain(l, -32767.0f, 32767.0f);
        outR->data[i] = (int16_t)constrain(r, -32767.0f, 32767.0f);
      }
      transmit(outL, 0);
      transmit(outR, 1);
    }
    if (recording) {
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        if (recHead - recTail >= RING_FRAMES) { recOverruns++; break; }
        uint16_t l = inL ? inL->data[i] : 0, r = inR ? inR->data[i] : 0;
        recRing[recHead % RING_FRAMES] = l | ((uint32_t)r << 16);
        recHead++;
      }
    }
    if (outL) release(outL);
    if (outR) release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
  }

private:
  audio_block_t *inputQueue_[2];

  static void playTrack(Track &tr, const uint32_t *ring, float sp, float *mixL, float *mixR) {
    uint32_t tail = tr.tail, head = tr.head;
    float frac = tr.frac;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      if (head - tail < 2) { tr.underruns++; continue; }
      uint32_t a = ring[tail % RING_FRAMES], b = ring[(tail + 1) % RING_FRAMES];
      float al = (int16_t)(a & 0xFFFF), ar = (int16_t)(a >> 16);
      float bl = (int16_t)(b & 0xFFFF), br = (int16_t)(b >> 16);
      mixL[i] += al + (bl - al) * frac;
      mixR[i] += ar + (br - ar) * frac;
      frac += sp;
      while (frac >= 1.0f) { frac -= 1.0f; tail++; }
    }
    tr.tail = tail;
    tr.frac = frac;
    uint32_t fill = head - tail;
    if (fill < tr.minFill) tr.minFill = fill;
  }
};

AudioInputI2S        i2sIn;
StressMixer          mixer;
AudioOutputI2S       i2sOut;
AudioConnection      c1(i2sIn, 0, mixer, 0), c2(i2sIn, 1, mixer, 1);
AudioConnection      c3(mixer, 0, i2sOut, 0), c4(mixer, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

// ---- SD timing statistics (reset with PLAY)
static uint32_t maxReadUs = 0, maxWriteUs = 0, slowOps = 0, bytesMoved = 0;

static void timeOp(uint32_t us, bool write) {
  if (write) maxWriteUs = max(maxWriteUs, us);
  else maxReadUs = max(maxReadUs, us);
  if (us > 20000) slowOps++;
}

// ---- Test files
static void statusLine(int y, const char *msg) {
  auto &t = board.tft();
  t.fillRect(0, y, 320, 12, ILI9341_BLACK);
  t.setTextColor(ILI9341_WHITE);
  t.setTextSize(1);
  t.setCursor(4, y + 2);
  t.print(msg);
  Serial.println(msg);
}

// A tone per track (A2 upwards in fourths), pulsing on every beat.
static void makeTrackFile(int t, const char *path) {
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  f.preAllocate(FILE_BYTES);
  float freq = 110.0f * powf(2.0f, t * 5 / 12.0f), phase = 0;
  const uint32_t beat = FRAMES_PER_BAR / 4;
  for (uint32_t pos = 0; pos < FILE_FRAMES; pos += CHUNK_FRAMES) {
    uint32_t n = min(CHUNK_FRAMES, FILE_FRAMES - pos);
    for (uint32_t i = 0; i < n; i++) {
      float env = expf(-(float)((pos + i) % beat) / 6000.0f);
      int16_t s = (int16_t)(sinf(phase) * env * 20000.0f);
      phase += 2.0f * PI * freq / 44100.0f;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;
      chunk[i] = (uint16_t)s | ((uint32_t)(uint16_t)s << 16);
    }
    f.write(chunk, n * 4);
  }
  f.close();
}

static bool prepareFiles() {
  SD.sdfs.mkdir("/looptest");
  char path[32], msg[64];
  for (int t = 0; t < TRACKS; t++) {
    snprintf(path, sizeof path, "/looptest/track%d.raw", t);
    FsFile f = SD.sdfs.open(path, O_RDONLY);
    bool ok = f && f.fileSize() == FILE_BYTES;
    f.close();
    if (!ok) {
      snprintf(msg, sizeof msg, "generating %s (5.6 MB, once)...", path);
      statusLine(40, msg);
      makeTrackFile(t, path);
    }
    Track &tr = tracks[t];
    tr.file = SD.sdfs.open(path, O_RDONLY);
    if (!tr.file) return false;
    // Loop ranges of 1-3 bars at different offsets, so every track keeps seeking.
    tr.rangeStart = (t % 4) * FRAMES_PER_BAR;
    tr.rangeEnd = tr.rangeStart + (1 + t % 3) * FRAMES_PER_BAR;
    tr.filePos = tr.rangeStart;
    tr.file.seekSet((uint64_t)tr.filePos * 4);
  }
  statusLine(40, "preallocating /looptest/rec.raw...");
  recFile = SD.sdfs.open("/looptest/rec.raw", O_RDWR | O_CREAT);
  if (!recFile) return false;
  if (recFile.fileSize() < FILE_BYTES) recFile.preAllocate(FILE_BYTES);
  return true;
}

// ---- Streaming: one SD call per invocation, the most urgent first
static bool serviceRecorder() {
  if (recHead - recTail < CHUNK_FRAMES) return false;
  for (uint32_t i = 0; i < CHUNK_FRAMES; i++) chunk[i] = recRing[(recTail + i) % RING_FRAMES];
  if (recFilePos + CHUNK_FRAMES > FILE_FRAMES) { recFilePos = 0; recFile.seekSet(0); }
  uint32_t t0 = micros();
  recFile.write(chunk, CHUNK_FRAMES * 4);
  timeOp(micros() - t0, true);
  recFilePos += CHUNK_FRAMES;
  recTail += CHUNK_FRAMES;
  bytesMoved += CHUNK_FRAMES * 4;
  return true;
}

static bool serviceTracks() {
  int best = -1;
  uint32_t bestFill = RING_FRAMES;
  for (int t = 0; t < activeTracks; t++) {
    uint32_t fill = tracks[t].head - tracks[t].tail;
    if (RING_FRAMES - fill >= CHUNK_FRAMES && fill < bestFill) { best = t; bestFill = fill; }
  }
  if (best < 0) return false;
  Track &tr = tracks[best];
  uint32_t n = min(CHUNK_FRAMES, tr.rangeEnd - tr.filePos);
  uint32_t t0 = micros();
  tr.file.read(chunk, n * 4);
  timeOp(micros() - t0, false);
  uint32_t head = tr.head;
  for (uint32_t i = 0; i < n; i++) trackRing[best][(head + i) % RING_FRAMES] = chunk[i];
  tr.head = head + n;
  tr.filePos += n;
  if (tr.filePos >= tr.rangeEnd) {                  // loop range: jump back
    tr.filePos = tr.rangeStart;
    tr.file.seekSet((uint64_t)tr.filePos * 4);
  }
  bytesMoved += n * 4;
  return true;
}

// ---- Screen
static void drawLabels() {
  auto &t = board.tft();
  t.fillScreen(ILI9341_BLACK);
  t.setTextSize(1);
  t.setTextColor(ILI9341_DARKGREY);
  t.setCursor(4, 216); t.print("UP/DOWN speed  LEFT/RIGHT tracks  EDIT record");
  t.setCursor(4, 228); t.print("PLAY reset stats");
}

// Text is drawn over its own background instead of clearing the area first:
// a big fillRect would block the loop for ~35 ms and starve the SD streaming.
static void drawStats() {
  auto &t = board.tft();
  t.setTextSize(1);
  char line[64];
  t.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  t.setCursor(4, 4);
  t.printf("speed %.1fx  tracks %d  rec %-3s    ", speed, activeTracks, recording ? "ON" : "off");
  t.setCursor(4, 18);
  t.printf("slowest read %3lu ms  write %3lu ms  >20ms: %-5lu", maxReadUs / 1000, maxWriteUs / 1000, slowOps);
  t.setCursor(4, 32);
  t.printf("SD %.2f MB/s   rec overruns %-6lu", bytesMoved / 1e6f, recOverruns);
  Serial.printf("speed %.1f tracks %d rec %d | read max %lu us write max %lu us slow %lu | %.2f MB/s rec overruns %lu\n",
                speed, activeTracks, recording, maxReadUs, maxWriteUs, slowOps, bytesMoved / 1e6f, recOverruns);
  for (int i = 0; i < TRACKS; i++) {
    Track &tr = tracks[i];
    float minMs = tr.minFill * 1000.0f / 44100.0f / speed;   // time left at this speed
    int y = 52 + i * 19;
    t.setTextColor(i < activeTracks ? ILI9341_WHITE : ILI9341_DARKGREY, ILI9341_BLACK);
    t.setCursor(4, y + 4);
    snprintf(line, sizeof line, "T%d  dropouts %4lu  min buffer %4.0f ms", i + 1, tr.underruns, minMs);
    t.print(line);
    int w = (int)(tr.minFill * 60 / RING_FRAMES);
    t.fillRect(250, y + 2, w, 10, tr.underruns ? ILI9341_RED : ILI9341_GREEN);
    t.fillRect(250 + w, y + 2, 60 - w, 10, ILI9341_DARKGREY);
    if (i < activeTracks) Serial.printf("  T%d dropouts %lu min %.0f ms\n", i + 1, tr.underruns, minMs);
    tr.minFill = RING_FRAMES;
  }
  bytesMoved = 0;
}

static void resetStats() {
  maxReadUs = maxWriteUs = slowOps = 0;
  recOverruns = 0;
  for (auto &tr : tracks) { tr.underruns = 0; tr.minFill = RING_FRAMES; }
}

static void handlePowerButton() {
  if (!board.powerButtonPressed()) return;
  uint32_t start = millis();
  while (board.powerButtonHeld())
    if (millis() - start >= 1000) { codec.volume(0.0f); board.powerOff(); }
  board.powerButtonPressed();
}

void setup() {
  Serial.begin(115200);
  AudioMemory(16);
  board.begin();
  board.powerButtonPressed();
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.volume(0.5f);

  board.tft().fillScreen(ILI9341_BLACK);
  if (!SD.begin(BUILTIN_SDCARD)) { statusLine(40, "No SD card."); for (;;) handlePowerButton(); }
  if (!prepareFiles()) { statusLine(40, "Could not open the test files."); for (;;) handlePowerButton(); }

  // Fill every ring before the interrupt starts consuming.
  while (serviceTracks()) {}
  resetStats();
  drawLabels();
}

void loop() {
  handlePowerButton();

  // Keep the SD busy: recorder first (it must never overflow), then the emptiest track.
  for (int i = 0; i < 4; i++) {
    bool did = serviceRecorder();
    did |= serviceTracks();
    if (!did) break;
  }

  uint8_t keys;
  static uint8_t prev = 0;
  if (board.keysChanged(keys)) {
    uint8_t pressed = keys & ~prev;
    prev = keys;
    if (pressed & m8::BTN_UP) speed = min(speed + 0.1f, 2.0f);
    if (pressed & m8::BTN_DOWN) speed = max(speed - 0.1f, 0.5f);
    if (pressed & m8::BTN_RIGHT) activeTracks = min(activeTracks + 1, TRACKS);
    if (pressed & m8::BTN_LEFT) activeTracks = max(activeTracks - 1, 1);
    if (pressed & m8::BTN_EDIT) {
      if (recording) {
        recording = false;
      } else {
        recTail = recHead;                          // drop anything left over
        recording = true;
      }
    }
    if (pressed & m8::BTN_PLAY) resetStats();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    drawStats();
  }
}
