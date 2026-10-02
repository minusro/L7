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
constexpr uint16_t BG_SELECTED = 0x2104;     // grey ~33: the selected row's current cell
constexpr uint16_t BG_ROW = 0x1082;          // grey ~16: the selected row's other cells and button
constexpr uint16_t BG_RECORDING = 0x5800;    // dark red (~90): the waveform cell of a recording track
constexpr uint16_t DIVIDER = 0x2104;         // grey ~33: line under the last track row

// ---- Waveforms
constexpr uint16_t WAVE_IN = 0xCE79;         // grey ~206 (80% white): inside the loop range
constexpr uint16_t WAVE_IN_BAR = 0x9CD3;     // grey ~154: bar lines inside the loop range
constexpr uint16_t WAVE_OUT = 0x4A49;        // grey ~74: outside the loop range
constexpr uint16_t START_MARK = 0xFFFF;      // white: the start position flag and the range edge lines (muted clips: MUTED_WEIGHT)
constexpr uint16_t WAVE_OUT_BAR = 0x39C7;    // grey ~58: bar lines outside the loop range
constexpr uint8_t MUTED_WEIGHT = 160;        // muted clips: loop range (and filter curve) at ~63% brightness

// ---- Playheads
// A playhead is two 1 px lines: its colour at the position, PLAYHEAD_EDGE on its left.
constexpr uint16_t PLAYHEAD = 0xFFFF;        // white: playing clips
constexpr uint16_t REC_HEAD = 0xF800;        // red: the recording position
constexpr uint16_t MUTED_HEAD = blend565(START_MARK, BG, MUTED_WEIGHT);   // the selected muted clip (a single line, no edge): as its range lines
constexpr uint16_t PLAYHEAD_EDGE = 0x0000;   // black: the line on its left

// ---- Row buttons
constexpr uint16_t ICON_SELECTED = 0xFFFF;   // white: icons on the selected row
constexpr uint16_t ICON_RECORD_COLOR = 0xF800;   // red: record, on the status row (A records)
constexpr uint16_t ICON_IDLE = 0x630C;           // grey ~99: track state icons on unselected rows
constexpr uint8_t ICON_UNAVAILABLE_WEIGHT = 48;  // an action with nothing to act on (paste, clipboard empty): ~19%
constexpr uint8_t ICON_PRESSED_WEIGHT = 96;      // the status row's icon while A is held: dimmed to ~38%

// ---- Filter curve
constexpr uint16_t CURVE = WAVE_IN;          // playing clips (muted: MUTED_WEIGHT)
constexpr uint8_t CURVE_FILL_WEIGHT = 64;    // fill under the curve: curve colour over the background

// ---- Volume / balance cell: concentric arcs, ((( for the left channel, )))
// for the right
constexpr uint16_t MIX_ARC_OFF = 0x39C7;     // grey ~58: above the channel's level
constexpr uint16_t MIX_ARC_LEVEL = 0x8C51;   // grey ~140: up to the channel's level (volume and balance; muted: MUTED_WEIGHT)
constexpr uint16_t MIX_ARC_METER = 0xFFFF;   // white: the meter, the channel's actual output level

// ---- Status row text
constexpr uint16_t TEXT_ACTIVE = 0xFFFF;     // white: tempo being edited, clock running
constexpr uint16_t TEXT_IDLE = 0x8C51;       // grey ~140: tempo not selected, clock stopped

// ---- Boot / errors
constexpr uint16_t TEXT_PROGRESS = 0xFFE0;   // yellow: startup messages
constexpr uint16_t TEXT_ERROR = 0xF800;      // red: fatal errors
