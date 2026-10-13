// n9_fx.cpp — see n9_fx.h. Fixed, tasteful settings per category; the M1's
// own per-effect parameters are not interpreted yet.
#include "n9_fx.h"

#include <Arduino.h>
#include <math.h>

FxCat fxCatOf(uint8_t raw) {
  if (raw > 32) return FX_NONE;                 // thru
  uint8_t pat = raw + 1;                        // 1-33
  if (pat <= 12) return FX_REVERB;              // halls, rooms, plates, early reflections
  if (pat <= 18) return FX_DELAY;               // delays
  if (pat <= 27) return FX_CHORUS;              // choruses, symphonic ensemble, flangers
  if (pat == 31 || pat == 32 || pat == 33) return FX_CHORUS;   // phaser, rotary, tremolo
  return FX_NONE;                               // exciter, enhancer, distortion (not yet)
}

static inline float i16f(int16_t v) { return v * (1.0f / 32768.0f); }
static inline int16_t fi16(float v) {
  if (v > 0.999f) v = 0.999f; else if (v < -0.999f) v = -0.999f;
  return (int16_t)(v * 32767.0f);
}

// ---- Chorus: two modulated taps off one line ------------------------------
static float chbuf[2048];
static int chpos = 0;
static float chph = 0;

static void chorus(const float *in, float *l, float *r, int n) {
  for (int i = 0; i < n; i++) {
    chbuf[chpos] = in[i];
    float m = sinf(chph * 6.2832f), m2 = sinf((chph + 0.25f) * 6.2832f);
    float d1 = 662.0f + 309.0f * m, d2 = 971.0f + 309.0f * m2;   // 15±7 ms, 22±7 ms
    for (int t = 0; t < 2; t++) {
      float d = t ? d2 : d1;
      int di = (int)d; float fr = d - di;
      float a = chbuf[(chpos - di) & 2047], b = chbuf[(chpos - di - 1) & 2047];
      (t ? r : l)[i] = a + (b - a) * fr;
    }
    chph += 0.6f / 44100.0f; if (chph >= 1.0f) chph -= 1.0f;
    chpos = (chpos + 1) & 2047;
  }
}

// ---- Delay: stereo cross feedback -----------------------------------------
DMAMEM static int16_t dbL[16384], dbR[16384];
static int dpos = 0;
static constexpr int DL = 13230, DR = 9922;     // 300 ms, 225 ms

static void delayFx(const float *in, float *l, float *r, int n) {
  for (int i = 0; i < n; i++) {
    float tl = i16f(dbL[(dpos - DL) & 16383]), tr = i16f(dbR[(dpos - DR) & 16383]);
    dbL[dpos] = fi16(in[i] + tr * 0.35f);
    dbR[dpos] = fi16(in[i] * 0.85f + tl * 0.35f);
    l[i] = tl; r[i] = tr;
    dpos = (dpos + 1) & 16383;
  }
}

// ---- Reverb: six damped combs + allpasses (freeverb-cut) ------------------
static constexpr int NC = 6;
static constexpr int CLEN[NC] = {1116, 1188, 1277, 1356, 1422, 1491};
DMAMEM static int16_t cb[NC][1491];
static float cf[NC];
static int cp[NC];
static constexpr int AL1 = 556, AL2 = 225, AL3 = 341;
DMAMEM static int16_t ab1[AL1], ab2[AL2], ab3[AL3];
static int ap1 = 0, ap2 = 0, ap3 = 0;

static inline float allpass(int16_t *buf, int len, int &pos, float x) {
  float b = i16f(buf[pos]);
  float y = -x + b;
  buf[pos] = fi16(x + b * 0.5f);
  if (++pos >= len) pos = 0;
  return y;
}

static void reverb(const float *in, float *l, float *r, int n) {
  for (int i = 0; i < n; i++) {
    float x = in[i] * 0.4f, sum = 0;
    for (int c = 0; c < NC; c++) {
      float y = i16f(cb[c][cp[c]]);
      cf[c] = cf[c] * 0.35f + y * 0.65f;        // damping
      cb[c][cp[c]] = fi16(x + cf[c] * 0.82f);
      if (++cp[c] >= CLEN[c]) cp[c] = 0;
      sum += y;
    }
    sum *= (1.0f / NC) * 1.8f;
    float a = allpass(ab1, AL1, ap1, sum);
    l[i] = allpass(ab2, AL2, ap2, a);
    r[i] = allpass(ab3, AL3, ap3, a);
  }
}

// ---- The chain ------------------------------------------------------------

static void stage(const float *in, float *l, float *r, int n, FxCat cat, float wet) {
  if (wet > 0.7f) wet = 0.7f;                   // the dry part always stays audible
  if (cat == FX_NONE || wet <= 0.001f) {
    for (int i = 0; i < n; i++) l[i] = r[i] = in[i];
    return;
  }
  float wl[128], wr[128];
  if (cat == FX_REVERB) reverb(in, wl, wr, n);
  else if (cat == FX_DELAY) delayFx(in, wl, wr, n);
  else chorus(in, wl, wr, n);
  float dry = 1.0f - wet;
  for (int i = 0; i < n; i++) {
    l[i] = in[i] * dry + wl[i] * wet;
    r[i] = in[i] * dry + wr[i] * wet;
  }
}

void fxProcess(const float *in, float *addL, float *addR, int n,
               FxCat cat1, float wet1, FxCat cat2, float wet2) {
  static bool cleared = false;
  if (!cleared) {                               // DMAMEM starts uninitialised
    memset(dbL, 0, sizeof dbL); memset(dbR, 0, sizeof dbR);
    memset(cb, 0, sizeof cb);
    memset(ab1, 0, sizeof ab1); memset(ab2, 0, sizeof ab2); memset(ab3, 0, sizeof ab3);
    cleared = true;
  }
  float l1[128], r1[128], mono[128], l2[128], r2[128];
  stage(in, l1, r1, n, cat1, wet1);
  for (int i = 0; i < n; i++) mono[i] = (l1[i] + r1[i]) * 0.5f;
  stage(mono, l2, r2, n, cat2, wet2);
  for (int i = 0; i < n; i++) {
    addL[i] += l1[i] * 0.4f + l2[i] * 0.6f;     // a little of stage 1's own stereo kept
    addR[i] += r1[i] * 0.4f + r2[i] * 0.6f;
  }
}
