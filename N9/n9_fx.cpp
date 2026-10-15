// n9_fx.cpp — see n9_fx.h.
#include "n9_fx.h"

#include <Arduino.h>
#include <math.h>

const char *const FX_TYPE_NAMES[FX_TYPES] = {
    "-", "FLANGER", "PHASER", "TREM", "DRIVE", "CRUSH", "FILTER", "COMP",
    "WOW", "DELAY", "REVERB", "CHORUS", "PINGPONG"};

bool fxIsSend(uint8_t type) { return type == FXT_DELAY || type == FXT_REVERB || type == FXT_CHORUS; }
int fxSendIndex(uint8_t type) {
  return type == FXT_DELAY ? 0 : type == FXT_REVERB ? 1 : type == FXT_CHORUS ? 2 : -1;
}

// The eight cells per type; index 7 is always DRY/WET (SEND on a send slot).
static const ParamInfo FX_PARAMS[FX_TYPES][8] = {
    {{"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}},
    {{"RATE", 30}, {"DEPTH", 60}, {"FEEDB", 55}, {"MANUAL", 35}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 45}},
    {{"RATE", 30}, {"DEPTH", 60}, {"FEEDB", 50}, {"STAGES", 50}, {"CENTER", 40}, {"-", 0}, {"-", 0}, {"WET", 45}},
    {{"RATE", 45}, {"DEPTH", 60}, {"SPREAD", 50}, {"SHAPE", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 99}},
    {{"GAIN", 40}, {"TONE", 55}, {"LEVEL", 70}, {"BIAS", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 99}},
    {{"BITS", 60}, {"RATE", 50}, {"TONE", 60}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 99}},
    {{"CUTOFF", 60}, {"RES", 30}, {"MODE", 0}, {"DRIVE", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 99}},
    {{"THRESH", 45}, {"RATIO", 50}, {"ATK", 20}, {"REL", 40}, {"GAIN", 55}, {"-", 0}, {"-", 0}, {"WET", 99}},
    {{"DEPTH", 35}, {"RATE", 40}, {"SAT", 45}, {"FLUTTER", 20}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 99}},
    {{"TIME", 45}, {"FEEDB", 40}, {"TONE", 60}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 35}},
    {{"SIZE", 55}, {"DAMP", 45}, {"TONE", 70}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 35}},
    {{"RATE", 30}, {"DEPTH", 50}, {"SPREAD", 70}, {"-", 0}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 40}},
    {{"TIME", 45}, {"FEEDB", 45}, {"WIDTH", 80}, {"TONE", 60}, {"-", 0}, {"-", 0}, {"-", 0}, {"WET", 35}},
};

static ParamInfo sendParamBuf[8];
const ParamInfo *fxParams(uint8_t type, bool asSend) {
  if (type >= FX_TYPES) type = 0;
  if (!asSend || !fxIsSend(type)) return FX_PARAMS[type];
  for (int i = 0; i < 7; i++) sendParamBuf[i] = FX_PARAMS[type][i];
  sendParamBuf[7] = {"SEND", 0};
  return sendParamBuf;
}

FxSlot trackFx[9][2];
FxSlot sendFx[3];
FxSlot masterFx;

struct FxState {
  int16_t *line;
  int lineLen;
  float sp[8];
  float ph = 0, ph2 = 0;
  float envL = 0, gain = 1;
  float lpL = 0, lpR = 0, s1L = 0, s2L = 0, s1R = 0, s2R = 0;
  float apL[6], apR[6], cf[6];
  float heldL = 0, heldR = 0;
  int hold = 0, pos = 0, idx[10];
};
DMAMEM static int16_t masterLine[27648];
DMAMEM static int16_t trackLine[9][2][1024];   // inserts' taps never pass ~820 samples
DMAMEM static int16_t sendDelayLine[27648];
DMAMEM static int16_t sendReverbLine[27648];
DMAMEM static int16_t sendChorusLine[4096];
static FxState stTrack[9][2], stSend[3], stMaster;

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
// LFO sine from a table (the audio interrupt pays for sinf() per sample).
static float FXSINE[257];
static inline float fxsin(float ph) {
  ph -= (int)ph;
  float f = ph * 256.0f;
  int i = (int)f;
  return FXSINE[i] + (FXSINE[i + 1] - FXSINE[i]) * (f - i);
}

