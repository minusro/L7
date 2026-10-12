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
  // Volume (steps from unity, config.h; VOLUME_OFF: off) and balance (loop()
  // sets them with setTrackMix, on the clip's scale: its overview's peak): a 2x2 mix, out
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
  bool hpOn = false, lpOn = false;               // (wanted: each fades in / out, hpWet / lpWet, FILTER_FADE_MS)
  float hpHz = 0, lpHz = 0, hpTarget = 0, lpTarget = 0;   // their cutoffs: now (gliding, FILTER_GLIDE_MS), and set
  float hpWet = 0, lpWet = 0;                    // each one's share of the signal (0: unfiltered; interrupt only)
  float filterZ[2][2][2] = {};     // [hp/lp][left/right][state]; interrupt only, or loop() while not live
  // Waveform overview (loop() only): peak level per column across the loop,
  // 0-255 on a square-root scale, then the loop's peak (OVERVIEW_BYTES,
  // config.h). overviewVersion changes whenever it does.
  uint8_t overview[OVERVIEW_BYTES] = {};
  uint8_t overviewVersion = 0;

  // The fine rotate (TrackUnit): the clip turned this many frames later, its
  // audio as it was: every place played is that much earlier in the file.
  uint32_t rotate = 0;
  // The part of it in whole quarter beats (TrackUnit's steps): its audio
  // plays by rotate as a whole; shown, the waveform turns by the rest (the
  // ms offset), the range, start, playhead and bar lines move by these.
  uint16_t steps = 0;
  uint32_t rangeStartFrame() const {
    uint32_t loop = loopFrames(), f = rangeStartBeat * spbNative;
    return loop ? (f + loop - rotate % loop) % loop : f;
  }
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
  // The filters' level make-up (setTrackMix(): filterCompOf()), before the
  // mix (in float: the track's own headroom unlimited; the master limiter
  // catches the mix). comp set by loop(); compNow (gliding to it) the interrupt's.
  volatile float comp = 1.0f;
  float compNow = 1.0f;
  volatile bool stopping = false;
  volatile uint32_t underruns = 0;
  // Ran short and fell behind the clock (a snag): where the stream should be
  // at that beat (onBeat()); loop()'s produce() jumps there (the next beat's
  // snap then lines it up exactly).
  volatile bool resyncOn = false;
  volatile uint32_t resyncTo = 0;
  // Sample switch (loop() -> interrupt): from stream frame switchAt on, the
  // stream holds another recording (or range); the start handshake's mapping
  // then becomes nextS0 / nextStartBeat / nextSpb. The old one plays up to it.
  volatile bool sampleSwitch = false;
  uint32_t switchAt = 0, nextS0 = 0, nextSpb = 0;
  int32_t nextStartBeat = 0;
  // Time stretching (loop() sets stretch): the clip at its own pitch whatever
  // the tempo (else resampled: its pitch with the tempo). The playhead (tail,
  // frac) moves as ever; the sound is made of grains (STRETCH_GRAIN frames,
  // overlapping by half, Hann-windowed), each read at its own speed from
  // around where the playhead says it should be, placed where it best
  // continues the one before (WSOLA). Interrupt only: the two grains' stream
  // frames (at their starts) and positions in them (-1: none), the samples to
  // the next one; grainsOn false: set up afresh from the playhead.
  bool stretch = STRETCH_DEFAULT;
  bool grainsOn = false;
  // Grains 0-1 the two playing; 2-3 the two before a transient, fading out
  // (grainGain falling) after it; fadeLimit that transient (they play nothing from it on).
  uint32_t grainSrc[4] = {};
  int16_t grainPos[4] = {-1, -1, -1, -1};
  float grainGain[4] = {1, 1, 0, 0};
  uint32_t fadeLimit = 0;
  int16_t toNextGrain = 0;
  // Transients in the stream (findTransients(): loop() as it fills the ring):
  // the stream frames of the next ones, onsetIn written by loop(), onsetOut by
  // the interrupt as the playhead passes them (stretching: the grains start
  // afresh on each). The finder's state (loop() only): the windows' slow
  // average energy, the window being summed (its sum, frames in it, its first
  // stream frame), the last transient found.
  static constexpr uint32_t ONSET_QUEUE = 32;
  uint32_t onsets[ONSET_QUEUE] = {};
  volatile uint32_t onsetIn = 0, onsetOut = 0;
  float onsetSlow = 0, onsetSum = 0;
  uint32_t onsetFrames = 0, onsetWindow = 0, onsetLast = 0;

  // Producer side (loop() only)
  uint32_t sample = 0;             // the sample it plays (or records into), by id (storage.h); 0: none
  FsFile file;                     // that file
  FsFile unitFile;                 // where the track's settings are saved
  uint32_t fileFrame = 0;          // loop frame that stream frame `head` comes from

  // Queued loop range and fine rotate (loop() only): the stream switches to
  // them at stream frame switchS, which falls on transport quarter beat
  // switchQ (the first the stream hadn't buffered yet: the new data is read
  // after the old range's, a whole buffer ahead). The old range keeps playing
  // until then.
  bool rangePending = false;
  uint32_t pendingStartBeat = 0, pendingBeats = 0, pendingRotate = 0, switchS = 0;
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
  // loop() -> interrupt: the clock reads 1.1.1 from this beat on (a project
  // switching while playing): Stop, Song Position 0 and Continue sent at it,
  // before its first tick (INT32_MIN: none).
  volatile int32_t midiRestartBeat = INT32_MIN;
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
  // The input's latency it's taken with (inputLatency() when it was pressed:
  // the line input's, or USB's while USB audio comes in), kept to its end.
  uint32_t latency = LATENCY_FRAMES;
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
extern volatile bool inputMonitored;      // the input heard in the mix (app.h's setting; recorded either way)
extern uint32_t inputRing[INPUT_FRAMES];  // stereo input, index = sample % INPUT_FRAMES
extern uint32_t mixRing[MIX_FRAMES];      // the output as resampling records it (always), index = sample % MIX_FRAMES
// Tracks (bit per track) silenced from transport sample silenceFrom on: the
// others while a resampling's loop takes over from them. A track's bit
// clears when its stream switches clips (see takeSwitch); all clear on stop.
extern volatile uint8_t silencedTracks;
extern volatile uint32_t silenceFrom;
// The input latency a recording is shifted by (none for a resampling).
inline uint32_t recordingLatency() { return recording.resample ? 0 : recording.latency; }
// The input's latency, in samples: how late what's on the beat arrives in the
// input history. The line input's: LATENCY_FRAMES (the codec's round trip).
// USB's (while USB audio comes in): measured (serviceUsbLatency()): the
// computer's sharp sounds near our beats (its metronome, a kick) timed as they
// arrive, 8 beats in a row agreeing within ~2 ms (its own output's delay, a
// DAW's buffer, the USB buffers); before that, USB_LATENCY_GUESS.
extern volatile uint32_t usbInputLatency;
uint32_t inputLatency();
void serviceUsbLatency();                         // loop(): USB's latency measured

