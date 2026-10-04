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
// The accent colour: the loaded project's (setAccent(), ui.cpp); green
// turquoise until one is loaded (the splash screen). Recording, a scene change
// on its way, the action button, playheads. ACCENT_DARK: at half brightness
// (light grey's while editing).
inline uint16_t ACCENT = rgb565(48, 230, 170);
inline uint16_t ACCENT_DARK = rgb565(24, 120, 72);

// The project slots' colours (the menu), pastels: rose, peach, apricot,
// butter, pistachio, mint, seafoam, sky, periwinkle, lavender, lilac, blush.
constexpr uint16_t PROJECT_COLORS[12] = {
    rgb565(0xF4, 0xB6, 0xC2), rgb565(0xF8, 0xC8, 0xA8), rgb565(0xFA, 0xD9, 0xA1), rgb565(0xF7, 0xED, 0xA3),
    rgb565(0xD6, 0xEB, 0xA8), rgb565(0xB5, 0xE3, 0xC4), rgb565(0xA8, 0xE0, 0xD8), rgb565(0xA9, 0xD6, 0xEE),
    rgb565(0xB7, 0xC4, 0xF0), rgb565(0xCD, 0xB8, 0xEE), rgb565(0xE3, 0xB5, 0xE6), rgb565(0xF2, 0xB8, 0xD8)};

// ---- Roles
constexpr uint16_t MUTED_HEAD = BG;          // the selected muted clip (a cut through its waveform)

// ---- Boot / errors
constexpr uint16_t TEXT_PROGRESS = 0xFFE0;   // yellow: startup messages
constexpr uint16_t LOGO_COLOR = rgb565(48, 230, 170);   // the splash screen's logo (the accent before a project is loaded)
constexpr uint16_t SPLASH_TEXT = LIGHT;      // and its copyright line
constexpr uint16_t TEXT_ERROR = 0xF800;      // red: fatal errors
