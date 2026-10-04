// ableton.cpp — see ableton.h
#include "ableton.h"
#include "storage.h"
#ifndef PROGMEM
#define PROGMEM                                    // (the simulator)
#endif

namespace looper {

#include "ableton_template.h"

// ---- gzip, its deflate data in stored (uncompressed) blocks: a block per
// buffer full, an empty last one at the end, then the CRC-32 and the length.
static uint32_t crcStep(uint32_t crc, uint8_t b) {
  static const uint32_t T[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                 0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  crc = (crc >> 4) ^ T[(crc ^ b) & 15];
  return (crc >> 4) ^ T[(crc ^ (b >> 4)) & 15];
}

static struct Gzip {
  FsFile f;
  uint8_t block[5 + 4096];                       // a stored block's header, then its data
  int n = 0;
  uint32_t crc = 0, size = 0;
  bool ok = false;

  bool open(const char *path) {
    f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
    static const uint8_t header[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};   // deflate, no name, Unix
    ok = f && f.write(header, sizeof header) == sizeof header;
    n = 0;
    crc = 0xFFFFFFFF;
    size = 0;
    return ok;
  }
  void flush(bool last) {
    block[0] = last ? 1 : 0;                     // BFINAL, stored
    block[1] = n & 0xFF;
    block[2] = n >> 8;
    block[3] = ~n & 0xFF;
    block[4] = (~n >> 8) & 0xFF;
    ok = ok && f.write(block, 5 + n) == (size_t)(5 + n);
    n = 0;
  }
  void put(char c) {
    block[5 + n++] = (uint8_t)c;
    crc = crcStep(crc, (uint8_t)c);
    size++;
    if (n == (int)sizeof block - 5) flush(false);
  }
  void puts(const char *s) { while (*s) put(*s++); }
  bool close() {
    flush(true);
    uint32_t c = ~crc;
    uint8_t tail[8] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24),
                       (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24)};
    ok = ok && f.write(tail, sizeof tail) == sizeof tail;
    ok = f.sync() && ok;
    f.close();
    return ok;
  }
} gz;

// ---- The set, from the templates: their fields filled in as they're written.
static struct {
  int p, scenes, current, number;                // scenes: those with clips; current: its place among them (-1: none)
  uint8_t pos[MAX_SCENES];                       // each one's position in the project (1..)
  float tempo;
  char dir[48], name[16];                        // "/Ableton/L7 A005", "L7 A005"
  int place;                                     // the track being written: 0-6 (Live's order)
  int ids;                                       // its fresh ids so far
  int slot;                                      // the clip slot or scene being written (0..)
  TrackUnit unit;                                // the clip's
  SampleDetails sample;
  uint32_t loopStart, loopEnd, start;            // beats
} x;

static void putNumber(double v) {
  char b[24];
  snprintf(b, sizeof b, "%.10g", v);
  gz.puts(b);
}

static void emit(const char *t);

// A track's index for Live's place p: track 1 (on the screen: the bottom
// one) first.
static int trackAt(int place) { return NUM_TRACKS - 1 - place; }

static void clipSlots(bool freeze) {
  for (x.slot = 0; x.slot < x.scenes; x.slot++) {
    TrackUnit units[NUM_TRACKS];
    bool clip = !freeze && projectScene(x.p, x.pos[x.slot], units) && units[trackAt(x.place)].sample &&
                sampleDetails(units[trackAt(x.place)].sample, x.sample);
    if (!clip) {
      emit(ALS_SLOT);
      continue;
    }
    x.unit = units[trackAt(x.place)];
    // The loop: the track's range (whole loop: length 0), or for one across
    // the loop's end (a Live loop can't wrap) the whole loop; the start: the
    // range's start position.
    uint32_t loopBeats = x.sample.loopBars * BEATS_PER_BAR;
    uint32_t len = x.unit.rangeLen ? x.unit.rangeLen : loopBeats;
    uint32_t sp = startPosition(loopBeats, x.unit.rangeStart, len);
    if (x.unit.rangeStart + len > loopBeats) {
      x.loopStart = 0;
      x.loopEnd = loopBeats;
    } else {
      x.loopStart = x.unit.rangeStart;
      x.loopEnd = x.unit.rangeStart + len;
    }
    x.start = (sp + loopBeats - x.loopStart) % loopBeats;
    if (x.start >= x.loopEnd - x.loopStart) x.start = 0;
    emit(ALS_CLIP_SLOT);
  }
}