static inline float softsat(float x) {
  if (x > 1.5f) x = 1.5f; else if (x < -1.5f) x = -1.5f;
  return x - x * x * x * (4.0f / 27.0f);
}
float fxSoftClip(float x) { return softsat(x); }

static void resetState(FxState &t, int16_t *line, int lineLen, const FxSlot &sl) {
  memset(&t, 0, sizeof(FxState));
  t.gain = 1;
  t.line = line; t.lineLen = lineLen;
  if (line) memset(line, 0, lineLen * 2);
  for (int i = 0; i < 8; i++) t.sp[i] = sl.p[i] / 99.0f;
}

// The slot's TYPE is the activation gate the audio interrupt reads: it goes
// to NONE first, the state and parameters are rebuilt, and the real type is
// written LAST. (Setting it first left a window where the interrupt ran a
// line-based effect over a null, zero-length line: a hard fault at boot and
// on live type changes.)
static void loadDefaults(FxSlot &sl, uint8_t type) {
  sl.type = FXT_NONE;
  for (int i = 0; i < 8; i++) sl.p[i] = FX_PARAMS[type][i].def;
}

void fxSetTrackType(uint8_t t, uint8_t slot, uint8_t type) {
  if (t >= 9 || slot >= 2 || type >= FX_TYPES || type == FXT_PINGPONG) return;
  loadDefaults(trackFx[t][slot], type);
  if (fxIsSend(type)) trackFx[t][slot].p[7] = 0;        // send starts silent
  resetState(stTrack[t][slot], trackLine[t][slot], 1024, trackFx[t][slot]);
  trackFx[t][slot].type = type;                         // armed last
  Serial.printf("fx: track %u.%u %s%s\n", t + 1, slot + 1, FX_TYPE_NAMES[type], fxIsSend(type) ? " (send)" : "");
}

void fxSetMasterType(uint8_t type) {
  if (type >= FX_TYPES) return;
  loadDefaults(masterFx, type);
  resetState(stMaster, masterLine, 27648, masterFx);
  masterFx.type = type;                                 // armed last
  Serial.printf("fx: master %s\n", FX_TYPE_NAMES[type]);
}

uint8_t fxStepTrackType(uint8_t type, int dir) {
  do type = (type + FX_TYPES + dir) % FX_TYPES; while (type == FXT_PINGPONG);
  return type;
}
uint8_t fxStepMasterType(uint8_t type, int dir) { return (type + FX_TYPES + dir) % FX_TYPES; }

void fxSlotsInit() {
  for (int i = 0; i <= 256; i++) FXSINE[i] = sinf(i * (2.0f * (float)M_PI / 256.0f));
  for (int t = 0; t < 9; t++)
    for (int k = 0; k < 2; k++) { loadDefaults(trackFx[t][k], FXT_NONE); resetState(stTrack[t][k], trackLine[t][k], 1024, trackFx[t][k]); }
  static const uint8_t SEND_TYPES[3] = {FXT_DELAY, FXT_REVERB, FXT_CHORUS};
  int16_t *const SEND_LINES[3] = {sendDelayLine, sendReverbLine, sendChorusLine};
  static const int SEND_LENS[3] = {27648, 27648, 4096};
  for (int i = 0; i < 3; i++) {
    loadDefaults(sendFx[i], SEND_TYPES[i]);
    resetState(stSend[i], SEND_LINES[i], SEND_LENS[i], sendFx[i]);
    sendFx[i].type = SEND_TYPES[i];                     // armed last
  }
  loadDefaults(masterFx, FXT_NONE);
  resetState(stMaster, masterLine, 27648, masterFx);
}

