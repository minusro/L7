// colors.h — every colour the looper draws with (RGB565), by role.
// Grey levels in the comments are 0-255 per channel.
#pragma once
#include <stdint.h>

// Blends two RGB565 colours: a at weight w (0-255), b at 255 - w.
constexpr uint16_t blend565(uint16_t a, uint16_t b, uint8_t w) {
  return (uint16_t)(((((a >> 11) & 31) * w + ((b >> 11) & 31) * (255 - w)) / 255) << 11 |
                    ((((a >> 5) & 63) * w + ((b >> 5) & 63) * (255 - w)) / 255) << 5 |
                    (((a & 31) * w + (b & 31) * (255 - w)) / 255));
}

// ---- Backgrounds
constexpr uint16_t BG = 0x0000;              // black: the screen, unselected rows
constexpr uint16_t GAP = 0x0000;             // black: the 1 px gaps between cells
constexpr uint16_t BG_SELECTED = 0x1082;     // grey ~16: the selected row's current cell
constexpr uint16_t BG_ROW = 0x0841;          // grey ~8: the selected row's other cells and button
constexpr uint16_t BG_RECORDING = 0x3800;    // dark red: the waveform cell of a recording track
constexpr uint16_t DIVIDER = 0x1082;         // grey ~16: line under the last track row

// ---- Waveforms
constexpr uint16_t WAVE_IN = 0xCE79;         // grey ~206 (80% white): inside the loop range
constexpr uint16_t WAVE_IN_BAR = 0x9CD3;     // grey ~154: bar lines inside the loop range
constexpr uint16_t WAVE_OUT = 0x2104;        // grey ~33: outside the loop range
constexpr uint16_t WAVE_OUT_BAR = 0x18C3;    // grey ~25: bar lines outside the loop range
constexpr uint8_t MUTED_WEIGHT = 128;        // muted clips: loop range (and filter curve) at half brightness

// ---- Playheads
constexpr uint16_t PLAYHEAD = 0x6B4D;        // grey ~107: playing clips
constexpr uint16_t REC_HEAD = 0xF800;        // red: the recording position
constexpr uint16_t MUTED_HEAD = 0x0000;      // black: the selected muted clip

// ---- Row buttons
constexpr uint16_t ICON_SELECTED = 0xFFFF;   // white: icons on the selected row
constexpr uint16_t ICON_RECORD_COLOR = 0xF800;   // red: record
constexpr uint16_t ICON_TOGGLE_IDLE = WAVE_OUT;  // grey ~33: track on/off on unselected rows
constexpr uint16_t ICON_EMPTY_COLOR = 0x1082;    // grey ~16: empty tracks on unselected rows
constexpr uint16_t ICON_TRANSPORT_IDLE = 0x7BEF; // grey ~123: play/stop when the status row isn't selected

// ---- Filter curve
constexpr uint16_t CURVE = WAVE_IN;          // playing clips (muted: MUTED_WEIGHT)
constexpr uint16_t CURVE_EMPTY = WAVE_OUT;   // empty tracks
constexpr uint8_t CURVE_FILL_WEIGHT = 48;    // fill under the curve: curve colour over the background

// ---- Status row text
constexpr uint16_t TEXT_ACTIVE = 0xFFFF;     // white: tempo being edited, clock running
constexpr uint16_t TEXT_IDLE = 0x7BEF;       // grey ~123: tempo not selected, clock stopped

// ---- Boot / errors
constexpr uint16_t TEXT_PROGRESS = 0xFFE0;   // yellow: startup messages
constexpr uint16_t TEXT_ERROR = 0xF800;      // red: fatal errors
