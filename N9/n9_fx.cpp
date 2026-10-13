// n9_fx.cpp — see n9_fx.h. Each slot owns a 54 KB state block in RAM2; a
// type change loads the type's defaults and clears the state.
#include "n9_fx.h"

#include <Arduino.h>
#include <math.h>

const char *const FX_TYPE_NAMES[FX_TYPES] = {
    "-", "CHORUS", "FLANGER", "PHASER", "TREM", "DELAY",
    "PINGPONG", "REVERB", "DRIVE", "CRUSH", "FILTER", "COMP", "WOW"};

static const ParamInfo FX_PARAMS[FX_TYPES][4] = {
    {{"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}},
    {{"RATE", 30}, {"DEPTH", 50}, {"SPREAD", 70}, {"MIX", 40}},       // CHORUS (I/II-ish at the defaults)
    {{"RATE", 25}, {"DEPTH", 60}, {"FEEDB", 55}, {"MIX", 45}},        // FLANGER
    {{"RATE", 30}, {"DEPTH", 60}, {"FEEDB", 50}, {"MIX", 45}},        // PHASER
    {{"RATE", 45}, {"DEPTH", 60}, {"SPREAD", 50}, {"SHAPE", 0}},      // TREM
    {{"TIME", 45}, {"FEEDB", 40}, {"TONE", 60}, {"MIX", 35}},         // DELAY
    {{"TIME", 45}, {"FEEDB", 45}, {"WIDTH", 80}, {"MIX", 35}},        // PINGPONG
    {{"SIZE", 55}, {"DAMP", 45}, {"TONE", 70}, {"MIX", 35}},          // REVERB
    {{"GAIN", 40}, {"TONE", 55}, {"LEVEL", 70}, {"MIX", 99}},         // DRIVE
    {{"BITS", 60}, {"RATE", 50}, {"TONE", 60}, {"MIX", 99}},          // CRUSH
    {{"CUTOFF", 60}, {"RES", 30}, {"MODE", 0}, {"LEVEL", 80}},        // FILTER
    {{"THRESH", 45}, {"RATIO", 50}, {"REL", 40}, {"GAIN", 55}},       // COMP
    {{"DEPTH", 35}, {"RATE", 40}, {"SAT", 45}, {"MIX", 99}},          // WOW
};

const ParamInfo *fxParams(uint8_t type) { return FX_PARAMS[type < FX_TYPES ? type : 0]; }

FxSlot fxSlots[4];
FxSlot trackFx[9][2];

// Per-slot DSP state. `line` holds the type's delay memory:
//   CHORUS/FLANGER/WOW: L at 0, R at 4096 (2048 samples each is plenty)
//   DELAY: one mono line, 26460 samples (600 ms)
//   PINGPONG: L at 0, R at 13824 (300 ms each)
//   REVERB: combs at c*2700, allpasses at 16400 / 17200 / 17700
struct FxState {
  float sp[4];   // the four params, eased per block against zipper
  float ph = 0;
  float envL = 0, envR = 0, gain = 1;
  float lpL = 0, lpR = 0, s1L = 0, s2L = 0, s1R = 0, s2R = 0;
  float apL[4], apR[4], cf[6];
  float heldL = 0, heldR = 0;
  int hold = 0, pos = 0, idx[10];
  int16_t line[27648];
};
DMAMEM static FxState st[4];

static inline float i16f(int16_t v) { return v * (1.0f / 32768.0f); }
static inline int16_t fi16(float v) {
  if (v > 0.999f) v = 0.999f; else if (v < -0.999f) v = -0.999f;
  return (int16_t)(v * 32767.0f);
}
static inline float readLine(const int16_t *buf, int len, int wr, float back) {
  int bi = (int)back;
  float fr = back - bi;
  int a = wr - bi, b = a - 1;
  if (a < 0) a += len; if (b < 0) b += len;
  return i16f(buf[a]) + (i16f(buf[b]) - i16f(buf[a])) * fr;
}
static inline float softsat(float x) {
  if (x > 1.5f) x = 1.5f; else if (x < -1.5f) x = -1.5f;
  return x - x * x * x * (4.0f / 27.0f);
}

void fxSetType(uint8_t slot, uint8_t type) {
  if (slot >= 4 || type >= FX_TYPES) return;
  memset(&st[slot], 0, sizeof(FxState));
  st[slot].gain = 1;
  fxSlots[slot].type = type;
  for (int i = 0; i < 4; i++) {
    fxSlots[slot].p[i] = FX_PARAMS[type][i].def;
    st[slot].sp[i] = FX_PARAMS[type][i].def / 99.0f;
  }
  Serial.printf("fx: slot %u %s\n", slot, FX_TYPE_NAMES[type]);
}

void fxSlotsInit() {
  for (int s = 0; s < 4; s++) { memset(&st[s], 0, sizeof(FxState)); st[s].gain = 1; fxSlots[s] = {FXT_NONE, {0, 0, 0, 0}}; }
  for (int t = 0; t < 9; t++) for (int k = 0; k < 2; k++) trackFx[t][k] = {FXT_NONE, {0, 0, 0, 0}};
}

void fxApplyTrack(uint8_t t) {
  for (int k = 0; k < 2; k++) {
    fxSetType(k, trackFx[t][k].type);            // defaults in, state cleared
    for (int i = 0; i < 4; i++) {
      fxSlots[k].p[i] = trackFx[t][k].p[i];
      st[k].sp[i] = trackFx[t][k].p[i] / 99.0f;
    }
  }
}

static void processSlot(FxSlot &sl, FxState &t, float *l, float *r, int n) {
  for (int i = 0; i < 4; i++) t.sp[i] += 0.12f * (sl.p[i] / 99.0f - t.sp[i]);   // eased, no zipper
  float p0 = t.sp[0], p1 = t.sp[1], p2 = t.sp[2], p3 = t.sp[3];
  switch (sl.type) {
    case FXT_CHORUS: case FXT_FLANGER: case FXT_WOW: {
      // One modulated line, two taps; the flanger adds feedback, WOW is a
      // slow wobble with saturation and full wet.
      bool fl = sl.type == FXT_FLANGER, wow = sl.type == FXT_WOW;
      float hz = wow ? 0.3f + 2.2f * p1 : 0.08f * expf(0.042f * p0 * 99.0f);
      float base = fl ? 60.0f : wow ? 320.0f : 480.0f;
      float depth = (fl ? 160.0f : wow ? 140.0f : 330.0f) * (wow ? p0 : p1);
      float fb = fl ? 0.85f * p2 : 0;
      float mix = wow ? p3 : (fl ? p3 : p3);
      float sat = wow ? p2 : 0;
      for (int i = 0; i < n; i++) {
        float x = (l[i] + r[i]) * 0.5f;
        float m1 = sinf(t.ph * 6.2832f);
        float m2 = sinf((t.ph + (wow ? 0.13f : 0.25f * p2)) * 6.2832f);
        float d1 = base + depth * (1.0f + m1) * 0.5f + 2.0f;
        float d2 = base + depth * (1.0f + m2) * 0.5f + 2.0f;
        float w1 = readLine(t.line, 4096, t.pos, d1);
        float w2 = readLine(t.line, 4096, t.pos, d2);
        t.line[t.pos] = fi16(x + w1 * fb);
        t.pos = (t.pos + 1) & 4095;
        t.ph += hz / 44100.0f; if (t.ph >= 1.0f) t.ph -= 1.0f;
        float ol = l[i] * (1 - mix) + w1 * mix, orr = r[i] * (1 - mix) + w2 * mix;
        if (sat > 0.01f) { float g = 1.0f + 4.0f * sat; ol = softsat(ol * g) / (1 + sat); orr = softsat(orr * g) / (1 + sat); }
        l[i] = ol; r[i] = orr;
      }
      break;
    }
    case FXT_PHASER: {
      float hz = 0.06f * expf(0.042f * p0 * 99.0f);
      float fb = 0.7f * p2, mix = p3;
      for (int i = 0; i < n; i++) {
        t.ph += hz / 44100.0f; if (t.ph >= 1.0f) t.ph -= 1.0f;
        float a = 0.2f + 0.75f * p1 * (0.5f + 0.5f * sinf(t.ph * 6.2832f));
        float xl = l[i] + t.lpL * fb, xr = r[i] + t.lpR * fb;
        for (int k = 0; k < 4; k++) {
          float yl = -a * xl + t.apL[k]; t.apL[k] = xl + a * yl; xl = yl;
          float yr = -a * xr + t.apR[k]; t.apR[k] = xr + a * yr; xr = yr;
        }
        t.lpL = xl; t.lpR = xr;
        l[i] = l[i] * (1 - mix) + xl * mix;
        r[i] = r[i] * (1 - mix) + xr * mix;
      }
      break;
    }
    case FXT_TREM: {
      float hz = 0.4f + 11.0f * p0 * p0 * 2.0f;
      bool square = p3 >= 0.5f;
      for (int i = 0; i < n; i++) {
        t.ph += hz / 44100.0f; if (t.ph >= 1.0f) t.ph -= 1.0f;
        float phR = t.ph + 0.5f * p2; if (phR >= 1.0f) phR -= 1.0f;
        float mL = square ? (t.ph < 0.5f ? 1.0f : 0.0f) : 0.5f + 0.5f * sinf(t.ph * 6.2832f);
        float mR = square ? (phR < 0.5f ? 1.0f : 0.0f) : 0.5f + 0.5f * sinf(phR * 6.2832f);
        l[i] *= 1.0f - p1 * (1.0f - mL);
        r[i] *= 1.0f - p1 * (1.0f - mR);
      }
      break;
    }
    case FXT_DELAY: {
      int d = (int)((40.0f + 560.0f * p0) * 44.1f);
      float fb = 0.75f * p1, mix = p3;
      float tone = 0.08f + 0.6f * p2;
      for (int i = 0; i < n; i++) {
        float w = readLine(t.line, 26460, t.pos, (float)d);
        t.lpL += tone * (w - t.lpL);
        t.line[t.pos] = fi16((l[i] + r[i]) * 0.5f + t.lpL * fb);
        t.pos++; if (t.pos >= 26460) t.pos = 0;
        l[i] += t.lpL * mix; r[i] += t.lpL * mix;
      }
      break;
    }
    case FXT_PINGPONG: {
      int d = (int)((30.0f + 270.0f * p0) * 44.1f);
      float fb = 0.8f * p1, mix = p3, width = p2;
      int16_t *L = t.line, *R = t.line + 13824;
      for (int i = 0; i < n; i++) {
        float wl = readLine(L, 13230, t.pos, (float)d);
        float wr = readLine(R, 13230, t.pos, (float)d);
        float x = (l[i] + r[i]) * 0.5f;
        L[t.pos] = fi16(x + wr * fb);
        R[t.pos] = fi16(wl * fb);
        t.pos++; if (t.pos >= 13230) t.pos = 0;
        float cl = wl * width + wr * (1 - width), cr = wr * width + wl * (1 - width);
        l[i] += cl * mix; r[i] += cr * mix;
      }
      break;
    }
    case FXT_REVERB: {
      static const int BASE[6] = {1116, 1188, 1277, 1356, 1422, 1491};
      float scale = 0.55f + 1.15f * p0;
      float fbk = 0.72f + 0.25f * p0;
      float damp = 0.15f + 0.7f * p1, tone = 0.1f + 0.75f * p2, mix = p3;
      int len[6];
      for (int c = 0; c < 6; c++) { len[c] = (int)(BASE[c] * scale); if (len[c] > 2690) len[c] = 2690; }
      for (int i = 0; i < n; i++) {
        float x = (l[i] + r[i]) * 0.2f, sum = 0;
        for (int c = 0; c < 6; c++) {
          int16_t *cb = t.line + c * 2700;
          float y = i16f(cb[t.idx[c]]);
          t.cf[c] += (1.0f - damp) * (y - t.cf[c]);
          cb[t.idx[c]] = fi16(x + t.cf[c] * fbk);
          if (++t.idx[c] >= len[c]) t.idx[c] = 0;
          sum += y;
        }
        sum *= 0.45f;
        t.lpL += tone * (sum - t.lpL);
        int16_t *a1 = t.line + 16400, *a2 = t.line + 17200, *a3 = t.line + 17700;
        float b = i16f(a1[t.idx[6]]); float y1 = -t.lpL + b; a1[t.idx[6]] = fi16(t.lpL + b * 0.5f); if (++t.idx[6] >= 556) t.idx[6] = 0;
        b = i16f(a2[t.idx[7]]); float yl = -y1 + b; a2[t.idx[7]] = fi16(y1 + b * 0.5f); if (++t.idx[7] >= 225) t.idx[7] = 0;
        b = i16f(a3[t.idx[8]]); float yr = -y1 + b; a3[t.idx[8]] = fi16(y1 + b * 0.5f); if (++t.idx[8] >= 341) t.idx[8] = 0;
        l[i] = l[i] * (1 - mix) + yl * mix;
        r[i] = r[i] * (1 - mix) + yr * mix;
      }
      break;
    }
    case FXT_DRIVE: {
      float g = 1.0f + 14.0f * p0 * p0, lvl = 0.15f + 0.85f * p2, mix = p3;
      float tone = 0.06f + 0.7f * p1;
      for (int i = 0; i < n; i++) {
        float dl = softsat(l[i] * g) * lvl, dr = softsat(r[i] * g) * lvl;
        t.lpL += tone * (dl - t.lpL); t.lpR += tone * (dr - t.lpR);
        l[i] = l[i] * (1 - mix) + t.lpL * mix;
        r[i] = r[i] * (1 - mix) + t.lpR * mix;
      }
      break;
    }
    case FXT_CRUSH: {
      float bits = 2.0f + 12.0f * p0;
      float steps = powf(2.0f, bits);
      int holdN = 1 + (int)((1.0f - p1) * 30.0f);
      float tone = 0.1f + 0.7f * p2, mix = p3;
      for (int i = 0; i < n; i++) {
        if (--t.hold <= 0) {
          t.heldL = floorf(l[i] * steps + 0.5f) / steps;
          t.heldR = floorf(r[i] * steps + 0.5f) / steps;
          t.hold = holdN;
        }
        t.lpL += tone * (t.heldL - t.lpL); t.lpR += tone * (t.heldR - t.lpR);
        l[i] = l[i] * (1 - mix) + t.lpL * mix;
        r[i] = r[i] * (1 - mix) + t.lpR * mix;
      }
      break;
    }
    case FXT_FILTER: {
      float fc = 0.002f + 0.26f * p0 * p0;
      float g = fc * 6.2832f; if (g > 1.2f) g = 1.2f;
      float q = 1.0f - 0.85f * p1, lvl = 0.2f + 1.0f * p3;
      bool hp = p2 >= 0.5f;
      for (int i = 0; i < n; i++) {
        float hlL = l[i] - t.s1L * 2.0f * q - t.s2L;
        t.s1L += g * hlL; t.s2L += g * t.s1L;
        float hlR = r[i] - t.s1R * 2.0f * q - t.s2R;
        t.s1R += g * hlR; t.s2R += g * t.s1R;
        l[i] = (hp ? hlL : t.s2L) * lvl;
        r[i] = (hp ? hlR : t.s2R) * lvl;
      }
      break;
    }
    case FXT_COMP: {
      float thr = powf(10.0f, (-36.0f + 32.0f * p0) / 20.0f);
      float ratio = 1.5f + 8.0f * p1;
      float rel = 1.0f - expf(-1.0f / ((30.0f + 500.0f * p2) * 44.1f));
      float makeup = 0.7f + 1.5f * p3;
      for (int i = 0; i < n; i++) {
        float a = fabsf(l[i]) > fabsf(r[i]) ? fabsf(l[i]) : fabsf(r[i]);
        if (a > t.envL) t.envL += 0.3f * (a - t.envL); else t.envL += rel * (a - t.envL);
        float want = 1.0f;
        if (t.envL > thr) want = (thr + (t.envL - thr) / ratio) / t.envL;
        t.gain += 0.05f * (want - t.gain);
        l[i] *= t.gain * makeup; r[i] *= t.gain * makeup;
      }
      break;
    }
    default: break;
  }
}

void fxProcessAll(float *l, float *r, int n) {
  for (int s = 0; s < 4; s++)
    if (fxSlots[s].type != FXT_NONE) processSlot(fxSlots[s], st[s], l, r, n);
}