// ---- The processor: every type, eight smoothed params, wet at 7 ----------
static void processSlot(const FxSlot &sl, FxState &t, float *l, float *r, int n, bool pureWet) {
  for (int i = 0; i < 8; i++) t.sp[i] += 0.12f * (sl.p[i] / 99.0f - t.sp[i]);
  float p0 = t.sp[0], p1 = t.sp[1], p2 = t.sp[2], p3 = t.sp[3], p4 = t.sp[4];
  float wet = pureWet ? 1.0f : t.sp[7];
  float dry = pureWet ? 0.0f : 1.0f - wet;
  switch (sl.type) {
    case FXT_CHORUS: case FXT_FLANGER: case FXT_WOW: {
      if (t.lineLen < 1024) break;               // state not armed yet
      bool fl = sl.type == FXT_FLANGER, wow = sl.type == FXT_WOW;
      float hz = wow ? 0.3f + 2.2f * p1 : 0.08f * expf(0.042f * p0 * 99.0f);
      float base = fl ? 40.0f + 300.0f * p3 : wow ? 320.0f : 480.0f;
      float depth = (fl ? 160.0f : wow ? 140.0f : 330.0f) * (wow ? p0 : p1);
      float fb = fl ? 0.85f * p2 : 0;
      float spread = wow ? 0.13f : fl ? 0.1f : 0.25f * p2;
      float sat = wow ? p2 : 0;
      float flut = wow ? 24.0f * p3 : 0;
      for (int i = 0; i < n; i++) {
        float x = (l[i] + r[i]) * 0.5f;
        float wob = flut > 0 ? flut * fxsin(t.ph2) : 0;
        float m1 = fxsin(t.ph), m2 = fxsin(t.ph + spread);
        float d1 = base + depth * (1.0f + m1) * 0.5f + wob + 2.0f;
        float d2 = base + depth * (1.0f + m2) * 0.5f + wob + 2.0f;
        float w1 = readLine(t.line, t.lineLen, t.pos, d1);
        float w2 = readLine(t.line, t.lineLen, t.pos, d2);
        t.line[t.pos] = fi16(softsat(x + w1 * fb));   // (hot buses: no hard 16-bit clip)
        if (++t.pos >= t.lineLen) t.pos = 0;   // (the master line isn't a power of two)
        t.ph += hz / 44100.0f; if (t.ph >= 1.0f) t.ph -= 1.0f;
        t.ph2 += 6.955f * hz / 44100.0f; if (t.ph2 >= 1.0f) t.ph2 -= 1.0f;   // the flutter, phase-continuous
        float ol = l[i] * dry + w1 * wet, orr = r[i] * dry + w2 * wet;
        if (sat > 0.01f) { float g = 1.0f + 4.0f * sat; ol = softsat(ol * g) / (1 + sat); orr = softsat(orr * g) / (1 + sat); }
        l[i] = ol; r[i] = orr;
      }
      break;
    }
    case FXT_PHASER: {
      float hz = 0.06f * expf(0.042f * p0 * 99.0f);
      float fb = 0.7f * p2;
      int stages = 2 + 2 * (int)(p3 * 2.99f);
      for (int i = 0; i < n; i++) {
        t.ph += hz / 44100.0f; if (t.ph >= 1.0f) t.ph -= 1.0f;
        float a = 0.1f + 0.5f * p4 + 0.45f * p1 * (0.5f + 0.5f * fxsin(t.ph));
        if (a > 0.93f) a = 0.93f;
        float xl = l[i] + t.lpL * fb, xr = r[i] + t.lpR * fb;
        for (int k = 0; k < stages; k++) {
          float yl = -a * xl + t.apL[k]; t.apL[k] = xl + a * yl; xl = yl;
          float yr = -a * xr + t.apR[k]; t.apR[k] = xr + a * yr; xr = yr;
        }
        t.lpL = xl; t.lpR = xr;
        l[i] = l[i] * dry + xl * wet;
        r[i] = r[i] * dry + xr * wet;
      }
      break;
    }
    case FXT_TREM: {
      float hz = 0.4f + 22.0f * p0 * p0;
      bool square = p3 >= 0.5f;
      for (int i = 0; i < n; i++) {
        t.ph += hz / 44100.0f; if (t.ph >= 1.0f) t.ph -= 1.0f;
        float phR = t.ph + 0.5f * p2; if (phR >= 1.0f) phR -= 1.0f;
        float mL = square ? (t.ph < 0.5f ? 1.0f : 0.0f) : 0.5f + 0.5f * fxsin(t.ph);
        float mR = square ? (phR < 0.5f ? 1.0f : 0.0f) : 0.5f + 0.5f * fxsin(phR);
        l[i] = l[i] * dry + l[i] * (1.0f - p1 * (1.0f - mL)) * wet;
        r[i] = r[i] * dry + r[i] * (1.0f - p1 * (1.0f - mR)) * wet;
      }
      break;
    }
    case FXT_DRIVE: {
      float g = 1.0f + 14.0f * p0 * p0, lvl = 0.15f + 0.85f * p2;
      float tone = 0.06f + 0.7f * p1, bias = 0.4f * p3;
      float bc = softsat(bias);
      for (int i = 0; i < n; i++) {
        float dl = (softsat(l[i] * g + bias) - bc) * lvl, dr = (softsat(r[i] * g + bias) - bc) * lvl;
        t.lpL += tone * (dl - t.lpL); t.lpR += tone * (dr - t.lpR);
        l[i] = l[i] * dry + t.lpL * wet;
        r[i] = r[i] * dry + t.lpR * wet;
      }
      break;
    }
    case FXT_CRUSH: {
      float steps = powf(2.0f, 2.0f + 12.0f * p0);
      int holdN = 1 + (int)((1.0f - p1) * 30.0f);
      float tone = 0.1f + 0.7f * p2;
      for (int i = 0; i < n; i++) {
        if (--t.hold <= 0) {
          t.heldL = floorf(l[i] * steps + 0.5f) / steps;
          t.heldR = floorf(r[i] * steps + 0.5f) / steps;
          t.hold = holdN;
        }
        t.lpL += tone * (t.heldL - t.lpL); t.lpR += tone * (t.heldR - t.lpR);
        l[i] = l[i] * dry + t.lpL * wet;
        r[i] = r[i] * dry + t.lpR * wet;
      }
      break;
    }
    case FXT_FILTER: {
      float fc = 0.002f + 0.26f * p0 * p0;
      float g = fc * 6.2832f; if (g > 1.2f) g = 1.2f;
      float q = 1.0f - 0.85f * p1;
      float drv = 1.0f + 3.0f * p3;
      bool hp = p2 >= 0.5f;
      for (int i = 0; i < n; i++) {
        float xl = softsat(l[i] * drv), xr = softsat(r[i] * drv);
        float hlL = xl - t.s1L * 2.0f * q - t.s2L;
        t.s1L += g * hlL; t.s2L += g * t.s1L;
        float hlR = xr - t.s1R * 2.0f * q - t.s2R;
        t.s1R += g * hlR; t.s2R += g * t.s1R;
        l[i] = l[i] * dry + (hp ? hlL : t.s2L) * wet;
        r[i] = r[i] * dry + (hp ? hlR : t.s2R) * wet;
      }
      break;
    }
    case FXT_COMP: {
      float thr = powf(10.0f, (-36.0f + 32.0f * p0) / 20.0f);
      float ratio = 1.5f + 8.0f * p1;
      float atk = 1.0f - expf(-1.0f / ((1.0f + 60.0f * p2) * 44.1f));
      float rel = 1.0f - expf(-1.0f / ((30.0f + 500.0f * p3) * 44.1f));
      float makeup = 0.7f + 1.5f * p4;
      for (int i = 0; i < n; i++) {
        float a = fabsf(l[i]) > fabsf(r[i]) ? fabsf(l[i]) : fabsf(r[i]);
        if (a > t.envL) t.envL += atk * (a - t.envL); else t.envL += rel * (a - t.envL);
        float want = 1.0f;
        if (t.envL > thr) want = (thr + (t.envL - thr) / ratio) / t.envL;
        t.gain += 0.05f * (want - t.gain);
        l[i] = l[i] * dry + l[i] * t.gain * makeup * wet;
        r[i] = r[i] * dry + r[i] * t.gain * makeup * wet;
      }
      break;
    }
    case FXT_DELAY: {
      if (t.lineLen < 26460) break;
      int d = (int)((40.0f + 560.0f * p0) * 44.1f);
      float fb = 0.75f * p1;
      float tone = 0.08f + 0.6f * p2;
      for (int i = 0; i < n; i++) {
        float w = readLine(t.line, 26460, t.pos, (float)d);
        t.lpL += tone * (w - t.lpL);
        t.line[t.pos] = fi16((l[i] + r[i]) * 0.5f + t.lpL * fb);
        t.pos++; if (t.pos >= 26460) t.pos = 0;
        l[i] = l[i] * dry + t.lpL * wet;
        r[i] = r[i] * dry + t.lpL * wet;
      }
      break;
    }
    case FXT_PINGPONG: {
      if (t.lineLen < 26460) break;
      int d = (int)((30.0f + 270.0f * p0) * 44.1f);
      float fb = 0.8f * p1, width = p2;
      int16_t *L = t.line, *R = t.line + 13824;
      for (int i = 0; i < n; i++) {
        float wl = readLine(L, 13230, t.pos, (float)d);
        float wr = readLine(R, 13230, t.pos, (float)d);
        float x = (l[i] + r[i]) * 0.5f;
        L[t.pos] = fi16(x + wr * fb);
        R[t.pos] = fi16(wl * fb);
        t.pos++; if (t.pos >= 13230) t.pos = 0;
        float cl = wl * width + wr * (1 - width), cr = wr * width + wl * (1 - width);
        l[i] = l[i] * dry + cl * wet; r[i] = r[i] * dry + cr * wet;
      }
      break;
    }
    case FXT_REVERB: {
      if (t.lineLen < 26460) break;
      static const int BASE[6] = {1116, 1188, 1277, 1356, 1422, 1491};
      float scale = 0.55f + 1.15f * p0;
      float fbk = 0.72f + 0.25f * p0;
      float damp = 0.15f + 0.7f * p1, tone = 0.1f + 0.75f * p2;
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
        sum *= (1.0f / 6.0f) * 2.4f;
        t.lpL += tone * (sum - t.lpL);
        int16_t *a1 = t.line + 16400, *a2 = t.line + 17200, *a3 = t.line + 17700;
        float b = i16f(a1[t.idx[6]]); float y1 = -t.lpL + b; a1[t.idx[6]] = fi16(t.lpL + b * 0.5f); if (++t.idx[6] >= 556) t.idx[6] = 0;
        b = i16f(a2[t.idx[7]]); float yl = -y1 + b; a2[t.idx[7]] = fi16(y1 + b * 0.5f); if (++t.idx[7] >= 225) t.idx[7] = 0;
        b = i16f(a3[t.idx[8]]); float yr = -y1 + b; a3[t.idx[8]] = fi16(y1 + b * 0.5f); if (++t.idx[8] >= 341) t.idx[8] = 0;
        l[i] = l[i] * dry + yl * wet;
        r[i] = r[i] * dry + yr * wet;
      }
      break;
    }
    default: break;
  }
}

