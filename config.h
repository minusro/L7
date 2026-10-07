// config.h — looper constants
#pragma once
#include <stdint.h>

namespace looper {

constexpr const char *APP_NAME = "L7";            // the app's name (exports are named after it)
constexpr int NUM_TRACKS = 7;
constexpr int BEATS_PER_BAR = 4;                   // fixed 4/4
constexpr uint32_t MAX_BARS = 32;                  // longest recording
// Stopping a recording up to this many beats past a bar line trims it to end
// there; stopping later keeps recording to the end of that bar.
constexpr double SNAP_THRESHOLD_BEATS = 1.0;
// Every track file is created once at this size: 32 bars at 60 BPM (~22.6 MB).
// Below 60 BPM a recording stops at this length instead of 32 bars.
constexpr uint32_t MAX_REC_FRAMES = 128u * 44118u;

// Streaming buffers (stereo frames, packed as uint32: left low, right high).
// 64 KB per track covers ~370 ms of SD stall at 1x speed, ~185 ms at 2x
// (single writes of ~280 ms were seen on the M8's card).
constexpr uint32_t RING_FRAMES = 16384;
constexpr uint32_t CHUNK_FRAMES = 4096;            // 16 KB per SD call
// Input history: pre-roll for late record presses + slack for SD write stalls.
constexpr uint32_t INPUT_FRAMES = 32768;           // 128 KB, ~740 ms
constexpr uint32_t PREROLL_FRAMES = 11025;         // ~250 ms

// Measured round trip output -> input (audio_test): recordings are shifted
// earlier by this much so they line up with what was playing.
constexpr uint32_t LATENCY_FRAMES = 153;

// Tempo range. Loops play at any speed this allows vs their recorded tempo
// (1/8x to 8x); high ratios need that much more from the card.
constexpr float MIN_BPM = 30.0f, MAX_BPM = 240.0f;

// Loops up to this many bars: their range's start moves a beat at a time
// (input.cpp), and the start position is the range's first half bar or bar
// line (storage.cpp, startPosition()).
constexpr uint32_t BEAT_STEP_MAX_BARS = 8;

// MIDI clock out (serial MIDI port 2 on variant B, see M8Board.h).
constexpr bool MIDI_OUT_ENABLED = true;
// Following an external MIDI clock, start and stop (serial MIDI in, see
// midi_in.h; read from the MIDI out's timer: needs MIDI_OUT_ENABLED).
constexpr bool MIDI_IN_ENABLED = true;
// MIDI clock over USB too, in and out (midi_usb.h; the USB type with MIDI).
constexpr bool MIDI_USB_ENABLED = true;

// After a recording ends with a clip, select the next free track (the next
// empty one up, else the next muted one; input.cpp, followRecording()).
constexpr bool SELECT_NEXT_AFTER_RECORDING = false;

// Delete the recordings no project uses any more (deleteUnusedSamples(),
// scenes.cpp) whenever the menu opens. Off: they stay on the card (the function
// is kept, e.g. for a menu command).
constexpr bool DELETE_UNUSED_SAMPLES = false;

// Waveform overview per track: OVERVIEW_COLS peak values over the whole loop,
// stored in the WAV header. Peaks are gathered per PEAK_BIN_FRAMES while recording.
constexpr int OVERVIEW_COLS = 286;                 // one per pixel: x = 30..315
constexpr uint32_t PEAK_BIN_FRAMES = 4096;

// Per-track filters: a highpass at step `lo`, then a lowpass at step `hi`.
// Step k is FILTER_MIN_HZ * 2^(k * FILTER_OCTAVES / FILTER_MAX): 0..FILTER_MAX
// spans 20 Hz to ~20 kHz in third-octave steps. Either point may lie outside
// that range (the highpass is off at 0 and below, the lowpass at FILTER_MAX
// and above), but not both: hi >= 0 and lo <= FILTER_MAX, so the lowpass can
// close all the way down and the highpass all the way up. hi - lo is
// 0..FILTER_MAX.
constexpr int FILTER_MAX = 32;
constexpr float FILTER_MIN_HZ = 20.0f, FILTER_OCTAVES = 10.0f;

// Master volume: the final output (tracks and monitored input), steps
// 0..MASTER_VOLUME_MAX, one per line of the clock cell's volume control
// (ui_status.cpp). Step v is (v - MASTER_VOLUME_UNITY) * MASTER_STEP_DB dB:
// MASTER_VOLUME_UNITY is 100% (the default), the top step 200% (four steps of
// 1.5 dB above), 0 is off. Its meter has the same scale: line
// MASTER_VOLUME_UNITY is 0 dBFS (the ones above: clipping).
constexpr int MASTER_VOLUME_MAX = 21, MASTER_VOLUME_UNITY = 17;
constexpr float MASTER_STEP_DB = 1.5051500f;       // 20 log10(2) / 4

// Per-track volume and balance, in as many steps as the volume cell has lines
// per channel (each step shows). Volume step v is (v - VOLUME_UNITY) *
// VOLUME_STEP_DB dB (off at 0): -22.5 dB to +7.5 dB. Balance -BALANCE_MAX..
// BALANCE_MAX (twice as many steps), centre 0 (the signal as it is): turning
// it towards one side moves that share of the other channel over to this one
// (linearly), so at the end this side carries the mono sum (L + R) and the
// other is silent.
constexpr int VOLUME_MAX = 5, VOLUME_UNITY = 4;
constexpr float VOLUME_STEP_DB = 7.5f;
constexpr int BALANCE_MAX = 5;

// Declicking: loop and range edges, and clip switches, are crossfaded over
// this many frames (~3 ms); track levels (mute, volume, start, stop) glide
// with a ~1 ms time constant.
constexpr uint32_t XFADE_FRAMES = 128;
constexpr float GAIN_GLIDE_MS = 1.0f;

// WAV files keep their audio at a 512-byte offset so SD reads stay aligned.
constexpr uint32_t WAV_DATA_OFFSET = 512;

}  // namespace looper
