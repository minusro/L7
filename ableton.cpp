// ableton.cpp — see ableton.h
#include "ableton.h"
#include "storage.h"
#include "colors.h"
#include <math.h>
#ifndef PROGMEM
#define PROGMEM                                    // (the simulator)
#endif

namespace looper {

#include "ableton_template.h"
#include "ableton_icon.h"

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

// ---- Colours: Live's clip palette (index 0-69, five rows of 14; from Live 11's
// own scripts), and the nearest one to a colour, as the eye sees it (CIE Lab).
static const uint32_t LIVE_PALETTE[70] = {
    0xFF94A6, 0xFFA529, 0xCC9927, 0xF7F47C, 0xBFFB00, 0x1AFF2F, 0x25FFA8, 0x5CFFE8, 0x8BC5FF, 0x5480E4, 0x92A7FF, 0xD86CE4, 0xE553A0, 0xFFFFFF,
    0xFF3636, 0xF66C03, 0x99724B, 0xFFF034, 0x87FF67, 0x3DC300, 0x00BFAF, 0x19E9FF, 0x10A4EE, 0x007DC0, 0x886CE4, 0xB677C6, 0xFF39D4, 0xD0D0D0,
    0xE2675A, 0xFFA374, 0xD3AD71, 0xEDFFAE, 0xD2E498, 0xBAD074, 0x9BC48D, 0xD4FDE1, 0xCDF1F8, 0xB9C1E3, 0xCDBBE4, 0xAE98E5, 0xE5DCE1, 0xA9A9A9,
    0xC6928B, 0xB78256, 0x99836A, 0xBFBA69, 0xA6BE00, 0x7DB04D, 0x88C2BA, 0x9BB3C4, 0x85A5C2, 0x8393CC, 0xA595B5, 0xBF9FBE, 0xBC7196, 0x7B7B7B,
    0xAF3333, 0xA95131, 0x724F41, 0xDBC300, 0x85961F, 0x539F31, 0x0A9C8E, 0x236384, 0x1A2F96, 0x2F52A2, 0x624BAD, 0xA34BAD, 0xCC2E6E, 0x3C3C3C};

static void labOf(uint32_t rgb, float lab[3]) {
  float c[3];
  for (int i = 0; i < 3; i++) {
    float v = ((rgb >> (16 - 8 * i)) & 0xFF) / 255.0f;
    c[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
  }
  float xyz[3] = {(0.4124f * c[0] + 0.3576f * c[1] + 0.1805f * c[2]) / 0.95047f, 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2],
                  (0.0193f * c[0] + 0.1192f * c[1] + 0.9505f * c[2]) / 1.08883f};
  for (float &t : xyz) t = t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116;
  lab[0] = 116 * xyz[1] - 16;
  lab[1] = 500 * (xyz[0] - xyz[1]);
  lab[2] = 200 * (xyz[1] - xyz[2]);
}

// The palette index nearest to an RGB565 colour.
static int liveColorFor(uint16_t c565) {
  uint32_t rgb = (uint32_t)((c565 >> 11) * 255 / 31) << 16 | (uint32_t)(((c565 >> 5) & 63) * 255 / 63) << 8 | (uint32_t)((c565 & 31) * 255 / 31);
  float want[3];
  labOf(rgb, want);
  int best = 0;
  float bestD = 1e9f;
  for (int i = 0; i < 70; i++) {
    float lab[3];
    labOf(LIVE_PALETTE[i], lab);
    float d = 0;
    for (int k = 0; k < 3; k++) d += (lab[k] - want[k]) * (lab[k] - want[k]);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

// ---- The set, from the templates: their fields filled in as they're written.
static struct {
  int p, scenes, current, number;                // scenes: those with clips; current: its place among them (-1: none)
  uint8_t pos[MAX_SCENES];                       // each one's position in the project (1..)
  int color;                                     // the project's colour, as a Live palette index
  float tempo;
  char dir[48], name[16];                        // "/L7/EXPORTS/L7 A005 Project", "L7 A005"
  int place;                                     // the track being written: 0-6 (Live's order)
  int ids;                                       // its fresh ids so far
  int slot;                                      // the clip slot or scene being written (0..)
  TrackUnit unit;                                // the clip's
  SampleDetails sample;
  uint32_t loopStart, loopEnd, start;            // beats
  int event;                                     // the next envelope event's id
  int masterIds;                                 // the master Limiter's fresh ids so far
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
  for (x.slot = 0; x.slot <= x.scenes; x.slot++) {   // (one more than the scenes with clips: the empty last scene)
    TrackUnit units[NUM_TRACKS];
    bool clip = !freeze && x.slot < x.scenes && projectScene(x.p, x.pos[x.slot], units) && units[trackAt(x.place)].sample &&
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

// ---- A clip's envelopes: its track's balance (the Utility's, as pan) and filter
// points, each a flat line (a value at the start and at 0), so the clip sets the
// track's Utility and Auto Filters as the clip does in the app (the volume is the
// clip's own gain). Values as Live keeps them: balance -1..1, a cutoff a note
// number (Hz = 440 * 2^((n - 69) / 12), 20-135; 135 and 20: fully open). A
// clip that is off (muted) has them too.
static float cutoffNote(float hz) { return constrain(69 + 12 * log2f(hz / 440.0f), 20.0f, 135.0f); }

static void envelope(int id, int target, float value) {
  char b[900];
  snprintf(b, sizeof b,
           "<ClipEnvelope Id=\"%d\">\n<EnvelopeTarget>\n<PointeeId Value=\"%d\" />\n</EnvelopeTarget>\n<Automation>\n<Events>\n"
           "<FloatEvent Id=\"%d\" Time=\"-63072000\" Value=\"%.9g\" />\n<FloatEvent Id=\"%d\" Time=\"0\" Value=\"%.9g\" />\n</Events>\n"
           "<AutomationTransformViewState>\n<IsTransformPending Value=\"false\" />\n<TimeAndValueTransforms />\n</AutomationTransformViewState>\n"
           "</Automation>\n<LoopSlot>\n<Value />\n</LoopSlot>\n<ScrollerTimePreserver>\n<LeftTime Value=\"0\" />\n<RightTime Value=\"0\" />\n"
           "</ScrollerTimePreserver>\n</ClipEnvelope>\n",
           id, ALS_ID_BASE + x.place * ALS_TRACK_IDS + target, x.event, value, x.event + 1, value);
  x.event += 2;
  gz.puts(b);
}

static void clipEnvelopes() {
  const TrackUnit &u = x.unit;
  float pan = (float)u.balance / BALANCE_MAX;
  float lp = u.filterHi >= FILTER_MAX ? 135.0f : cutoffNote(filterStepHz(max((int)u.filterHi, 0)));
  float hp = u.filterLo <= 0 ? 20.0f : cutoffNote(filterStepHz(min((int)u.filterLo, FILTER_MAX)));
  gz.puts("<Envelopes>\n");
  envelope(0, ALS_BALANCE_AT, pan);
  envelope(1, ALS_HP_CUTOFF_AT, hp);
  envelope(2, ALS_LP_CUTOFF_AT, lp);
  gz.puts("</Envelopes>\n");
}

static void field(char code) {
  char b[96];
  switch (code) {
    // the set
    case 'N': putNumber(ALS_ID_BASE + NUM_TRACKS * ALS_TRACK_IDS + ALS_MASTER_IDS); break;
    case 'L': putNumber(ALS_ID_BASE + NUM_TRACKS * ALS_TRACK_IDS + x.masterIds++); break;   // the master Limiter's
    case 'T': putNumber(x.tempo); break;
    case 'R':
      for (x.place = 0; x.place < NUM_TRACKS; x.place++) {
        x.ids = 0;
        emit(ALS_TRACK);
      }
      break;
    case 'C':
      for (x.slot = 0; x.slot <= x.scenes; x.slot++) emit(ALS_SCENE);   // (and an empty last one)
      break;
    // a track
    case 'I': putNumber(26 + x.place); break;
    case 'n': putNumber(x.place + 1); break;
    case 'c': putNumber(x.color); break;               // a track's, a clip's: the project's colour
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
      /* a scene: no name */
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
    case 'E': clipEnvelopes(); break;
    case 'g':                                    // the clip's gain: the track's volume (1: 0 dB)
      putNumber(x.unit.volume <= 0 ? 0.0 : pow(10.0, (x.unit.volume - VOLUME_UNITY) * VOLUME_STEP_DB / 20.0));
      break;
  }
}

static void emit(const char *t) {
  for (; *t; t++) {
    if (*t != '\001') gz.put(*t);
    else if (t[1]) field(*++t);
  }
}

// ---- The folder: the next number for the project's letter; named as Live names its
// projects, "L7 A005 Project".
static const char FOLDER_SUFFIX[] = " Project";
static int nextNumber(char letter) {
  char prefix[16];
  snprintf(prefix, sizeof prefix, "%s %c", APP_NAME, letter);
  size_t pl = strlen(prefix);
  int highest = 0;
  FsFile dir = SD.sdfs.open("/L7/EXPORTS", O_RDONLY), f;
  char name[64];
  while (dir && f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof name);
    bool is = f.isDir();
    f.close();
    if (!is || strlen(name) != pl + 3 + strlen(FOLDER_SUFFIX) || strncmp(name, prefix, pl) || strcmp(name + pl + 3, FOLDER_SUFFIX)) continue;
    int n = 0;
    for (int i = 0; i < 3; i++) n = name[pl + i] >= '0' && name[pl + i] <= '9' && n >= 0 ? n * 10 + name[pl + i] - '0' : -1;
    if (n > highest) highest = n;
  }
  return highest < 999 ? highest + 1 : 0;
}

// ---- The folder's icon, as a Mac shows it (Live's own, as in any Live project).
// macOS keeps a folder's custom icon as the resource fork of a file in it called
// "Icon" and a carriage return, and its Finder flags as extended attributes; a
// FAT card has no such thing, so macOS keeps them in AppleDouble files beside the
// things they belong to ("._name"). Written here by hand: the Finder flags
// (custom icon for the folder, invisible for the Icon file), the icon.
static bool appleDouble(const char *path, uint16_t finderFlags, const uint8_t *rsrc, size_t rsrcSize) {
  static uint8_t head[0xEE2];                    // the layout macOS writes: Finder info, an (empty) attribute block, the fork
  memset(head, 0, sizeof head);
  static const uint8_t magic[] = {0x00, 0x05, 0x16, 0x07, 0x00, 0x02, 0x00, 0x00};
  memcpy(head, magic, sizeof magic);
  memcpy(head + 8, "Mac OS X        ", 16);
  auto be32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) head[at + i] = (uint8_t)(v >> (24 - 8 * i)); };
  head[25] = 2;                                  // two entries: 9 (Finder info), 2 (resource fork)
  be32(26, 9); be32(30, 0x32); be32(34, 0xEB0);
  be32(38, 2); be32(42, 0xEE2); be32(46, (uint32_t)rsrcSize);
  head[0x32 + 8] = (uint8_t)(finderFlags >> 8);  // (the Finder info: 32 bytes, its flags at 8)
  head[0x32 + 9] = (uint8_t)finderFlags;
  memcpy(head + 0x54, "ATTR", 4);               // the attributes' header, none in it
  be32(0x54 + 8, 0xEE2);
  be32(0x54 + 12, 0x78);
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  bool ok = f && f.write(head, sizeof head) == sizeof head && (!rsrcSize || f.write(rsrc, rsrcSize) == rsrcSize);
  if (f) f.close();
  return ok;
}

static void folderIcon() {
  char path[96];
  snprintf(path, sizeof path, "%s/Icon\r", x.dir);
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);   // (empty: its icon is the fork, in ._Icon)
  if (f) f.close();
  snprintf(path, sizeof path, "%s/._Icon\r", x.dir);
  appleDouble(path, 0x4000, ALS_ICON, sizeof ALS_ICON);
  snprintf(path, sizeof path, "/L7/EXPORTS/._%s%s", x.name, FOLDER_SUFFIX);
  appleDouble(path, 0x0400, nullptr, 0);
}

int exportProject(int p, void (*progress)(int done, int total)) {
  if (p < 0 || p >= PROJECTS || transport.running || recording.mode != Recording::Idle) return 0;
  x.p = p;
  x.color = liveColorFor(PROJECT_COLORS[p]);
  x.event = 1;
  x.masterIds = 0;
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
  SD.sdfs.mkdir("/L7/EXPORTS");
  x.number = nextNumber(letter);
  if (!x.number) return 0;
  snprintf(x.name, sizeof x.name, "%s %c%03d", APP_NAME, letter, x.number);
  snprintf(x.dir, sizeof x.dir, "/L7/EXPORTS/%s%s", x.name, FOLDER_SUFFIX);
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
  folderIcon();
  if (progress) progress(total + 1, total + 1);
  return ok ? x.number : 0;
}

}  // namespace looper
