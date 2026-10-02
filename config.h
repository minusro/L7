// config.h — looper constants
#pragma once
#include <stdint.h>

namespace looper {

constexpr int NUM_TRACKS = 8;
constexpr int BEATS_PER_BAR = 4;                   // fixed 4/4
constexpr uint32_t MAX_BARS = 32;                  // longest recording
// Stopping a recording up to this many beats past a power-of-two length trims
// it to that length; stopping later keeps recording up to the next one.
constexpr double SNAP_THRESHOLD_BEATS = 2.0;
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

constexpr float MIN_BPM = 30.0f, MAX_BPM = 300.0f;
constexpr float MIN_SPEED = 0.5f, MAX_SPEED = 2.0f;  // vs each track's recorded tempo

// MIDI clock out (serial MIDI port 2 on variant B, see M8Board.h).
constexpr bool MIDI_OUT_ENABLED = true;

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

// Per-track volume and balance. Volume step v is (v - VOLUME_UNITY) *
// VOLUME_STEP_DB dB (off at 0): -40.5 dB to +6 dB. Balance -BALANCE_MAX..
// BALANCE_MAX, centre 0: like a stereo channel's balance pot, turning it one
// way fades the other side out (linearly, to silence at the end) and leaves
// this side as it is; the channels aren't mixed.
constexpr int VOLUME_MAX = 32, VOLUME_UNITY = 28;
constexpr float VOLUME_STEP_DB = 1.5f;
constexpr int BALANCE_MAX = 16;

// WAV files keep their audio at a 512-byte offset so SD reads stay aligned.
constexpr uint32_t WAV_DATA_OFFSET = 512;

}  // namespace looper