uint32_t spbForBpm(float bpm);
// The clock at the start of the latest audio block (the interrupt writes it;
// midi_in.cpp maps an incoming tick's micros() onto the beat with it).
struct BlockStamp { uint32_t us; int32_t beat; uint32_t sampleInBeat, spb; bool running; };
extern volatile BlockStamp blockStamp;
// Audio follow (audio_follow.cpp): while onsetListen, the interrupt measures
// the input (what's monitored and recorded: the line input and USB audio)
// once a block: how sharply it gets louder (the rise of its log energy, in
// three bands: lows, highs, all; ONSET_SCALE a unit), into onsetRing. Block n
// (onsetCount: blocks so far) started at transport.now onsetNow - (onsetCount
// - 1 - n) * AUDIO_BLOCK_SAMPLES.
constexpr uint32_t ONSET_FRAMES = 2048;                 // (~5.9 s)
constexpr float ONSET_SCALE = 1000.0f;
extern volatile bool onsetListen;
extern uint16_t onsetRing[ONSET_FRAMES];
extern volatile uint32_t onsetCount, onsetNow;
extern volatile float onsetLevel;                       // the input's loudness at the latest block's end (mean square, over ~10 ms)
// Our beats as they started (running): beat b's first sample (transport.now)
// and its samples a beat, for the last BEAT_LOG beats (index b % BEAT_LOG).
constexpr int BEAT_LOG = 8;
struct BeatStart { int32_t beat; uint32_t now, spb; };
extern volatile BeatStart beatLog[BEAT_LOG];
// Called from the MIDI timer (every 200 us): MIDI in read (midi_in.cpp).
extern void (*midiInPollHook)();
float bpmForSpb(uint32_t spb);
float filterStepHz(int step);
// Sets a track's filter points (steps). The width is clamped to 0..FILTER_MAX,
// then the pair shifted so they don't both lie outside 0..FILTER_MAX.
void setTrackFilter(int t, int lo, int hi);
float filterResponseDb(int lo, int hi, float hz);   // magnitude of the pair (analog approximation)
// The filters' level compensation: the gain bringing a clip filtered by the
// pair back to its unfiltered level, from its spectrum profile (config.h
// profileOf(); exactly 1: the filters open, or not measured), at most
// FILTER_COMP_MAX, then taken to FILTER_COMP_STRENGTH of it in dB.
float filterCompOf(int lo, int hi, const uint8_t *profile);
// A clip's spectrum profile: its energy in PROFILE_BANDS octave bands (20 Hz
// up), measured as its audio goes by (a band-pass each, both channels), kept as each
// band's level below the loudest in 3 dB steps (a nibble: 15, 45 dB or more).
struct SpectrumProfiler {
  float z[PROFILE_BANDS][2][2];
  double energy[PROFILE_BANDS];
  void reset();
  void feed(const uint32_t *frames, uint32_t n);
  void finish(uint8_t *profile) const;             // (PROFILE_BYTES)
};
void setTrackMix(int t, int volume, int balance);
bool limiterResting();                             // the master limiter not turning anything down (tests)
float takeLimiterReduction();                      // its most since the last call, dB (0: none)   // volume: steps from unity (config.h), balance: steps, clamped
// A clip's scale (config.h): from its overview (and peak, OVERVIEW_BYTES) the
// dB its peak is below 0 dBFS (0: quiet or empty, QUIET_PEAK), its peak
// (max |sample|), and the normalising factor for its overview's columns
// (square-root scale: 1 / sqrt(peak)).
float headroomDbOf(const uint8_t *overview);
uint16_t peakOf(const uint8_t *overview);
float overviewNormOf(const uint8_t *overview);
int unityStepOf(float headroomDb);                  // its unity step (1..VOLUME_MAX)
int volumeStepOf(int stored, float headroomDb);     // a track's stored volume as a step on the clip's scale (0: off)
int volumeStoredOf(int step, float headroomDb);     // and back (step 0: VOLUME_OFF)
float volumeGainOf(int stored, float headroomDb);   // its linear gain
void takeTrackPeaks(int t, float &left, float &right);   // and start the next measurement
// Master volume (config.h, steps 0..MASTER_VOLUME_MAX) and balance
// (-MASTER_BALANCE_MAX..MASTER_BALANCE_MAX, as a track's) on the final output (the click:
// the volume only), and that output's peaks (full scale 1) since the last call.
void setMasterVolume(int step);
void setMasterBalance(int balance);
int masterBalance();
// A metronome click on every beat while on (set from loop(): the clock
// running in a project without clips): a small wood block, higher on the
// bar's first beat. In the output, after the master volume; not in what a
// resampling records.
extern volatile bool metronomeOn;
extern volatile bool metronomeSteady;          // (loop()) the metronome set on, not auto: it clicks on through a recording's end
// The input: the line input and USB audio from a computer, mixed. While USB
// audio comes in, the line input is left out once it has been quiet (peaks
// under LINE_GATE_LEVEL) for LINE_GATE_HOLD_MS (nothing plugged in: its
// noise not mixed in), and in again as soon as it isn't; each a fade over a
// block. lineInPeak: its loudest block peak since loop() last took it, and
// lineInFloor its quietest (0..1; the serial log, to set the level).
extern volatile float lineInPeak, lineInFloor;
extern volatile bool usbAudioIn;                     // USB audio coming in (its last block)
// USB audio with a computer (the sketch sets these to the M8's own, usb_audio_l7
// in the patched core; the simulator to its own). Each called once a block:
//   usbAudioRead: the computer's audio for this block (AUDIO_BLOCK_SAMPLES stereo
//     frames, left | right << 16); false when none is coming in.
//   usbAudioWrite: room for this block to the computer (AUDIO_BLOCK_SAMPLES frames of
//     USB_OUT_CHANNELS samples, interleaved): the main mix (the master volume in it,
//     no metronome), then each track from the top row down, as heard in the mix
//     before the master volume (its volume, balance, filters, mute; the mix's
//     headroom), left and right; nullptr when not streaming.
//     usbAudioWritten() once written.
constexpr int USB_OUT_CHANNELS = 2 + 2 * NUM_TRACKS;
constexpr int usbTrackChannel(int t) { return 2 + 2 * (NUM_TRACKS - 1 - t); }   // track t's left (the top row: track NUM_TRACKS - 1)
extern bool (*usbAudioRead)(uint32_t *frames);
extern int16_t *(*usbAudioWrite)();
extern void (*usbAudioWritten)();
// No click before this transport beat (loop(): while a scene or project
// switch waits for its bar line, the switch's beat: silent meanwhile, the
// new one's first beat clicks if it should).
extern volatile int32_t clickFromBeat;
extern volatile const char *underrunStage;        // loop()'s step (debugStage) as a track last ran short: the dropout log
// A click now (the next block), as the metronome's on a beat (bar: the bar's
// first beat's): tap tempo's taps, stopped (input.cpp).
void metronomeClick(bool bar);
int masterVolume();
void takeOutputPeaks(float &left, float &right);
// Transients in a track's stream (stretching locks its grains to them): the
// frames written into its ring from stream frame `at` on (loop(), as it fills
// it); resetTransients(): the stream starts afresh at `at` (none queued; the
// track not live), dropTransientsFrom(): its frames from `at` on are dropped (a
// clip switch: the transients found there go too).
void findTransients(Track &tr, uint32_t at, const uint32_t *frames, uint32_t n);
extern bool transientLock;                         // (stretching locks its grains to them; off: plain WSOLA, for comparing)
void resetTransients(Track &tr, uint32_t at);
void dropTransientsFrom(Track &tr, uint32_t at);
// The audio interrupt's CPU use (the engine's update, its share of a block's
// time) since loop() last took it: the average and the most; 0 where it can't be counted.
void takeEngineLoad(float &avg, float &most);

