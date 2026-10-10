// desktop.cpp — L7 as a desktop app (SDL2): runs the sketch's setup() and
// loop() as the M8 would, the screen in a window (3x), the M8's keys on the
// keyboard, the engine on SDL's audio thread (its interrupt), the computer's
// audio input as the line input, and a folder as the SD card.
//
//   ./build.sh && ./L7 [card-folder]      (default: ~/Documents/L7 Desktop)
//
// Keys: arrows; X or Left Ctrl: A (edit); Z or Left Alt: B (option);
// Left/Right Shift: SHIFT; Space: PLAY. Esc quits.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
#include "app.h"
#include "engine.h"

void setup();
void loop();
extern LooperEngine engine;

// ---- What the stand-ins expect from their host
std::string simRoot;
SDClass SD;
SerialStub Serial;
CrashReportStub CrashReport;
int simFailIo = 0, simReinits = 0, simFsChanges = 0, simFileCreates = 0, simSampleTruncates = 0;
void (*simSdLatency)(size_t, bool) = nullptr;

namespace looper {
void (*usbDriveTick)() = nullptr;
void (*usbDriveReload)() = nullptr;
void usbDriveBegin() {}
bool usbDriveMode() { return false; }
}  // namespace looper
void midiUsbBegin() {}

// ---- Time
static const auto t0 = std::chrono::steady_clock::now();
uint32_t micros() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
uint32_t millis() { return micros() / 1000; }
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void delayMicroseconds(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void yield() {}

// ---- Interrupts: one lock, held by the audio thread while the engine runs
// and by the app's code between noInterrupts() and interrupts() (nested:
// counted per thread).
static std::mutex irqLock;
static thread_local int irqDepth = 0;
void noInterrupts() { if (irqDepth++ == 0) irqLock.lock(); }
void interrupts() { if (irqDepth > 0 && --irqDepth == 0) irqLock.unlock(); }

// ---- Audio: the input (the computer's) through a ring to the engine; the
// engine run a block (128 frames) at a time as SDL asks for them.
static constexpr int RING = 1 << 15;               // (stereo frames)
static int16_t inRing[RING][2];
static std::atomic<uint32_t> inWrite{0}, inRead{0};

static void captureCallback(void *, Uint8 *stream, int len) {
  const int16_t *s = (const int16_t *)stream;
  int frames = len / 4;
  uint32_t w = inWrite.load();
  for (int i = 0; i < frames; i++, w++) {
    inRing[w % RING][0] = s[2 * i];
    inRing[w % RING][1] = s[2 * i + 1];
  }
  inWrite.store(w);
}

static void playCallback(void *, Uint8 *stream, int len) {
  int16_t *out = (int16_t *)stream;
  int frames = len / 4;
  static audio_block_t inL, inR, outL, outR;
  static int have = 0, at = 0;                     // (frames of the last block left to play)
  for (int i = 0; i < frames; i++) {
    if (at >= have) {
      uint32_t r = inRead.load(), w = inWrite.load();
      if (w - r > RING / 2) r = w - AUDIO_BLOCK_SAMPLES * 4;   // (far behind: caught up)
      for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
        bool got = r != w;
        inL.data[k] = got ? inRing[r % RING][0] : 0;
        inR.data[k] = got ? inRing[r % RING][1] : 0;
        if (got) r++;
      }
      inRead.store(r);
      memset(&outL, 0, sizeof outL);
      memset(&outR, 0, sizeof outR);
      noInterrupts();
      engine.simIn[0] = &inL;
      engine.simIn[1] = &inR;
      engine.simSetInput(0, &inL);
      engine.simSetInput(1, &inR);
      engine.simOut[0] = &outL;
      engine.simOut[1] = &outR;
      engine.update();
      interrupts();
      have = AUDIO_BLOCK_SAMPLES;
      at = 0;
    }
    out[2 * i] = outL.data[at];
    out[2 * i + 1] = outR.data[at];
    at++;
  }
}

// ---- Keys
static uint8_t keyBit(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return m8::BTN_UP;
    case SDLK_DOWN: return m8::BTN_DOWN;
    case SDLK_LEFT: return m8::BTN_LEFT;
    case SDLK_RIGHT: return m8::BTN_RIGHT;
    case SDLK_x: case SDLK_LCTRL: return m8::BTN_EDIT;
    case SDLK_z: case SDLK_LALT: return m8::BTN_OPT;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return m8::BTN_SHIFT;
    case SDLK_SPACE: return m8::BTN_PLAY;
    default: return 0;
  }
}

int main(int argc, char **argv) {
  std::string root = argc > 1 ? argv[1] : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Documents/L7 Desktop";
  mkdir(root.c_str(), 0755);
  simRoot = root;
  setvbuf(stdout, nullptr, _IOLBF, 0);             // (the serial log, a line at a time)

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  constexpr int SCALE = 3;
  SDL_Window *win = SDL_CreateWindow("L7", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     m8::Display::W * SCALE, m8::Display::H * SCALE, SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, m8::Display::W, m8::Display::H);
  SDL_RenderSetIntegerScale(ren, SDL_TRUE);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                       m8::Display::W, m8::Display::H);
  auto present = [&] {
    auto &d = board.tft();
    if (!d.dirty) return;
    d.dirty = false;
    SDL_UpdateTexture(tex, nullptr, d.px, m8::Display::W * 2);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
  };

  SDL_AudioSpec want = {}, got;
  want.freq = 44100;                              // (the stand-in's AUDIO_SAMPLE_RATE_EXACT: the engine's timing at it)
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 256;
  want.callback = captureCallback;
  SDL_AudioDeviceID cap = SDL_OpenAudioDevice(nullptr, 1, &want, &got, 0);
  if (!cap) fprintf(stderr, "no audio input (%s): the line input silent\n", SDL_GetError());
  want.callback = playCallback;
  SDL_AudioDeviceID play = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
  if (!play) fprintf(stderr, "no audio output: %s\n", SDL_GetError());

  // The sketch's setup() draws its splash with delays; the window shown as it goes.
  if (play) SDL_PauseAudioDevice(play, 0);
  if (cap) SDL_PauseAudioDevice(cap, 0);
  setup();
  present();

  bool quit = false;
  uint32_t shownAt = 0;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) quit = true;
      else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        if (e.key.keysym.sym == SDLK_ESCAPE) { quit = true; break; }
        if (e.key.repeat) continue;
        uint8_t b = keyBit(e.key.keysym.sym);
        if (e.type == SDL_KEYDOWN) board.heldKeys |= b;
        else board.heldKeys &= ~b;
      }
    }
    loop();
    if (millis() - shownAt >= 16) {
      shownAt = millis();
      present();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  if (play) SDL_CloseAudioDevice(play);
  if (cap) SDL_CloseAudioDevice(cap);
  SDL_Quit();
  return 0;
}
