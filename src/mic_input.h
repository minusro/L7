// mic_input.h — the Model:02's built-in mic as the input (docs/HARDWARE.md:
// on the codec's mic input, with the stock firmware's settings).
//
// The codec has one ADC, on the line input or on the mic, never both, and
// nothing on the board is known to sense a cable in the line-in jack (the
// stock firmware reads none; src/m02_diag.h looks for one on a device). So
// the AUDIO IN setting (app.h) decides: LINE or MIC as said, or AUTO - the
// line input until it has been silent for lineGateHoldMs (the same rule that
// takes it out of the mix while USB audio comes in), then the mic; while on
// the mic, a peek at the line input every PEEK_EVERY_MS (PEEK_MS long), back
// to it as soon as one hears anything. A cable plugged into a silent source
// counts as nothing plugged in.
//
// The engine (engine.h inputSource) takes it from there: the mic recorded
// but never heard, USB audio heard and recorded with it, a resampling (it
// records the mix) without it. The codec is only switched between
// recordings, and a peek never lands near a bar line, where a recording or a
// late press's grace starts. The recording track shows a mic in its button
// meanwhile (ui.h micIsInput).
//
// Header-only and included from L7.ino alone (the codec is there): the
// Model:01's build gains nothing, not even an empty object file.
#pragma once
#include "screen.h"
#if M8_MODEL == 2
#include "app.h"
#include "ui.h"
#include <Audio.h>

extern AudioControlSGTL5000 codec;

namespace micinput {

constexpr uint8_t SETTLE_BLOCKS = 12;               // the codec's input left out after a switch (~35 ms)
constexpr uint32_t PEEK_EVERY_MS = 1000;            // while on the mic (AUTO): how often the line input is checked,
constexpr uint32_t PEEK_MS = 120;                   // for how long (its settling included)
constexpr uint32_t BAR_CLEAR_MS = 250;              // and never this close to a bar line, either side

inline bool onMic = false;                          // the mic is the input (a peek still counts as on it)
inline bool peeking = false;
inline uint32_t peekAt = 0, nextPeekAt = 0;

// The codec to an input, its settling left out first.
inline void codecTo(uint8_t source) {
  inputHold = SETTLE_BLOCKS;
  inputSource = source;
  if (source == INPUT_MIC) {                        // the stock Model:02's: +30 dB preamp, bias on, +12 dB ADC gain
    codec.inputSelect(AUDIO_INPUT_MIC);
    codec.micGain(30);
    codec.lineInLevel(8, 8);
  } else {                                          // the line input as L7 has always set it
    codec.inputSelect(AUDIO_INPUT_LINEIN);
    codec.lineInLevel(5);
  }
}

// Nothing recording, or about to.
inline bool idle() { return recording.mode == Recording::Idle && recordQueued() < 0; }

// Far enough from a bar line for a peek, before and after (stopped: always).
inline bool barClear() {
  if (!transport.running) return true;
  const uint32_t spb = transport.spb;
  if (!spb) return false;
  const int32_t beat = transport.beat;
  const uint64_t bar = (uint64_t)spb * BEATS_PER_BAR;
  const uint64_t into = (uint64_t)(((beat % BEATS_PER_BAR) + BEATS_PER_BAR) % BEATS_PER_BAR) * spb + transport.sampleInBeat;
  const uint64_t clear = (uint64_t)(BAR_CLEAR_MS * AUDIO_SAMPLE_RATE_EXACT / 1000);
  const uint64_t peek = (uint64_t)(PEEK_MS * AUDIO_SAMPLE_RATE_EXACT / 1000);
  return into >= clear && bar - into >= clear + peek;
}

inline void toMic() {
  onMic = true;
  peeking = false;
  codecTo(INPUT_MIC);
  nextPeekAt = millis() + PEEK_EVERY_MS;
}
inline void toLine() {
  onMic = false;
  peeking = false;
  codecTo(INPUT_LINE);
}

}  // namespace micinput

// loop(): the input kept as AUDIO IN says, and the mark shown.
inline void micInputService() {
  using namespace micinput;
  const int mode = audioIn();
  const uint32_t now = millis();
  if (peeking) {                                    // a peek: a recording wanted meanwhile cuts it short
    if (!idle()) {
      toMic();
    } else if (now - peekAt >= PEEK_MS) {
      if (peekPeak >= lineGateLevel && mode == AUDIO_IN_AUTO) {   // something playing into the line input: on it
        onMic = false;
        peeking = false;
        inputSource = INPUT_LINE;                   // (the codec on it already, settled)
      } else {
        toMic();
      }
    }
  } else if (idle()) {                              // (the codec switched only between recordings)
    const uint32_t quietMs = (uint32_t)((uint64_t)lineQuietBlocks * AUDIO_BLOCK_SAMPLES * 1000 / AUDIO_SAMPLE_RATE_EXACT);
    const bool wantMic = mode == AUDIO_IN_MIC || (mode == AUDIO_IN_AUTO && (onMic || quietMs >= lineGateHoldMs));
    if (wantMic && !onMic) toMic();
    else if (!wantMic && onMic) toLine();
    else if (onMic && mode == AUDIO_IN_AUTO && (int32_t)(now - nextPeekAt) >= 0 && barClear()) {
      peeking = true;                               // a peek at the line input: anything playing into it?
      peekAt = now;
      peekPeak = 0;
      codecTo(INPUT_PEEK);
    }
  }
  micIsInput = onMic;
}

#endif  // M8_MODEL == 2
