// k6_ui.h — K6's drawing (see k6_ui.cpp). The navigation prototype draws
// each screen whole into the frame buffer and sends it synchronously.
#pragma once
#include "M8Board.h"
#include "colors.h"

extern m8::Board board;

void drawScreen();     // the current map screen, whole
void splashShow();     // the static K6 mark
