// Round trip through L7 over USB: clicks played to its output device ("L7", output), found in
// its input device ("L7", input; L7 monitors its input to its output), timed by CoreAudio's host
// times. Prints each click's delay and CoreAudio's own latency figures.
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
static AudioObjectID inDev = 0, outDev = 0;
static volatile double clickAt[64]; static volatile int clicks = 0;       // host ns of each click played
static volatile double foundAt[64]; static volatile int found = 0;        // host ns of each found
static volatile UInt64 startNs = 0;
static double rate = 44100;
static UInt32 u32(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc) {
  AudioObjectPropertyAddress a = {sel, sc, kAudioObjectPropertyElementMain}; UInt32 v = 0, s = sizeof v;
  if (AudioObjectGetPropertyData(id, &a, 0, NULL, &s, &v)) return 0; return v;
}
static OSStatus outProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inT,
                        AudioBufferList *out, const AudioTimeStamp *outT, void *ctx) {
  double t0 = AudioConvertHostTimeToNanos(outT->mHostTime);
  if (!startNs) startNs = t0;
  for (UInt32 b = 0; b < out->mNumberBuffers; b++) {
    float *d = out->mBuffers[b].mData; UInt32 ch = out->mBuffers[b].mNumberChannels, n = out->mBuffers[b].mDataByteSize / 4 / ch;
    for (UInt32 k = 0; k < n; k++) {
      double t = t0 + k * 1e9 / rate;
      int want = (int)((t - startNs) / 3e8);                  // a click every 0.3 s, from 1 s in
      float v = 0;
      if (want >= 3 && want - 3 == clicks && clicks < 40 && b == 0) { v = 0.5f; clickAt[clicks] = t; clicks++; }
      for (UInt32 c = 0; c < ch; c++) d[k * ch + c] = (b == 0 || 1) ? (k == k ? v : 0) : 0;
    }
  }
  return 0;
}
static OSStatus inProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inT,
                       AudioBufferList *out, const AudioTimeStamp *outT, void *ctx) {
  static double lastFound = 0;
  double t0 = AudioConvertHostTimeToNanos(inT->mHostTime);
  const float *d = in->mBuffers[0].mData; UInt32 ch = in->mBuffers[0].mNumberChannels, n = in->mBuffers[0].mDataByteSize / 4 / ch;
  for (UInt32 k = 0; k < n; k++) {
    double t = t0 + k * 1e9 / rate;
    if (fabsf(d[k * ch]) > 0.08f && t - lastFound > 1e8 && found < 64) { foundAt[found++] = t; lastFound = t; }
  }
  return 0;
}
int main(void) {
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 size = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
  AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
  for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
    CFStringRef name = NULL; UInt32 ns = sizeof name;
    AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name);
    char buf[128] = ""; if (name) CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    if (strcmp(buf, "L7")) continue;
    AudioObjectPropertyAddress ia = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 s1 = 0; AudioObjectGetPropertyDataSize(ids[i], &ia, 0, NULL, &s1);
    if (s1) inDev = ids[i]; else outDev = ids[i];
  }
  if (!inDev || !outDev) { printf("L7 devices not found\n"); return 1; }
  AudioDeviceIOProcID po, pi;
  AudioDeviceCreateIOProcID(outDev, outProc, NULL, &po);
  AudioDeviceCreateIOProcID(inDev, inProc, NULL, &pi);
  AudioDeviceStart(inDev, pi); AudioDeviceStart(outDev, po);
  sleep(15);
  AudioDeviceStop(outDev, po); AudioDeviceStop(inDev, pi);
  double rtt[64]; int m = 0;
  for (int c = 0, f = 0; c < clicks && f < found; c++) {
    while (f < found && foundAt[f] < clickAt[c]) f++;
    if (f < found && foundAt[f] - clickAt[c] < 2e8) { rtt[m++] = (foundAt[f] - clickAt[c]) / 1e6; f++; }
  }
  double sum = 0, mn = 1e9, mx = 0; for (int i = 0; i < m; i++) { sum += rtt[i]; if (rtt[i] < mn) mn = rtt[i]; if (rtt[i] > mx) mx = rtt[i]; }
  printf("clicks %d found %d matched %d: round trip %.2f ms avg (%.2f-%.2f)\n", clicks, found, m, m ? sum / m : 0, mn, mx);
  printf("CoreAudio says: output latency %u + safety %u frames, input latency %u + safety %u frames (= %.2f ms on top of the buffers)\n",
         u32(outDev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput), u32(outDev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput),
         u32(inDev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput), u32(inDev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput),
         (u32(outDev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) + u32(outDev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) +
          u32(inDev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) + u32(inDev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput)) * 1000.0 / rate);
  return 0;
}
