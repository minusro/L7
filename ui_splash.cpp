// ui_splash.cpp — the logo shown while the looper starts (see ui.h): "L7",
// italic, the 7's right edge one 45-degree line up to a sharp corner, its
// diagonal thick; near the bottom two cuts, each as thick as the line under
// it (80s; the L's foot starts above them). Drawn in full resolution (pixels,
// not blocks) from its shape (drawL7()): 110 x 52 px here, in the middle of the
// screen, 55 x 26 in the menu's corner (drawSmallLogo()). It fades in from
// black, then its colour goes once round all hues (from the green turquoise
// accent back to it, eased in and out), fading out at the end, with the
// copyright line under it in light grey (fading in), in the pixel font
// (ui_font.cpp, 2x2-pixel blocks).
#include "ui.h"
#include "ui_font.h"

// The logo's shape: 55 x 26 units, drawn at k pixels a unit (top left at x,
// y), a row of pixels at a time: the italic slant a pixel every three rows,
// the 7's right edge a pixel a row (45 degrees), whatever the size. In units:
// the L's stem 7 wide, its foot 17 wide from row 16 down; the 7's top line 7
// tall from column 21, its diagonal 10 wide; the cuts rows 18-19 and 22-23.
static constexpr int L7_W = 55, L7_H = 26;
static void drawL7(int x, int y, int k, uint16_t color) {
  auto &t = board.tft();
  const int h = L7_H * k;
  for (int p = 0; p < h; p++) {
    if ((p >= 18 * k && p < 20 * k) || (p >= 22 * k && p < 24 * k)) continue;   // the cuts
    int off = (h - 1 - p) / 3;                                  // italic
    t.fillRect(x + off, y + p, (p >= h - 10 * k ? 17 : 7) * k, 1, color);   // the L: its stem, then its foot
    int right = L7_W * k - p;                                   // the 7: its right edge
    int left = p < 7 * k ? 21 * k + off : right - 10 * k;        // its top line, then its diagonal
    t.fillRect(x + left, y + p, right - left, 1, color);
  }
}

static constexpr int LOGO_SCALE = 2, LOGO_PIXEL = 2;  // the logo's pixels a unit; the copyright line's font blocks
// The logo and the copyright line under it centred together (the logo
// above the middle).
static constexpr int TEXT_GAP = 24;               // logo to text, px
static constexpr int GROUP_H = L7_H * LOGO_SCALE + TEXT_GAP + 6 * LOGO_PIXEL;   // logo, gap, text (to the copyright sign's bottom)
static constexpr int GROUP_DOWN = 8;                 // the group sits this far below the middle, px
static constexpr int LOGO_X = (320 - L7_W * LOGO_SCALE) / 2,
                     LOGO_Y = (240 - GROUP_H) / 2 / LOGO_PIXEL * LOGO_PIXEL + GROUP_DOWN;
// The splash's animation, in ms from showSplash(): the logo fades in from
// black (0-500), then its colour goes once round the hues (500-2500); the
// copyright line fades in (1500-2000); both fade out (3000-3500). Shown at
// least this long.
static constexpr uint32_t LOGO_FADE_MS = 500, HUE_MS = 2000, TEXT_FROM_MS = 1500, TEXT_FADE_MS = 500;
static constexpr uint32_t OUT_FROM_MS = 3000, OUT_MS = 500;
static constexpr uint32_t SPLASH_MIN_MS = OUT_FROM_MS + OUT_MS;
static uint32_t shownAt = 0;

static const char COPYRIGHT[] = "\xA9 2026 Daniel Stanciu";

// Width of a line of text, in blocks.
static int textCols(const char *text) {
  int cols = 0;
  for (const char *p = text; *p; p++)
    if (const Glyph *g = glyphFor(*p)) cols += glyphWidth(*g) + (p[1] ? 1 : 0);
  return cols;
}

static void drawLine(int x, int y, const char *text, uint16_t color) {   // (the copyright sign reaches a row above y)
  auto &t = board.tft();
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    int w = glyphWidth(*g);
    for (int r = 0; r < 7 && g->rows[r]; r++)
      for (int c = 0; c < w; c++)
        if (g->rows[r][c] == '#')
          t.fillRect(x + c * LOGO_PIXEL, y + (g->top + r) * LOGO_PIXEL, LOGO_PIXEL, LOGO_PIXEL, color);
    x += (w + 1) * LOGO_PIXEL;
  }
}

