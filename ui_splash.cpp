// ui_splash.cpp — the logo shown while the looper starts (see ui.h): "L7",
// italic, the 7's right edge one 45-degree line up to a sharp corner, its
// diagonal thick; near the bottom two cuts, each as thick as the line under
// it (80s; the L's foot starts above them). Drawn in full resolution (pixels,
// not blocks) from its shape (drawL7()): 138 x 65 px here, a little right of
// the middle, 55 x 26 in the menu's corner (drawSmallLogo()). It fades in from
// black, then its colour goes once round all hues (from the green turquoise
// accent back to it, eased in and out), fading out at the end; as it fades
// in, the L and the 7 slide in from the sides, in place at 1 s. With the
// copyright line under it in light grey (fading in), centred, in the pixel
// font (ui_font.cpp), as in the rest of the app (2 x 2-pixel blocks).
#include "ui.h"
#include "ui_font.h"

// The logo's shape: 55 x 26 units, drawn at s pixels a unit (top left at x,
// y), a row of pixels at a time: the italic slant a pixel every three rows,
// the 7's right edge a pixel a row (45 degrees), whatever the size. In units:
// the L's stem 7 wide, its foot 17 wide from row 16 down; the 7's top line 7
// tall from column 21, its diagonal 10 wide; the cuts rows 18-19 and 22-23.
static constexpr int L7_W = 55, L7_H = 26;
static int l7Px(float units, float s) { return (int)lroundf(units * s); }
// Row p of it: the L's span [l0, l1) and the 7's [s0, s1), from its left edge;
// false: a cut (nothing there).
static bool l7Row(int p, float s, int &l0, int &l1, int &s0, int &s1) {
  auto u = [s](float units) { return l7Px(units, s); };
  const int h = u(L7_H);
  if ((p >= u(18) && p < u(20)) || (p >= u(22) && p < u(24))) return false;   // the cuts
  int off = (h - 1 - p) / 3;                                    // italic
  l0 = off;                                                     // the L: its stem, then its foot
  l1 = off + (p >= h - u(10) ? u(17) : u(7));
  s1 = u(L7_W) - p;                                             // the 7: its right edge
  s0 = p < u(7) ? u(21) + off : s1 - u(10);                     // its top line, then its diagonal
  return true;
}
// The logo over what's there (the menu's corner).
static void drawL7(int x, int y, float s, uint16_t color) {
  auto &t = board.tft();
  for (int p = 0, h = l7Px(L7_H, s); p < h; p++) {
    int l0, l1, s0, s1;
    if (!l7Row(p, s, l0, l1, s0, s1)) continue;
    t.fillRect(x + l0, y + p, l1 - l0, 1, color);
    t.fillRect(x + s0, y + p, s1 - s0, 1, color);
  }
}

static constexpr float LOGO_SCALE = 2.5f;           // the splash's logo: pixels a unit
static constexpr int LOGO_W = 138, LOGO_H = 65;     // (55 x 26 units at 2.5: drawL7() rounds the width up)
// The logo and the copyright line under it centred together (the logo
// above the middle).
static constexpr int TEXT_GAP = 22;               // logo to text (the copyright sign's top), px
static constexpr int TEXT_PX = 2, TEXT_ROWS = 7, TEXT_H = TEXT_ROWS * TEXT_PX;   // the app's font blocks; the text's block rows (the copyright sign's)
static constexpr int GROUP_H = LOGO_H + TEXT_GAP + TEXT_H;   // logo, gap, text
static constexpr int GROUP_DOWN = 16;                // the group sits this far below the middle, px
static constexpr int LOGO_RIGHT = 12;                // the logo this far right of the middle, px (the text stays centred)
static constexpr int LOGO_X = (320 - LOGO_W) / 2 + LOGO_RIGHT, LOGO_Y = (240 - GROUP_H) / 2 + GROUP_DOWN;
// The splash's animation, in ms from showSplash(): the logo fades in from
// black (0-500), then its colour goes once round the hues (500-2500); the
// copyright line fades in (1500-2000); both fade out (3500-4000). Shown at
// least this long.
static constexpr uint32_t LOGO_FADE_MS = 500, HUE_MS = 2000, TEXT_FROM_MS = 1500, TEXT_FADE_MS = 500;
// Meanwhile the L slides in from the left and the 7 from the right, from far
// off the screen (still about at its edges once they're well lit, ~0.35 s),
// slowing down (cubic ease-out) until they're in place at 1 s.
static constexpr uint32_t SLIDE_MS = 1000;
static constexpr int SLIDE_PX = 600;
// And the copyright line rises 8 px (4 blocks) into place as it fades in,
// from level with the status row's text, slowing down the same way, in place
// 1 s after its fade starts.
static constexpr uint32_t RISE_MS = 1000;
static constexpr int RISE_PX = 8;
static constexpr uint32_t OUT_FROM_MS = 3500, OUT_MS = 500;
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
static int textWidthPx(const char *text) { return textCols(text) * TEXT_PX; }

