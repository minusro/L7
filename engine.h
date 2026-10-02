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
  uint32_t loopBars = 0;           // whole bars
  uint32_t recordedFrames = 0;     // audio in the file; the rest of the loop is silence
  // Loop range: starts on any beat of the loop; any length from a beat up to
  // the whole loop. It may run past the loop's end and continue from its start.
  uint32_t rangeStartBeat = 0, rangeBeats = 0;
  // Transport position, in quarter beats, at which the loop range is at its
  // first beat (it repeats every rangeBeats from there). Starts on the beat
  // the recording began; shifts when a range change keeps the playhead.
  int32_t rangeAnchorQ = 0;
  float gain = 1.0f;               // overall trim (tests)
  // Volume and balance (loop() sets them with setTrackMix): a 2x2 mix, out
  // left = mixLL * left + mixLR * right, out right = mixRL * left + mixRR * right
  // (volume included). levelL/R: each output's gain for a signal on both
  // channels (row sums), for display.
  int8_t volume = VOLUME_UNITY, balance = 0;
  float mixLL = 1, mixLR = 0, mixRL = 0, mixRR = 1;
  float levelL = 1, levelR = 1;
  // Output level meter: peak |sample| / 32768 per channel after the track's
  // gains (before master gain and mute), since loop() last took it.
  volatile float peakL = 0, peakR = 0;
  // Filters (loop() sets them with setTrackFilter): highpass then lowpass,
  // 12 dB/octave each, on both channels.
  int8_t filterLo = 0, filterHi = FILTER_MAX;   // steps (see config.h)
  Biquad hp, lp;
  bool hpOn = false, lpOn = false;
  float filterZ[2][2][2] = {};     // [hp/lp][left/right][state]; interrupt only, or loop() while not live
  // Waveform overview (loop() only): peak level per column across the loop,
  // 0-255 on a square-root scale. overviewVersion changes whenever it does.
  uint8_t overview[OVERVIEW_COLS] = {};
  uint8_t overviewVersion = 0;

  uint32_t rangeStartFrame() const { return rangeStartBeat * spbNative; }
  uint32_t rangeFrames() const { return rangeBeats * spbNative; }
  uint32_t loopFrames() const { return loopBars * BEATS_PER_BAR * spbNative; }
  // Loop frame `offset` frames into the range (offset < rangeFrames()).
  uint32_t rangeFrame(uint32_t offset) const { return (rangeStartFrame() + offset) % loopFrames(); }
  // How far loop frame f is into the range (>= rangeFrames(): outside it).
  uint32_t rangeOffset(uint32_t f) const { return (f + loopFrames() - rangeStartFrame()) % loopFrames(); }

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
  volatile uint32_t playSpb = 0;         // spbNative of what the interrupt is playing
  volatile uint32_t underruns = 0;
  // Sample switch (loop() -> interrupt): from stream frame switchAt on, the
  // stream holds another recording (or range); the start handshake's mapping
  // then becomes nextS0 / nextStartBeat / nextSpb. The old one plays up to it.
  volatile bool sampleSwitch = false;
  uint32_t switchAt = 0, nextS0 = 0, nextSpb = 0;
  int32_t nextStartBeat = 0;

  // Producer side (loop() only)
  uint16_t sample = 0;             // the sample file it plays (or records into); 0: none
  FsFile file;                     // that file
  FsFile unitFile;                 // where the track's settings are saved
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
  volatile int32_t spbAtBeat = -1;       // ... at this beat's start (a scene change), not at once
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
// Sets a track's filter points (steps). The width is clamped to 0..FILTER_MAX,
// then the pair shifted so they don't both lie outside 0..FILTER_MAX.
void setTrackFilter(int t, int lo, int hi);
float filterResponseDb(int lo, int hi, float hz);   // magnitude of the pair (analog approximation)
void setTrackMix(int t, int volume, int balance);   // steps (config.h), clamped
float volumeGain(int volume);                       // linear gain of a volume step
void takeTrackPeaks(int t, float &left, float &right);   // and start the next measurement

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