// ---- The track chains and the send buses ----------------------------------
static float sbus[3][128];

void fxBeginBlock(int n) { memset(sbus, 0, sizeof sbus); }

static inline float sendLevel(float w) { return w <= 0.5f ? w * 2.0f : 1.0f; }
static inline float dryKeep(float w) { return w <= 0.5f ? 1.0f : 1.0f - (w - 0.5f) * 2.0f; }

static void runInsert(uint8_t t, uint8_t k, float *x, int n) {
  // mono: the stereo processor runs on a pair and the result folds down
  static float tmp[128];
  for (int i = 0; i < n; i++) tmp[i] = x[i];
  processSlot(trackFx[t][k], stTrack[t][k], tmp, x, n, false);
  for (int i = 0; i < n; i++) x[i] = (tmp[i] + x[i]) * 0.5f;
}

void fxTrackChain(uint8_t t, float *x, int n, float postVol) {
  const FxSlot &f1 = trackFx[t][0], &f2 = trackFx[t][1];
  bool s1 = fxIsSend(f1.type), s2 = fxIsSend(f2.type);
  static float orig[128];
  if (s1 && s2) for (int i = 0; i < n; i++) orig[i] = x[i];

  float w1 = f1.p[7] / 99.0f, w2 = f2.p[7] / 99.0f;
  if (f1.type != FXT_NONE) {
    if (s1) {
      float lv = sendLevel(w1) * postVol, dk = dryKeep(w1);
      float *tap = sbus[fxSendIndex(f1.type)];
      for (int i = 0; i < n; i++) { tap[i] += x[i] * lv; x[i] *= dk; }
    } else runInsert(t, 0, x, n);
  }
  if (f2.type != FXT_NONE) {
    if (s2) {
      float lv = sendLevel(w2) * postVol, dk = dryKeep(w2);
      float *tap = sbus[fxSendIndex(f2.type)];
      const float *src = s1 ? orig : x;        // a second send taps the full output
      for (int i = 0; i < n; i++) { tap[i] += src[i] * lv; x[i] *= dk; }
    } else runInsert(t, 1, x, n);
  }
}

void fxRenderSends(float *addL, float *addR, int n) {
  static float wl[128], wr[128];
  for (int s = 0; s < 3; s++) {
    for (int i = 0; i < n; i++) wl[i] = wr[i] = sbus[s][i];
    processSlot(sendFx[s], stSend[s], wl, wr, n, true);
    for (int i = 0; i < n; i++) { addL[i] += wl[i]; addR[i] += wr[i]; }
  }
}

void fxProcessMaster(float *l, float *r, int n) {
  if (masterFx.type == FXT_NONE) return;
  processSlot(masterFx, stMaster, l, r, n, false);
}
