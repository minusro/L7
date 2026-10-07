// L7's USB audio channels: their names as CoreAudio has them (from the descriptors), then
// a few seconds recorded from all of them while a 440 Hz tone plays to L7 (it monitors its
// input into the main mix): each channel's peak, and jumps in the main mix far bigger
// than the tone's own steps (a ring running dry or dropping: a click). Play some loops
// on L7 to see the tracks' channels.
//   clang -O1 -o stems stems.c -framework CoreAudio -framework CoreFoundation && ./stems [seconds]
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

static AudioObjectID dev = 0;
static double rate = 44100;
static volatile double phase = 0;
static float peak[64];
static volatile long frames = 0, jumps = 0, gaps = 0;
static volatile double lastHostNs = 0;
static float prevMain = 0;
static int havePrev = 0;

static UInt32 channels(AudioObjectID id, AudioObjectPropertyScope sc) {
  AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, sc, kAudioObjectPropertyElementMain};
  UInt32 s = 0;
  if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &s)) return 0;
  AudioBufferList *l = malloc(s);
  UInt32 n = 0;
  if (!AudioObjectGetPropertyData(id, &a, 0, NULL, &s, l))
    for (UInt32 b = 0; b < l->mNumberBuffers; b++) n += l->mBuffers[b].mNumberChannels;
  free(l);
  return n;
}

static OSStatus io(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inT,
                   AudioBufferList *out, const AudioTimeStamp *outT, void *ctx) {
  for (UInt32 b = 0; b < out->mNumberBuffers; b++) {           // the tone, both channels
    float *o = out->mBuffers[b].mData;
    UInt32 ch = out->mBuffers[b].mNumberChannels, n = out->mBuffers[b].mDataByteSize / 4 / ch;
    for (UInt32 k = 0; k < n; k++) {
      float v = 0.25f * sinf(phase);
      phase += 2 * M_PI * 440 / rate;
      if (phase > 2 * M_PI) phase -= 2 * M_PI;
      for (UInt32 c = 0; c < ch; c++) o[k * ch + c] = v;
    }
  }
  if (in->mNumberBuffers < 1) return 0;
  const float *x = in->mBuffers[0].mData;
  UInt32 ch = in->mBuffers[0].mNumberChannels, n = in->mBuffers[0].mDataByteSize / 4 / ch;
  for (UInt32 k = 0; k < n; k++) {
    for (UInt32 c = 0; c < ch && c < 64; c++) if (fabsf(x[k * ch + c]) > peak[c]) peak[c] = fabsf(x[k * ch + c]);
    float m = x[k * ch];
    if (havePrev && fabsf(m - prevMain) > 0.1f) jumps++;      // (the tone's steps: ~0.016)
    prevMain = m;
    havePrev = 1;
  }
  frames += n;
  return 0;
}

int main(int argc, char **argv) {
  double seconds = argc > 1 ? atof(argv[1]) : 8;
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 s = 0;
  AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &s);
  AudioObjectID ids[64];
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &s, ids);
  for (UInt32 i = 0; i < s / sizeof(AudioObjectID); i++) {
    CFStringRef name = NULL;
    UInt32 ns = sizeof name;
    AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name) || !name) continue;
    char buf[128];
    CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(name);
    if (!strcmp(buf, "L7") && channels(ids[i], kAudioObjectPropertyScopeInput)) dev = ids[i];
  }
  if (!dev) { printf("no L7 audio device\n"); return 1; }
  UInt32 inCh = channels(dev, kAudioObjectPropertyScopeInput), outCh = channels(dev, kAudioObjectPropertyScopeOutput);
  Float64 sr = 0;
  UInt32 srs = sizeof sr;
  AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  AudioObjectGetPropertyData(dev, &ra, 0, NULL, &srs, &sr);
  rate = sr;
  printf("L7: %u channels from it, %u to it, %.0f Hz\n", inCh, outCh, sr);
  for (UInt32 c = 1; c <= inCh; c++) {
    CFStringRef name = NULL;
    UInt32 ns = sizeof name;
    AudioObjectPropertyAddress na = {kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, c};
    char buf[64] = "(no name)";
    if (!AudioObjectGetPropertyData(dev, &na, 0, NULL, &ns, &name) && name) {
      CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
      CFRelease(name);
    }
    printf("  in %2u: %s\n", c, buf);
  }
  AudioDeviceIOProcID proc;
  AudioDeviceCreateIOProcID(dev, io, NULL, &proc);
  AudioDeviceStart(dev, proc);
  sleep(1);                                                    // (settling: not counted)
  for (int c = 0; c < 64; c++) peak[c] = 0;
  frames = 0;
  jumps = 0;
  usleep((useconds_t)(seconds * 1e6));
  AudioDeviceStop(dev, proc);
  AudioDeviceDestroyIOProcID(dev, proc);
  printf("%.1f s recorded (%ld frames, %.1f a second)\n", seconds, frames, frames / seconds);
  for (UInt32 c = 0; c < inCh && c < 64; c++) printf("  in %2u: peak %6.1f dBFS\n", c + 1, 20 * log10f(peak[c] + 1e-9f));
  printf("clicks in the main mix (jumps over 0.1): %ld\n", jumps);
  return 0;
}
