// engine.h — the looper's audio engine: master clock, track playback with
// varispeed, input capture/monitoring and MIDI clock scheduling.
//
// Threading: LooperEngine::update() runs in the audio interrupt. loop() code
// (storage.cpp, the sketch) talks to it through the volatile fields below.
// Rule of thumb: a track's playback fields are only changed by loop() while
// the track is neither live nor armed.
#pragma once
#include <Audio.h>
#include <SD.h>
#include "config.h"

namespace looper {

enum class TrackState : uint8_t { Empty, Playing, Muted };

// 2nd-order filter section (transposed direct form II).
struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

struct Track {
  // Description of the loop (set by loop() while not live)
  volatile TrackState state = TrackState::Empty;
  uint32_t spbNative = 0;          // samples per beat at the tempo it was recorded at
  uint32_t loopBars = 0;           // power of two
  uint32_t recordedFrames = 0;     // audio in the file; the rest of the loop is silence
  uint32_t rangeStartBeat = 0, rangeBeats = 0;   // loop range; length is a power of two beats
  // Transport position, in quarter beats, at which the loop range is at its
  // first beat (it repeats every rangeBeats from there). Starts on the beat
  // the recording began; shifts when a range change keeps the playhead.
  int32_t rangeAnchorQ = 0;
  float gain = 1.0f;
  // Filters (loop() sets them with setTrackFilter): highpass then lowpass,
  // 12 dB/octave each, on both channels.
  uint8_t filterBase = 0, filterWidth = FILTER_MAX;
  Biquad hp, lp;
  bool hpOn = false, lpOn = false;
  float filterZ[2][2][2] = {};     // [hp/lp][left/right][state]; interrupt only, or loop() while not live
  // Waveform overview (loop() only): peak level per column across the loop,
  // 0-255 on a square-root scale. overviewVersion changes whenever it does.
  uint8_t overview[OVERVIEW_COLS] = {};
  uint8_t overviewVersion = 0;

  uint32_t rangeStartFrame() const { return rangeStartBeat * spbNative; }
  uint32_t rangeFrames() const { return rangeBeats * spbNative; }

  // Stream: frames of the looped range in playback order. head is written by
  // loop(), tail by the interrupt. Counters run freely; ring index = n % RING.
  uint32_t *ring = nullptr;
  volatile uint32_t head = 0, tail = 0;
  double frac = 0;                 // interrupt only

  // Start handshake: loop() fills the ring from stream frame s0, then arms the
  // track; the interrupt goes live when the transport reaches startBeat.
  // Stream frame for transport beat b is then s0 + (b - startBeat) * spbNative.
  volatile bool live = false, armed = false;
  volatile bool needsRestart = false;    // interrupt -> loop(): missed the start, refill
  volatile int32_t startBeat = 0;
  uint32_t s0 = 0;
  volatile uint32_t underruns = 0;

  // Producer side (loop() only)
  FsFile file;
  uint32_t fileFrame = 0;          // loop frame that stream frame `head` comes from

  // Queued loop range (loop() only): the stream switches to it at stream
  // frame switchS, which falls on transport quarter beat switchQ. The old
  // range keeps playing until then.
  bool rangePending = false;
  uint32_t pendingStartBeat = 0, pendingBeats = 0, switchS = 0;
  int32_t switchQ = 0;
};

struct Transport {
  volatile bool running = false;         // set by loop()
  volatile uint32_t spb = 22059;         // samples per beat (~120 BPM)
  volatile uint32_t pendingSpb = 0;      // loop() -> interrupt: tempo change
  volatile int32_t beat = 0;             // beats since the transport started
  volatile uint32_t sampleInBeat = 0;
  volatile uint32_t now = 0;             // samples processed since boot
  volatile uint32_t lastBarSample = 0;   // `now` at the latest bar line
  volatile int32_t lastBarBeat = 0;
};

// Recording: the interrupt only fills in where it starts.
struct Recording {
  enum Mode : uint8_t { Idle, WaitBar, Running, Finishing };
  volatile Mode mode = Idle;
  int track = -1;
  volatile uint32_t startSample = 0;     // input sample index of the loop's first frame
  volatile int32_t startBeat = 0;
  uint32_t written = 0;                  // frames written to the file
  uint32_t targetFrames = 0;             // set when stopped
  uint32_t loopBars = 0;
  volatile uint32_t overruns = 0;
};

extern Track tracks[NUM_TRACKS];
extern Transport transport;
extern Recording recording;
extern uint32_t inputRing[INPUT_FRAMES];  // stereo input, index = sample % INPUT_FRAMES

uint32_t spbForBpm(float bpm);
float bpmForSpb(uint32_t spb);
float filterStepHz(int step);
void setTrackFilter(int t, int base, int width);            // steps, clamped to 0..FILTER_MAX
float filterResponseDb(int base, int width, float hz);      // magnitude of the pair (analog approximation)

void midiBegin(HardwareSerial *port);   // port already started (Board::midiOutBegin)
void midiSendNow(uint8_t status);        // transport start/stop messages

class LooperEngine : public AudioStream {
public:
  LooperEngine() : AudioStream(2, inputQueue_) {}
  float masterGain = 0.7f;
  bool monitor = true;
  void update() override;

private:
  audio_block_t *inputQueue_[2];
  int tickInBeat_ = 0;
  uint32_t nextTickAt_ = 0;
  void onBeat(uint32_t blockMicros, int sampleIndex);
};

}  // namespace looper
