// n9_fx.h — the effects behind the M1 engine (and later the FX slots):
// chorus, stereo cross delay and a small reverb, in series as the M1 wires
// its two effects. Each M1 program's two effect patterns are mapped to the
// nearest of the three (or dry), the wet amounts from its balances.
#pragma once
#include <stdint.h>

enum FxCat : uint8_t { FX_NONE, FX_REVERB, FX_DELAY, FX_CHORUS };

// The M1 effect pattern (raw byte 0-32 = patterns 1-33, 33 = thru) as a category.
FxCat fxCatOf(uint8_t raw);

// Runs the two-stage chain over one block: in (mono) -> addL/addR get the
// dry + wet result added (the caller adds nothing else for this signal).
// wet1/wet2 are 0..1. Buffers clear themselves at first use.
void fxProcess(const float *in, float *addL, float *addR, int n,
               FxCat cat1, float wet1, FxCat cat2, float wet2);
