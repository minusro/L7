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

// RGB565 from 8-bit red, green, blue.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

// The palette: black, two greys, white, and one accent colour.
constexpr uint16_t BG = rgb565(0, 0, 0);            // black: the background
constexpr uint16_t DARK = rgb565(32, 32, 32);       // 12.5% grey: in the background (outside the loop range,
                                                    //   the row's other frames, fills, unlit arcs)
constexpr uint16_t DIM = rgb565(128, 128, 128);     // 50% grey: inactive (muted clips, idle icons and text)
constexpr uint16_t LIGHT = rgb565(192, 192, 192);   // 75% grey: the selection's markers while B or A is held
constexpr uint16_t INK = rgb565(255, 255, 255);     // white: what's active, selected, playing
constexpr uint16_t ACCENT = rgb565(255, 150, 0);    // amber: recording, and a scene change on its way

// ---- Roles
constexpr uint16_t DIVIDER = DARK;           // line above the status row
constexpr uint16_t PLAYHEAD = ACCENT;        // playing clips
constexpr uint16_t MUTED_HEAD = BG;          // the selected muted clip (a cut through its waveform)

// ---- Panel (B+A)
constexpr uint16_t PANEL_SHADE = BG;         // every other pixel of the screen behind it
constexpr uint16_t PANEL_BG = BG;
constexpr uint16_t PANEL_BORDER = INK;

// ---- Boot / errors
constexpr uint16_t TEXT_PROGRESS = 0xFFE0;   // yellow: startup messages
constexpr uint16_t LOGO_COLOR = INK;         // the splash screen's logo
constexpr uint16_t TEXT_ERROR = 0xF800;      // red: fatal errors
