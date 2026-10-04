// ui_splash.cpp — the logo shown while the looper starts (see ui.h): "L7",
// italic (a column every three rows), the 7's right edge one 45-degree line
// up to a sharp corner, the diagonal 10 blocks thick; near the bottom two
// cuts, each as thick as the line under it (80s; the L's foot starts above
// them),
// fading in from black, then its colour going once round all hues (from the
// green turquoise accent back to it, eased in and out), fading out at the end, in 2x2-pixel blocks, 26 tall, in the middle of the screen,
// with the copyright line under it in light grey (fading in), in the pixel font (ui_font.cpp).
#include "ui.h"
#include "ui_font.h"

PROGMEM static const char LOGO[][56] = {          // (in flash: const data would sit in RAM1)
    "........#######..............##########################",
    "........#######..............#########################.",
    ".......#######..............#########################..",
    ".......#######..............########################...",
    ".......#######..............#######################....",
    "......#######..............#######################.....",
    "......#######..............######################......",
    "......#######.........................##########.......",
    ".....#######.........................##########........",
    ".....#######........................##########.........",
    ".....#######.......................##########..........",
    "....#######.......................##########...........",
    "....#######......................##########............",
    "....#######.....................##########.............",
    "...#######.....................##########..............",
    "...#######....................##########...............",
    "...#################.........##########................",
    "..#################.........##########.................",
    ".......................................................",
    ".......................................................",
    ".#################.......##########....................",
    ".#################......##########.....................",
    ".......................................................",
    ".......................................................",
    "#################....##########........................",
    "#################...##########.........................",
};
static constexpr int LOGO_ROWS = sizeof LOGO / sizeof LOGO[0], LOGO_COLS = sizeof LOGO[0] - 1, LOGO_PIXEL = 2;
// The logo and the copyright line under it centred together (the logo
// above the middle).
static constexpr int TEXT_GAP = 24;               // logo to text, px
static constexpr int GROUP_H = LOGO_ROWS * LOGO_PIXEL + TEXT_GAP + 6 * LOGO_PIXEL;   // logo, gap, text (to the copyright sign's bottom)
static constexpr int GROUP_DOWN = 8;                 // the group sits this far below the middle, px
static constexpr int LOGO_X = (320 - LOGO_COLS * LOGO_PIXEL) / 2,
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

static constexpr int TEXT_Y_SPLASH = LOGO_Y + LOGO_ROWS * LOGO_PIXEL + TEXT_GAP;

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

static void drawLogo(uint16_t color) {
  auto &t = board.tft();
  for (int r = 0; r < LOGO_ROWS; r++)
    for (int c = 0; c < LOGO_COLS; c++)
      if (LOGO[r][c] == '#') t.fillRect(LOGO_X + c * LOGO_PIXEL, LOGO_Y + r * LOGO_PIXEL, LOGO_PIXEL, LOGO_PIXEL, color);
}

// The logo small, for the menu's corner: drawn again at 30 x 13 blocks (the
// status row's height), not shrunk, so it stays in the UI's 2 x 2 blocks; the
// same two cuts and lines, a block each.
PROGMEM static const char SMALL_LOGO[][31] = {
    "....#####......###############",
    "...#####......###############.",
    "...#####......##############..",
    "...#####......#############...",
    "..#####......#############....",
    "..#####............######.....",
    "..#####...........######......",
    ".#####...........######.......",
    ".##########.....######........",
    "..............................",
    "##########....######..........",
    "..............................",
    "##########..######............",
};
static constexpr int SMALL_ROWS = sizeof SMALL_LOGO / sizeof SMALL_LOGO[0], SMALL_COLS = sizeof SMALL_LOGO[0] - 1;

void drawSmallLogo(int x, int y, uint16_t color) {
  auto &t = board.tft();
  for (int r = 0; r < SMALL_ROWS; r++)
    for (int c = 0; c < SMALL_COLS; c++)
      if (SMALL_LOGO[r][c] == '#') t.fillRect(x + c * PX, y + r * PX, PX, PX, color);
}

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
