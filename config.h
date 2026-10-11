// config.h — looper constants
#pragma once
#include <stdint.h>

// Code that's rarely run (boot, the Ableton export) goes in flash, not the
// fast RAM (RAM1, full: its code grows in 32 KB banks). The simulator: as is.
#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace looper {

constexpr const char *APP_NAME = "L7";            // the app's name (exports are named after it)
constexpr int NUM_TRACKS = 7;
constexpr int BEATS_PER_BAR = 4;                   // fixed 4/4
constexpr uint32_t MAX_BARS = 32;                  // longest recording (at 60 BPM and over; below, half: storage.cpp maxBarsNow())
// Stopping a recording up to this many ms past a bar line trims it to end
// there; stopping later keeps recording to the end of that bar.
constexpr double SNAP_THRESHOLD_MS = 200.0;
// Every track file is created once at this size: 32 bars at 60 BPM (~22.6 MB).
// Below 60 BPM a recording stops at this length instead of 32 bars.
constexpr uint32_t MAX_REC_FRAMES = 128u * 44118u;

// Streaming buffers (stereo frames, packed as uint32: left low, right high).
// 64 KB per track covers ~370 ms of SD stall at 1x speed, ~185 ms at 2x
// (single writes of ~280 ms were seen on the M8's card).
constexpr uint32_t RING_FRAMES = 16384;
constexpr uint32_t CHUNK_FRAMES = 4096;            // 16 KB per SD call
// Time stretching (engine.h Track::stretch): its grains read up to this far
// behind the playhead, so the streamer leaves it there (fills STREAM_FRAMES ahead at most).
constexpr uint32_t STRETCH_HISTORY = 1024;            // (a grain reads at most ~515 frames back at half speed, ~710 at an eighth)
constexpr uint32_t STREAM_FRAMES = RING_FRAMES - STRETCH_HISTORY;
constexpr bool STRETCH_DEFAULT = true;             // new tracks stretch (keep their pitch) rather than resample
constexpr bool LOG_ENGINE_LOAD = true;             // the audio interrupt's CPU use on the serial log every 5 s
// Input history: the late press's grace (and the 128-frame pre-roll before
// it) + slack for SD write stalls (what's recorded waits in it for the card).
constexpr uint32_t INPUT_FRAMES = 24576;           // 96 KB, ~560 ms
// A record or resample press up to this long after a bar line starts there
// (from the history); stopping up to as long after one ends there (config
// SNAP_THRESHOLD_MS).
constexpr uint32_t START_GRACE_FRAMES = 8824;      // ~200 ms
// Mix history (resampling's late press): the output as resampling records it,
// the grace and the pre-roll's worth (copied into the input history then).
constexpr uint32_t MIX_FRAMES = 9216;              // 36 KB, ~209 ms

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


// Delete the recordings no project uses any more (deleteUnusedSamples(),
// scenes.cpp) whenever the menu opens. Off: they stay on the card (the function
// is kept, e.g. for a menu command).
constexpr bool DELETE_UNUSED_SAMPLES = false;

// Waveform overview per track: OVERVIEW_COLS peak values over the whole loop,
// stored in the WAV header. Peaks are gathered per PEAK_BIN_FRAMES while recording.
constexpr int OVERVIEW_COLS = 286;                 // one per pixel: x = 30..315
// In memory the overview is followed by the loop's peak (max |sample|, uint16
// little-endian; 0: not known, then taken from the overview), kept in the WAV
// header too.
constexpr int OVERVIEW_BYTES = OVERVIEW_COLS + 2;
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


// Per-track volume and balance. The volume cell has 5 lines per channel, each
// two 1-pixel halves: volume lights a half per step, balance takes a half
// off the other side per step. Each clip has its own scale (engine.h,
// volumeStepOf()): its top step, VOLUME_MAX, plays it peaking at 0 dBFS
// (normalised: its peak, kept with its overview); its unity step plays it as
// recorded, as many VOLUME_STEP_DB steps below the top as its peak is below
// 0 dBFS, rounded up (the step just below the top taking what's left), at
// least step 1; each step below that VOLUME_STEP_DB quieter; 0 off. A clip
// peaking under QUIET_PEAK (empty, or near silence): taken as peaking at
// 0 dBFS (its unity step the top). A track keeps its volume as steps from
// unity (VOLUME_UNITY: unity; VOLUME_OFF: off), so a clip switch keeps its
// level as recorded. Balance -BALANCE_MAX..
// BALANCE_MAX, centre 0 (the signal as it is): turning it towards one side
// moves that share of the other channel over to this one (linearly), so at the
// end this side carries the mono sum (L + R) and the other is silent.
constexpr int VOLUME_MAX = 20, VOLUME_UNITY = 16;    // (doubled: saved volumes from before, half as many steps, not converted)
constexpr int8_t VOLUME_OFF = -128;                // (a track's stored volume: off)
constexpr float VOLUME_STEP_DB = 1.875f;
constexpr uint16_t QUIET_PEAK = 130;               // ~-48 dBFS
// A recording whose samples all lie within EMPTY_SPAN of each other (silence,
// a constant level, a little noise) is empty: stepping through the samples
// (SHIFT+LEFT/RIGHT) skips it, and, stopped, deletes it if nothing uses it.
constexpr int EMPTY_SPAN = 256;
constexpr int BALANCE_MAX = 20;                    // (doubled: saved balances from before, half as many steps, not converted)
// Master volume and balance: the final output (tracks and monitored input),
// the clock cell's control a line per step (ui_status.cpp): volume
// 0..MASTER_VOLUME_MAX in MASTER_STEP_DB steps, MASTER_VOLUME_UNITY 100% (the
// default; the top +9.375 dB), balance
// -MASTER_BALANCE_MAX..MASTER_BALANCE_MAX (as a track's). Its meter: the same
// scale (the unity step's half: 0 dBFS).
constexpr int MASTER_VOLUME_MAX = 22, MASTER_VOLUME_UNITY = 17, MASTER_BALANCE_MAX = MASTER_VOLUME_MAX;   // (all the way: the other side's lines all off, at any volume)
constexpr float MASTER_STEP_DB = 1.875f;

// Declicking: loop and range edges, and clip switches, are crossfaded over
// this many frames (~3 ms); track levels (mute, volume, start, stop) glide
// with a ~1 ms time constant.
constexpr uint32_t XFADE_FRAMES = 128;
constexpr float GAIN_GLIDE_MS = 1.0f;
// The master volume and balance glide too, more slowly; a filter's cutoff
// glides (on a log scale, recalculated each block), and a filter switched on
// or off fades in or out (mixed with the signal unfiltered).
constexpr float MASTER_GLIDE_MS = 20.0f, FILTER_GLIDE_MS = 30.0f, FILTER_FADE_MS = 20.0f;

// WAV files keep their audio at a 512-byte offset so SD reads stay aligned.
constexpr uint32_t WAV_DATA_OFFSET = 512;

}  // namespace looper