static void field(char code) {
  char b[96];
  switch (code) {
    // the set
    case 'N': putNumber(ALS_ID_BASE + NUM_TRACKS * ALS_TRACK_IDS); break;
    case 'T': putNumber(x.tempo); break;
    case 'R':
      for (x.place = 0; x.place < NUM_TRACKS; x.place++) {
        x.ids = 0;
        emit(ALS_TRACK);
      }
      break;
    case 'C':
      for (x.slot = 0; x.slot < x.scenes; x.slot++) emit(ALS_SCENE);
      break;
    // a track
    case 'I': putNumber(26 + x.place); break;
    case 'n': putNumber(x.place + 1); break;
    case 'i': putNumber(ALS_ID_BASE + x.place * ALS_TRACK_IDS + x.ids++); break;
    case 'M': clipSlots(false); break;
    case 'S': {                                  // the current scene's clip, saved as playing
      TrackUnit units[NUM_TRACKS];
      bool clip = x.current >= 0 && projectScene(x.p, x.pos[x.current], units) && units[trackAt(x.place)].sample;
      putNumber(clip ? x.current : -2);
      break;
    }
    case 'F': clipSlots(true); break;
    // a slot, a clip, a scene
    case 'k': putNumber(x.slot); break;
    case 'm':
      if (x.place < NUM_TRACKS) gz.puts(x.sample.name);   // a clip: its sample's
      else putNumber(x.pos[x.slot]);                      // a scene: its number in the project
      break;
    case 'b': case 's': putNumber(x.loopStart); break;
    case 'e': case 'l': case 'o': putNumber(x.loopEnd); break;
    case 'r': putNumber(x.start); break;
    case 'd': gz.puts(x.unit.muted ? "true" : "false"); break;
    case 'p': snprintf(b, sizeof b, "Samples/Imported/%s.wav", x.sample.name); gz.puts(b); break;
    case 'P': snprintf(b, sizeof b, "%s/Samples/Imported/%s.wav", x.dir, x.sample.name); gz.puts(b); break;
    case 'z': putNumber((double)x.sample.bytes); break;
    case 'u': putNumber(x.sample.frames); break;
    // The second warp marker: the loop's end, so the sample plays at its own
    // tempo (its frames per beat; the file says 44100 Hz).
    case 'w': putNumber((double)x.sample.loopBars * BEATS_PER_BAR * x.sample.spb / 44100.0); break;
    case 'W': putNumber(x.sample.loopBars * BEATS_PER_BAR); break;
    case 'E': gz.puts("<Envelopes />"); break;
  }
}

static void emit(const char *t) {
  for (; *t; t++) {
    if (*t != '\001') gz.put(*t);
    else if (t[1]) field(*++t);
  }
}

// ---- The folder: the next number for the project's letter.
static int nextNumber(char letter) {
  char prefix[16];
  snprintf(prefix, sizeof prefix, "%s %c", APP_NAME, letter);
  size_t pl = strlen(prefix);
  int highest = 0;
  FsFile dir = SD.sdfs.open("/Ableton", O_RDONLY), f;
  char name[64];
  while (dir && f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof name);
    bool is = f.isDir();
    f.close();
    if (!is || strlen(name) != pl + 3 || strncmp(name, prefix, pl)) continue;
    int n = 0;
    for (int i = 0; i < 3; i++) n = name[pl + i] >= '0' && name[pl + i] <= '9' && n >= 0 ? n * 10 + name[pl + i] - '0' : -1;
    if (n > highest) highest = n;
  }
  return highest < 999 ? highest + 1 : 0;
}

int exportProject(int p, void (*progress)(int done, int total)) {
  if (p < 0 || p >= PROJECTS || transport.running || recording.mode != Recording::Idle) return 0;
  x.p = p;
  int scenes, current;
  if (!projectInfo(p, scenes, x.tempo, current)) return 0;
  // Its scenes with clips (an empty one, as the empty scene 0* is, left out).
  x.scenes = 0;
  x.current = -1;
  for (int pos = 1; pos <= scenes && x.scenes < MAX_SCENES; pos++) {
    TrackUnit units[NUM_TRACKS];
    bool clips = false;
    if (projectScene(p, pos, units))
      for (const auto &u : units) clips |= u.sample != 0;
    if (!clips) continue;
    if (pos == current) x.current = x.scenes;
    x.pos[x.scenes++] = pos;
  }
  char letter = (char)('A' + p);
  SD.sdfs.mkdir("/Ableton");
  x.number = nextNumber(letter);
  if (!x.number) return 0;
  snprintf(x.name, sizeof x.name, "%s %c%03d", APP_NAME, letter, x.number);
  snprintf(x.dir, sizeof x.dir, "/Ableton/%s", x.name);
  char path[96];
  SD.sdfs.mkdir(x.dir);
  snprintf(path, sizeof path, "%s/Ableton Project Info", x.dir);   // (what makes it a Live project:
  SD.sdfs.mkdir(path);                                               //  its samples' paths relative to it)
  snprintf(path, sizeof path, "%s/Samples", x.dir);
  SD.sdfs.mkdir(path);
  snprintf(path, sizeof path, "%s/Samples/Imported", x.dir);
  SD.sdfs.mkdir(path);

  // The samples its scenes play, copied.
  clearSampleMarks();
  for (int k = 0; k < x.scenes; k++) {
    TrackUnit units[NUM_TRACKS];
    if (projectScene(p, x.pos[k], units))
      for (const auto &u : units) markSample(u.sample);
  }
  int total = 0, done = 0;
  for (int i = 0; i < (int)sampleCount(); i++) total += sampleMarked(i);
  if (progress) progress(done, total + 1);
  for (int i = 0; i < (int)sampleCount(); i++) {
    if (!sampleMarked(i)) continue;
    SampleDetails d;
    if (sampleDetails(sampleAt(i), d)) {
      snprintf(path, sizeof path, "%s/Samples/Imported/%s.wav", x.dir, d.name);
      copySample(sampleAt(i), path);
    }
    if (progress) progress(++done, total + 1);
  }
  clearSampleMarks();

  // The set.
  snprintf(path, sizeof path, "%s/%s.als", x.dir, x.name);
  if (!gz.open(path)) return 0;
  x.place = NUM_TRACKS;                          // (not in a track)
  emit(ALS_SET);
  bool ok = gz.close();
  if (progress) progress(total + 1, total + 1);
  return ok ? x.number : 0;
}

}  // namespace looper
