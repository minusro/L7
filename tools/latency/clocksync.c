// L7 following a MIDI clock over USB, recorded over USB audio: how far its beat
// lands from the clock's, as a DAW would see it. Sends Start and a 120 BPM clock
// to the MIDI destination "L7" (each tick scheduled for an exact host time),
// records the input device "L7" (L7's output), finds its metronome's clicks
// (the metronome has to be on: SHIFT+A on the tempo cell, to 1), places each
// as a DAW does (its input time less CoreAudio's latency and safety offset),
// and prints how far each falls from its beat. Then Stop.
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>
static double rate = 44100, inShiftNs = 0;
static volatile double clickAt[512]; static volatile float clickLevel[512]; static volatile int clicks = 0;
static OSStatus inProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inT,
                       AudioBufferList *out, const AudioTimeStamp *outT, void *ctx) {
  static double last = 0;
  double t0 = AudioConvertHostTimeToNanos(inT->mHostTime) - inShiftNs;
  const float *d = in->mBuffers[0].mData; UInt32 ch = in->mBuffers[0].mNumberChannels, n = in->mBuffers[0].mDataByteSize / 4 / ch;
  for (UInt32 k = 0; k < n; k++) {
    double t = t0 + k * 1e9 / rate;
    if (fabsf(d[k * ch]) > (getenv("THRESH") ? atof(getenv("THRESH")) : 0.04f) && t - last > 2e8 && clicks < 512) { clickLevel[clicks] = fabsf(d[k * ch]); clickAt[clicks++] = t; last = t; }
  }
  return 0;
}
static UInt32 u32(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc) {
  AudioObjectPropertyAddress a = {sel, sc, kAudioObjectPropertyElementMain}; UInt32 v = 0, s = sizeof v;
  if (AudioObjectGetPropertyData(id, &a, 0, NULL, &s, &v)) return 0; return v;
}
int main(int argc, char **argv) {
  double bpm = argc > 1 ? atof(argv[1]) : 120, beatNs = 60e9 / bpm;
  // The input device "L7".
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 size = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
  AudioObjectID ids[64], inDev = 0; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
  for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
    CFStringRef name = NULL; UInt32 ns = sizeof name;
    AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name);
    char buf[128] = ""; if (name) CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    AudioObjectPropertyAddress ia = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 s1 = 0; AudioObjectGetPropertyDataSize(ids[i], &ia, 0, NULL, &s1);
    if (!strcmp(buf, "L7") && s1) inDev = ids[i];
  }
  // The MIDI destination "L7".
  MIDIEndpointRef dest = 0;
  for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) {
    MIDIEndpointRef e = MIDIGetDestination(i); CFStringRef nm = NULL; char buf[128] = "";
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &nm);
    if (nm) CFStringGetCString(nm, buf, sizeof buf, kCFStringEncodingUTF8);
    if (strstr(buf, "L7")) dest = e;
  }
  if (!inDev || !dest) { printf("L7 not found (audio %u, MIDI %u)\n", inDev, (unsigned)dest); return 1; }
  UInt32 lat = u32(inDev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput), safe = u32(inDev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
  inShiftNs = (lat + safe) * 1e9 / rate;              // (as a DAW places what it records)
  MIDIClientRef client; MIDIPortRef port;
  MIDIClientCreate(CFSTR("clocksync"), NULL, NULL, &client);
  MIDIOutputPortCreate(client, CFSTR("out"), &port);
  AudioDeviceIOProcID pi; AudioDeviceCreateIOProcID(inDev, inProc, NULL, &pi); AudioDeviceStart(inDev, pi);
  // Start, then 24 ticks a beat for 12 beats, each scheduled; the first tick (beat 0) a moment after Start.
  UInt64 now = AudioGetCurrentHostTime();
  double t0 = AudioConvertHostTimeToNanos(now) + 300e6;
  int beats = 16;
  Byte buf[4096]; MIDIPacketList *pl = (MIDIPacketList *)buf; MIDIPacket *p = MIDIPacketListInit(pl);
  Byte start = 0xFA, tick = 0xF8;
  p = MIDIPacketListAdd(pl, sizeof buf, p, AudioConvertNanosToHostTime((UInt64)(t0 - 1e6)), 1, &start);
  MIDISend(port, dest, pl);
  for (int k = 0; k < beats * 24; k++) {
    p = MIDIPacketListInit(pl);
    p = MIDIPacketListAdd(pl, sizeof buf, p, AudioConvertNanosToHostTime((UInt64)(t0 + k * beatNs / 24)), 1, &tick);
    MIDISend(port, dest, pl);
  }
  usleep((useconds_t)((beats * beatNs + 600e6) / 1000));
  Byte stop = 0xFC; p = MIDIPacketListInit(pl); p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 1, &stop); MIDISend(port, dest, pl);
  AudioDeviceStop(inDev, pi);
  // Each click against its beat (the nearest; from beat 4 on, once settled).
  double sum = 0, mn = 1e9, mx = -1e9, on[512]; int m = 0, n = 0;
  for (int c = 0; c < clicks; c++) {
    double b = (clickAt[c] - t0) / beatNs; long nb = lround(b);
    if (nb < 4 || nb >= beats) continue;
    double off = (clickAt[c] - (t0 + nb * beatNs)) / 1e6;
    if (getenv("VERBOSE")) printf("  click at beat %.3f (level %.3f): %+.2f ms\n", b, clickLevel[c], off);
    if (fabs(off) > 20) continue;                       // (something else in its output: not a click on a beat)
    on[n++] = off;
    sum += off; m++; if (off < mn) mn = off; if (off > mx) mx = off;
  }
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (on[j] < on[i]) { double x = on[i]; on[i] = on[j]; on[j] = x; }
  printf("%d clicks on beats 4-%d: L7's beat lands %.2f ms after the clock's (median; %.2f to %.2f), as a DAW would record it\n",
         m, beats - 1, n ? on[n / 2] : 0, mn, mx);
  return m ? 0 : 2;
}
