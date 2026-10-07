// What L7 sends over USB MIDI (its clock out), while this sends it a Song
// Position (beat 16), Continue, 4 beats of clock and Stop at 120 BPM: prints
// every non-clock byte L7 sends, and counts its clock ticks.
#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/HostTime.h>
#include <stdio.h>
#include <unistd.h>
static volatile int ticks = 0;
static void readProc(const MIDIPacketList *pl, void *ref, void *src) {
  const MIDIPacket *p = &pl->packet[0];
  for (UInt32 i = 0; i < pl->numPackets; i++) {
    for (UInt16 k = 0; k < p->length; k++) {
      if (p->data[k] == 0xF8) ticks++;
      else printf("  from L7: %02X\n", p->data[k]);
    }
    p = MIDIPacketNext(p);
  }
}
static MIDIEndpointRef find(int dest) {
  ItemCount n = dest ? MIDIGetNumberOfDestinations() : MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef e = dest ? MIDIGetDestination(i) : MIDIGetSource(i); CFStringRef nm = NULL; char buf[128] = "";
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &nm);
    if (nm) CFStringGetCString(nm, buf, sizeof buf, kCFStringEncodingUTF8);
    if (strstr(buf, "L7")) return e;
  }
  return 0;
}
int main(int argc, char **argv) {
  int pos16 = argc > 1 ? atoi(argv[1]) : 64;          // (16ths: 64 = beat 16)
  MIDIClientRef client; MIDIPortRef in, out;
  MIDIClientCreate(CFSTR("midimon"), NULL, NULL, &client);
  MIDIInputPortCreate(client, CFSTR("in"), readProc, NULL, &in);
  MIDIOutputPortCreate(client, CFSTR("out"), &out);
  MIDIEndpointRef src = find(0), dest = find(1);
  if (!src || !dest) { printf("L7 MIDI not found\n"); return 1; }
  MIDIPortConnectSource(in, src, NULL);
  Byte buf[1024]; MIDIPacketList *pl = (MIDIPacketList *)buf; MIDIPacket *p;
  double t0 = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime()) + 200e6, beat = 60e9 / 120;
  Byte spp[3] = {0xF2, (Byte)(pos16 & 0x7F), (Byte)(pos16 >> 7)}, cont = 0xFB, tick = 0xF8, stop = 0xFC;
  if (pos16 >= 0) { p = MIDIPacketListInit(pl); p = MIDIPacketListAdd(pl, sizeof buf, p, AudioConvertNanosToHostTime((UInt64)(t0 - 3e6)), 3, spp); MIDISend(out, dest, pl); }
  Byte startOrCont = pos16 >= 0 ? cont : 0xFA;
  p = MIDIPacketListInit(pl); p = MIDIPacketListAdd(pl, sizeof buf, p, AudioConvertNanosToHostTime((UInt64)(t0 - 1e6)), 1, &startOrCont); MIDISend(out, dest, pl);
  for (int k = 0; k < 4 * 24; k++) {
    p = MIDIPacketListInit(pl); p = MIDIPacketListAdd(pl, sizeof buf, p, AudioConvertNanosToHostTime((UInt64)(t0 + k * beat / 24)), 1, &tick); MIDISend(out, dest, pl);
  }
  usleep((useconds_t)((4 * beat + 400e6) / 1000));
  p = MIDIPacketListInit(pl); p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 1, &stop); MIDISend(out, dest, pl);
  usleep(300000);
  printf("L7 sent %d clock ticks\n", ticks);
  return 0;
}
