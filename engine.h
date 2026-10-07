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
  // Level glide (interrupt only): the mix gains actually applied, moving
  // towards the targets; 0 while not live. stopping (loop() -> interrupt):
  // glide to silence, then go off (live false).
  float curMix[4] = {};
  volatile bool stopping = false;
  volatile uint32_t underruns = 0;
  // Sample switch (loop() -> interrupt): from stream frame switchAt on, the
  // stream holds another recording (or range); the start handshake's mapping
  // then becomes nextS0 / nextStartBeat / nextSpb. The old one plays up to it.
  volatile bool sampleSwitch = false;
  uint32_t switchAt = 0, nextS0 = 0, nextSpb = 0;
  int32_t nextStartBeat = 0;

  // Producer side (loop() only)
  uint32_t sample = 0;             // the sample it plays (or records into), by id (storage.h); 0: none
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
  // loop() -> interrupt: the transport started part way into its beat
  // (sampleInBeat > 0: an external clock's position, midi_in.cpp): the tracks
  // armed for this beat join where they'd be, in the next block.
  volatile bool joinMidBeat = false;
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
  // Resampling: the app's output is recorded instead of the input (the
  // interrupt puts it in inputRing while this recording is on; no latency).
  volatile bool resample = false;
  // The clock while it records (the tempo may change meanwhile): from its
  // first bar line, a mark at every tempo change: the transport sample, the
  // beat position there and the samples per beat from there on. The clip is
  // written as it comes; the marks give where its bars end (its length) and
  // each bar line, kept in its header for the Ableton export's warp markers;
  // it plays at its average tempo (storage.cpp). The interrupt adds the marks.
  struct TempoMark { uint32_t sample; double beat; uint32_t spb; };
  static constexpr int TEMPO_MARKS = 32;
  TempoMark marks[TEMPO_MARKS];
  volatile int markCount = 0;
};
// The tempo marks begun (a recording starting): its first bar line, at
// transport sample `sample` (interrupts off, or from the interrupt).
void beginTempoMarks(uint32_t sample, int32_t beat);

extern Track tracks[NUM_TRACKS];
extern Transport transport;
extern Recording recording;
extern uint32_t inputRing[INPUT_FRAMES];  // stereo input, index = sample % INPUT_FRAMES
// Tracks (bit per track) silenced from transport sample silenceFrom on: the
// others while a resampling's loop takes over from them. A track's bit
// clears when its stream switches clips (see takeSwitch); all clear on stop.
extern volatile uint8_t silencedTracks;
extern volatile uint32_t silenceFrom;
// The input latency a recording is shifted by (none for a resampling).
inline uint32_t recordingLatency() { return recording.resample ? 0 : LATENCY_FRAMES; }

uint32_t spbForBpm(float bpm);
// The clock at the start of the latest audio block (the interrupt writes it;
// midi_in.cpp maps an incoming tick's micros() onto the beat with it).
struct BlockStamp { uint32_t us; int32_t beat; uint32_t sampleInBeat, spb; bool running; };
extern volatile BlockStamp blockStamp;
// Called from the MIDI timer (every 200 us): MIDI in read (midi_in.cpp).
extern void (*midiInPollHook)();
float bpmForSpb(uint32_t spb);
float filterStepHz(int step);
// Sets a track's filter points (steps). The width is clamped to 0..FILTER_MAX,
// then the pair shifted so they don't both lie outside 0..FILTER_MAX.
void setTrackFilter(int t, int lo, int hi);
float filterResponseDb(int lo, int hi, float hz);   // magnitude of the pair (analog approximation)
void setTrackMix(int t, int volume, int balance);   // steps (config.h), clamped
float volumeGain(int volume);                       // linear gain of a volume step
void takeTrackPeaks(int t, float &left, float &right);   // and start the next measurement
// Master volume (config.h, steps 0..MASTER_VOLUME_MAX) on the final output,
// and that output's peaks (full scale 1) since the last call.
void setMasterVolume(int step);
// A metronome click on every beat while on (set from loop(): the clock
// running in a project without clips): a small wood block, higher on the
// bar's first beat. In the output, after the master volume; not in what a
// resampling records.
extern volatile bool metronomeOn;
// The input: the line input and USB audio from a computer, mixed. While USB
// audio comes in, the line input is left out once it has been quiet (peaks
// under LINE_GATE_LEVEL) for LINE_GATE_HOLD_MS (nothing plugged in: its
// noise not mixed in), and in again as soon as it isn't; each a fade over a
// block. lineInPeak: its loudest block peak since loop() last took it, and
// lineInFloor its quietest (0..1; the serial log, to set the level).
extern volatile float lineInPeak, lineInFloor;
extern volatile bool usbAudioIn;                     // USB audio coming in (its last block)
// No click before this transport beat (loop(): while a scene or project
// switch waits for its bar line, the switch's beat: silent meanwhile, the
// new one's first beat clicks if it should).
extern volatile int32_t clickFromBeat;
int masterVolume();
void takeOutputPeaks(float &left, float &right);

void midiBegin(HardwareSerial *port);   // port already started (Board::midiOutBegin)
void midiSendNow(uint8_t status);        // transport start/stop messages
// The same clock out for USB MIDI (midi_usb.cpp): attach once, then take what's
// due by micros() `now` (start / stop first, then the ticks due), at most max bytes.
void midiOutAttachUsb();
int midiOutTakeUsb(uint32_t now, uint8_t *out, int max);

class LooperEngine : public AudioStream {
public:
  // Inputs 0-1: the line input; 2-3: USB audio from a computer (mixed in).
  LooperEngine() : AudioStream(4, inputQueue_) {}
  float masterGain = 0.7f;                         // the tracks' mix, before the master volume (headroom)
  bool monitor = true;
  void update() override;

private:
  audio_block_t *inputQueue_[4];
  float lineGain_ = 1;                              // the line input's level in the mix (the gate below)
  uint32_t lineQuietBlocks_ = 0;
  int tickInBeat_ = 0;
  uint32_t nextTickAt_ = 0;
  void onBeat(uint32_t blockMicros, int sampleIndex);
};

}  // namespace looper
