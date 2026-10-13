// n9_ui.h — N9's drawing: primitives, the 3x5 font, and the six mock screens.
// Navigation prototype only: every screen is static, drawn into the display's
// frame buffer and sent whole. The visuals are ported one to one from the
// mockup document (../docs/design/n9_mockup.html).
#pragma once
#include "M8Board.h"
#include "colors.h"

extern m8::Board board;

// The screen stack, top to bottom (SHIFT+UP towards 0, SHIFT+DOWN towards 5).
enum Screen : uint8_t { SCR_FX, SCR_MIX, SCR_MOD, SCR_INST, SCR_GRID, SCR_EDIT, SCR_COUNT };

void drawScreen(uint8_t ix);   // clears, draws screen ix, sends the frame