static constexpr int TEXT_Y_SPLASH = LOGO_Y + L7_H * LOGO_SCALE + TEXT_GAP;

int splashBottom() { return TEXT_Y_SPLASH + 6 * LOGO_PIXEL; }   // (the copyright sign's last row)

// OKLCH (lightness, chroma, hue in degrees) to RGB565; a chroma out of sRGB's
// reach at that lightness comes down until it fits.
static uint16_t oklch565(float L, float C, float hDeg) {
  float h = hDeg * (float)M_PI / 180, r = 0, g = 0, b = 0;
  for (int i = 0; i < 12; i++, C *= 0.9f) {
    float a = C * cosf(h), bb = C * sinf(h);
    float l = L + 0.3963377774f * a + 0.2158037573f * bb, m = L - 0.1055613458f * a - 0.0638541728f * bb,
          s = L - 0.0894841775f * a - 1.2914855480f * bb;
    l = l * l * l; m = m * m * m; s = s * s * s;
    r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    if (r >= -0.001f && r <= 1.001f && g >= -0.001f && g <= 1.001f && b >= -0.001f && b <= 1.001f) break;
  }
  auto gamma = [](float c) {
    c = constrain(c, 0.0f, 1.0f);
    return (uint8_t)lroundf(255 * (c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1 / 2.4f) - 0.055f));
  };
  return rgb565(gamma(r), gamma(g), gamma(b));
}

static void drawLogo(uint16_t color) { drawL7(LOGO_X, LOGO_Y, LOGO_SCALE, color); }

// The logo small, for the menu's corner: 55 x 26 px (the status row's height).
void drawSmallLogo(int x, int y, uint16_t color) { drawL7(x, y, 1, color); }

static int copyrightX = 0;

// The splash now: the logo fading in, then its hue (the accent's, OKLCH 0.824,
// 0.164, 164.8 deg) turned once round, eased in and out (smoothstep); the
// copyright line fading in.
void splashTick() {
  static uint16_t shownLogo = 0, shownText = 0;
  uint32_t ms = millis() - shownAt;
  uint16_t logo;
  if (ms < LOGO_FADE_MS) {
    logo = blend565(LOGO_COLOR, BG, (uint8_t)(255 * ms / LOGO_FADE_MS));
  } else {
    float t = min(1.0f, (ms - LOGO_FADE_MS) / (float)HUE_MS), e = t * t * (3 - 2 * t);
    logo = t >= 1 ? LOGO_COLOR : oklch565(0.8238f, 0.1641f, 164.76f + 360 * e);
  }
  uint8_t out = ms < OUT_FROM_MS ? 255 : 255 - (uint8_t)(255 * min(ms - OUT_FROM_MS, OUT_MS) / OUT_MS);   // (the fade out)
  logo = blend565(logo, BG, out);
  if (logo != shownLogo) {
    shownLogo = logo;
    drawLogo(logo);
  }
  uint32_t into = ms < TEXT_FROM_MS ? 0 : min(ms - TEXT_FROM_MS, TEXT_FADE_MS);
  uint16_t text = blend565(blend565(SPLASH_TEXT, BG, (uint8_t)(255 * into / TEXT_FADE_MS)), BG, out);
  if (text != shownText) {
    shownText = text;
    drawLine(copyrightX, TEXT_Y_SPLASH, COPYRIGHT, text);
  }
}

void showSplash() {
  auto &t = board.tft();
  t.fillScreen(BG);                                // (logo and line fade in: splashTick())
  copyrightX = (320 - textCols(COPYRIGHT) * LOGO_PIXEL) / 2;
  shownAt = millis();
  splashTick();
}

void hideSplash() {
  while (millis() - shownAt < SPLASH_MIN_MS) {
    splashTick();
    delay(10);
  }
  splashTick();                                    // (faded out)
  board.tft().fillScreen(BG);
  uiGeneration++;                                // (the menu comes next: nothing of the looper's view drawn first)
}
