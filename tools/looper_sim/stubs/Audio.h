// Minimal stand-ins for the Teensy core + Audio library, enough to compile and
// drive engine.cpp / storage.cpp on a desktop (see ../looper_sim.cpp).
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <algorithm>

#define AUDIO_BLOCK_SAMPLES 128
#define AUDIO_SAMPLE_RATE_EXACT 44117.64706f
#define DMAMEM

using std::min;
using std::max;
template <class T, class L, class H> T constrain(T v, L lo, H hi) { return v < lo ? lo : v > hi ? hi : v; }

extern uint32_t simMicros;
inline uint32_t micros() { return simMicros; }
inline uint32_t millis() { return simMicros / 1000; }
// Busy waits let the (simulated) audio interrupt run meanwhile.
extern void (*simDelayHook)(uint32_t us);
inline void delayMicroseconds(uint32_t us) { if (simDelayHook) simDelayHook(us); else simMicros += us; }
inline void noInterrupts() {}
inline void interrupts() {}

struct audio_block_t { int16_t data[AUDIO_BLOCK_SAMPLES]; };

class AudioStream {
public:
  AudioStream(int ninputs, audio_block_t **queue) : inputs_(queue), n_(ninputs) {
    for (int i = 0; i < n_; i++) inputs_[i] = nullptr;
  }
  virtual ~AudioStream() {}
  virtual void update() = 0;
  // Test hooks
  audio_block_t *simOut[4] = {};
  void simSetInput(int i, audio_block_t *b) { inputs_[i] = b; }
protected:
  audio_block_t *receiveReadOnly(int i) { audio_block_t *b = inputs_[i]; inputs_[i] = nullptr; return b; }
  audio_block_t *allocate() { return new audio_block_t(); }
  void transmit(audio_block_t *b, int ch) { if (simOut[ch]) memcpy(simOut[ch], b, sizeof *b); }
  void release(audio_block_t *b) { if (!isInput(b)) delete b; }
private:
  bool isInput(audio_block_t *b) { return b == simIn[0] || b == simIn[1] || b == simIn[2] || b == simIn[3]; }
  audio_block_t **inputs_;
  int n_;
public:
  audio_block_t *simIn[4] = {};
};

class HardwareSerial {
public:
  void begin(uint32_t) {}
  size_t write(uint8_t b) { if (b == 0xF8) ticks++; else msgs[nmsg++ % 16] = b; return 1; }
  int available() { return 0; }                   // (MIDI in: the simulator feeds midiInByte() instead)
  int read() { return -1; }
  uint32_t ticks = 0;
  uint8_t msgs[16] = {};
  int nmsg = 0;
};

struct SerialStub {
  int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r; }
  void println(const char *s) { puts(s); }
};
extern SerialStub Serial;

class IntervalTimer {
public:
  bool begin(void (*f)(), int) { fn = f; return true; }
  static void (*fn)();
};
