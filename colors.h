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

// The palette: white on black, one dim level. Patterns (every other block)
// stand in for in-between shades.
constexpr uint16_t BG = 0x0000;              // black
constexpr uint16_t INK = 0xFFFF;             // white: what's active, selected, playing
constexpr uint16_t DIM = 0x630C;             // grey ~99: inactive, muted, outside the loop range
constexpr uint16_t REC = 0xF800;             // red: the record icon on the status button (A records)

// ---- Roles
constexpr uint16_t DIVIDER = DIM;            // line above the status row
constexpr uint16_t PLAYHEAD = INK;           // playing clips (drawn inverted over the waveform)
constexpr uint16_t MUTED_HEAD = DIM;         // the selected muted clip

// ---- Panel (B+A)
constexpr uint16_t PANEL_SHADE = BG;         // every other pixel of the screen behind it
constexpr uint16_t PANEL_BG = BG;
constexpr uint16_t PANEL_BORDER = INK;

// ---- Boot / errors
constexpr uint16_t TEXT_PROGRESS = 0xFFE0;   // yellow: startup messages
constexpr uint16_t LOGO_COLOR = INK;         // the splash screen's logo
constexpr uint16_t TEXT_ERROR = 0xF800;      // red: fatal errors
