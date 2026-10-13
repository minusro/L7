// n9_input.h — N9's keys: SHIFT+UP/DOWN steps the screen stack, plain arrows
// move the screen's cursor (held: repeating, L7's timing), B+LEFT/RIGHT on the
// INST screen steps the engine. Returns true when the screen must be redrawn.
#pragma once
#include <stdint.h>

bool inputService();
