// Prints CoreAudio's latency figures for the devices named "L7".
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
static UInt32 u32(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
  UInt32 v = 0, size = sizeof v;
  if (AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &v)) return 0xFFFFFFFF;
  return v;
}
int main(void) {
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
  AudioObjectID ids[64];
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
  for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
    CFStringRef name = NULL; UInt32 ns = sizeof name;
    AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name);
    char buf[128] = "";
    if (name) CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    if (strcmp(buf, "L7")) continue;
    for (int s = 0; s < 2; s++) {
      AudioObjectPropertyScope sc = s ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
      AudioObjectPropertyAddress sa = {kAudioDevicePropertyStreams, sc, kAudioObjectPropertyElementMain};
      UInt32 ss = 0; AudioObjectGetPropertyDataSize(ids[i], &sa, 0, NULL, &ss);
      if (!ss) continue;
      AudioStreamID st[8]; AudioObjectGetPropertyData(ids[i], &sa, 0, NULL, &ss, st);
      printf("device %u %s: device latency %u, safety offset %u, buffer %u, stream latency %u (frames)\n", ids[i], s ? "output" : "input",
             u32(ids[i], kAudioDevicePropertyLatency, sc), u32(ids[i], kAudioDevicePropertySafetyOffset, sc),
             u32(ids[i], kAudioDevicePropertyBufferFrameSize, sc), u32(st[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal));
    }
  }
  return 0;
}