void midiBegin(HardwareSerial *port);   // port already started (Board::midiOutBegin)
void midiSendNow(uint8_t status);        // transport start/stop messages (from loop() or the audio interrupt)
void midiSendPosition(int32_t beats);    // a Song Position (beats from 1.1.1)
// The same clock out for USB MIDI (midi_usb.cpp): attach once, then take what's
// due by micros() `now` (start / stop first, then the ticks due), at most max bytes.
void midiOutAttachUsb();
int midiOutTakeUsb(uint32_t now, uint8_t *out, int max);

class LooperEngine : public AudioStream {
public:
  // Inputs 0-1: the line input (USB audio from a computer mixed in: usbAudioRead).
  // Outputs 0-1: the line out and headphones (after the master volume, the
  // metronome in; to USB: usbAudioWrite).
  LooperEngine() : AudioStream(2, inputQueue_) {}
  float masterGain = 0.6493816f;                   // the tracks' mix, before the master volume (headroom): -3.75 dB, two volume steps
                                                   // (a resampling's clip, two steps up, as loud as what it recorded)
  void update() override;

private:
  audio_block_t *inputQueue_[2];
  float lineGain_ = 1;                              // the line input's level in the mix (the gate below)
  uint32_t lineQuietBlocks_ = 0;
  int tickInBeat_ = 0;
  uint32_t nextTickAt_ = 0;
  void onBeat(uint32_t blockMicros, int sampleIndex);
};

}  // namespace looper
