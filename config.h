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

// WAV files keep their audio at a 512-byte offset so SD reads stay aligned.
constexpr uint32_t WAV_DATA_OFFSET = 512;

}  // namespace looper
