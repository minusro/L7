// n9_ui.h — N9's drawing: primitives, the 3x5 font, and the six mock screens.
// Navigation prototype only: every screen is static, drawn into the display's
// frame buffer and sent whole. The visuals are ported one to one from the
// mockup document (../docs/design/n9_mockup.html).
#pragma once
#include "M8Board.h"
#include "colors.h"

extern m8::Board board;

// The three screens, top to bottom, wrapping: the clip grid (home), the
// piano roll, the instrument pages.
enum Screen : uint8_t { SCR_GRID, SCR_NOTE, SCR_INST, SCR_COUNT };

// The menu (past the grid's left/right edge): L7's layout — twelve project
// cards (rows of four) and the list under them. Only STORAGE MODE is live.
constexpr int MENU_CARDS = 12, MENU_LIST = 4, MENU_ITEMS = MENU_CARDS + MENU_LIST, MENU_COLS = 4;
constexpr int MENU_STORAGE = 1;

void drawScreen(uint8_t ix);   // clears, draws screen ix, sends the frame
void uiMessage(const char *l1, const char *l2);   // two centred lines, sent at once
void uiStorageDraw(uint8_t state, const char *msg);   // storage mode's screen (state 0-3; msg replaces the exit hint)

// The splash (L7's, with the N9 mark): show it, play the intro through
// (blocking, skippable with any key), tick it between start-up steps, then
// hide it (waits out its minimum, fades).
void splashShow();
void splashIntro();
void splashTick();
void splashHide();