// The copyright line in the app's font (2 x 2-pixel blocks, a block between
// glyphs), built in a buffer and sent at once. (x, y): the top left of its
// TEXT_ROWS block rows (the copyright sign's, a row above the rest).
static constexpr int TEXT_MAX_W = 240;
static void drawLine(int x, int y, const char *text, uint16_t color) {
  static uint16_t px[TEXT_H * TEXT_MAX_W];
  const int w = min(textWidthPx(text), TEXT_MAX_W);
  for (int i = 0; i < TEXT_H * w; i++) px[i] = BG;
  int col = 0;
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    for (int r = 0; r < 7 && g->rows[r]; r++)
      for (int c = 0; c < glyphWidth(*g); c++)
        if (g->rows[r][c] == '#')
          for (int dy = 0; dy < TEXT_PX; dy++)
            for (int dx = 0, x0 = (col + c) * TEXT_PX; dx < TEXT_PX && x0 + dx < w; dx++)
              px[((g->top + r + 1) * TEXT_PX + dy) * w + x0 + dx] = color;
    col += glyphWidth(*g) + 1;
  }
  board.tft().writeRect(x, y, w, TEXT_H, px);
}

// The text near the bottom: it starts with its letters level with the status
// row's text (the clock's), the copyright sign a block above, and rises
// RISE_PX from there (the logo stays where the centring above puts it).
static constexpr int TEXT_Y_SPLASH = STATUS_TOP + (STATUS_H - FONT_H) / 2 / PX * PX - TEXT_PX;

int splashBottom() { return LOGO_Y + LOGO_H; }      // (startup messages go under the logo, above the text)

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

// The splash's logo, the L `shift` px left of its place and the 7 as far right
// (sliding in): each row built across the screen's width and sent whole, so
// what they moved off is cleared.
// The splash's colour at a phase of its hue cycle (0-1: once round from the
// logo's turquoise, back to it): for other screens that want the same hue.
uint16_t hueCycle(float phase) { return oklch565(0.8238f, 0.1641f, 164.76f + 360 * phase); }

static void drawLogo(uint16_t color, int shift) {
  static uint16_t row[320];
  auto &t = board.tft();
  auto span = [](int a, int b, uint16_t c) { for (int x = max(a, 0); x < min(b, 320); x++) row[x] = c; };
  for (int p = 0; p < LOGO_H; p++) {
    int l0, l1, s0, s1;
    if (!l7Row(p, LOGO_SCALE, l0, l1, s0, s1)) continue;       // (a cut: always black)
    span(0, 320, BG);
    span(LOGO_X + l0 - shift, LOGO_X + l1 - shift, color);
    span(LOGO_X + s0 + shift, LOGO_X + s1 + shift, color);
    t.writeRect(0, LOGO_Y + p, 320, 1, row);
  }
}
static_assert(LOGO_W == (int)(55 * LOGO_SCALE + 0.5f) && LOGO_H == (int)(26 * LOGO_SCALE + 0.5f), "the logo's size at its scale");

// The logo small, for the menu's corner: 55 x 26 px (the status row's height).
void drawSmallLogo(int x, int y, uint16_t color) { drawL7(x, y, 1, color); }

static int copyrightX = 0;

// The splash now: the logo fading in, then its hue (the accent's, OKLCH 0.824,
// 0.164, 164.8 deg) turned once round, eased in and out (smoothstep); the
// copyright line fading in.
void splashTick() {
  static uint16_t shownLogo = 0, shownText = 0;
  static int shownTextY = -1;
  static int shownShift = -1;
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
  float left = 1 - min(1.0f, ms / (float)SLIDE_MS);              // of the slide
  int shift = (int)lroundf(SLIDE_PX * left * left * left);
  if (logo != shownLogo || shift != shownShift) {
    shownLogo = logo;
    shownShift = shift;
    drawLogo(logo, shift);
  }
  uint32_t into = ms < TEXT_FROM_MS ? 0 : min(ms - TEXT_FROM_MS, TEXT_FADE_MS);
  uint16_t text = blend565(blend565(SPLASH_TEXT, BG, (uint8_t)(255 * into / TEXT_FADE_MS)), BG, out);
  float toRise = 1 - min(1.0f, (ms < TEXT_FROM_MS ? 0 : ms - TEXT_FROM_MS) / (float)RISE_MS);   // of the rise
  int y = TEXT_Y_SPLASH - RISE_PX + (int)lroundf(RISE_PX * toRise * toRise * toRise);
  if (text != shownText || y != shownTextY) {
    if (shownTextY >= 0 && y < shownTextY)       // moved up: what it left below cleared
      board.tft().fillRect(copyrightX, y + TEXT_H, textWidthPx(COPYRIGHT), shownTextY - y, BG);
    shownText = text;
    shownTextY = y;
    drawLine(copyrightX, y, COPYRIGHT, text);
  }
}

void showSplash() {
  auto &t = board.tft();
  t.fillScreen(BG);                                // (logo and line fade in: splashTick())
  copyrightX = (320 - textWidthPx(COPYRIGHT)) / 2;
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
