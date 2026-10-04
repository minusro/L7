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

// The palette: black, three greys, white, and the accent colour (and its dark version).
constexpr uint16_t BG = rgb565(0, 0, 0);            // black: the background
constexpr uint16_t DARK = rgb565(44, 44, 44);       // 17% grey: in the background (outside the loop range,
                                                    //   muted volume lines and filters)
constexpr uint16_t DIM = rgb565(112, 112, 112);     // 44% grey: inactive (muted clips, idle icons and text)
constexpr uint16_t LIGHT = rgb565(208, 208, 208);   // 81% grey: light grey (unselected waveforms, labels, the selected filter)
constexpr uint16_t INK = rgb565(255, 255, 255);     // white: what's active, selected, playing
constexpr uint16_t ACCENT = rgb565(48, 230, 170);   // green turquoise: recording, and a scene change on its way
constexpr uint16_t ACCENT_DARK = rgb565(24, 120, 72);   // the accent at about half brightness, a little greener: light grey's while editing

// The project slots' colours (the menu): eight hues a perceptual step apart
// (OKLCH, 45 degrees), red to pink, at the accent's lightness and chroma (the
// accent the 4th); hues that can't reach its chroma at that lightness as
// colourful as they can be.
constexpr uint16_t PROJECT_COLORS[8] = {
    rgb565(255, 173, 160), rgb565(255, 181, 62), rgb565(185, 212, 74), ACCENT,
    rgb565(0, 222, 248), rgb565(158, 201, 255), rgb565(207, 182, 255), rgb565(255, 164, 216)};

// ---- Roles
constexpr uint16_t PLAYHEAD = ACCENT;        // playing clips
constexpr uint16_t MUTED_HEAD = BG;          // the selected muted clip (a cut through its waveform)

// ---- Boot / errors
constexpr uint16_t TEXT_PROGRESS = 0xFFE0;   // yellow: startup messages
constexpr uint16_t LOGO_COLOR = ACCENT;      // the splash screen's logo
constexpr uint16_t SPLASH_TEXT = LIGHT;      // and its copyright line
constexpr uint16_t TEXT_ERROR = 0xF800;      // red: fatal errors
