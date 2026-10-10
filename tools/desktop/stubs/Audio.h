// Stand-ins for the Teensy core and Audio library on a desktop (the L7 desktop
// app): real time, the audio interrupt as SDL's audio thread (noInterrupts():
// a lock it holds while the engine runs), the audio graph's ends as no-ops.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cstdlib>
#include <algorithm>

#define AUDIO_BLOCK_SAMPLES 128
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f                 // (the desktop app's; the M8: 44117.64706)
#define DMAMEM
#define FLASHMEM
#define FASTRUN
#define EXTMEM
#ifndef PROGMEM
#define PROGMEM
#endif
#define AUDIO_INPUT_LINEIN 0

using std::min;
using std::max;
template <class T, class L, class H> T constrain(T v, L lo, H hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t micros();
uint32_t millis();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void noInterrupts();
void interrupts();
void yield();

struct audio_block_t { int16_t data[AUDIO_BLOCK_SAMPLES]; };

class AudioStream {
public:
  AudioStream(int ninputs, audio_block_t **queue) : inputs_(queue), n_(ninputs) {
    for (int i = 0; i < n_; i++) inputs_[i] = nullptr;
  }
  virtual ~AudioStream() {}
  virtual void update() = 0;
  audio_block_t *simOut[4] = {};                   // (the app's: where transmit() copies to)
  audio_block_t *simIn[4] = {};
  void simSetInput(int i, audio_block_t *b) { inputs_[i] = b; }
protected:
  audio_block_t *receiveReadOnly(int i) { audio_block_t *b = inputs_[i]; inputs_[i] = nullptr; return b; }
  audio_block_t *receiveWritable(int i) {
    audio_block_t *b = receiveReadOnly(i);
    if (!b) return nullptr;
    audio_block_t *w = allocate();
    memcpy(w, b, sizeof *w);
    return w;
  }
  audio_block_t *allocate() { return new audio_block_t(); }
  void transmit(audio_block_t *b, int ch = 0) { if (ch < 4 && simOut[ch]) memcpy(simOut[ch], b, sizeof *b); }
  void release(audio_block_t *b) { if (b != simIn[0] && b != simIn[1] && b != simIn[2] && b != simIn[3]) delete b; }
private:
  audio_block_t **inputs_;
  int n_;
};

struct AudioInputI2S {};
struct AudioOutputI2S {};
struct AudioConnection { template <class A, class B> AudioConnection(A &, int, B &, int) {} };
struct AudioControlSGTL5000 {
  bool enable() { return true; }
  void inputSelect(int) {}
  void lineInLevel(int) {}
  void lineOutLevel(int) {}
  void volume(float v) { gain = v; }
  float gain = 0.6f;
};
inline void AudioMemory(int) {}

class HardwareSerial {
public:
  void begin(uint32_t) {}
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t *, size_t n) { return n; }
  int available() { return 0; }
  int read() { return -1; }
  void flush() {}
};

struct CrashReportStub;
struct SerialStub {
  void begin(uint32_t) {}
  int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r; }
  void println(const char *s = "") { puts(s); }
  void print(const char *s) { fputs(s, stdout); }
  void print(const struct CrashReportStub &) {}
  explicit operator bool() const { return true; }
};
extern SerialStub Serial;
struct CrashReportStub { explicit operator bool() const { return false; } };
extern CrashReportStub CrashReport;

class IntervalTimer {
public:
  bool begin(void (*)(), int) { return true; }
  void priority(int) {}
  void end() {}
};
