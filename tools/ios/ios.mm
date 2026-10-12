// ios.mm — L7 on iOS, a first draft: the sketch's setup() and loop() on a
// thread of their own (as on the M8), the screen drawn at the top, sharp
// (nearest-neighbour), the M8's 8 keys as touch buttons under it, laid out as
// on the hardware (the arrows on the left; OPTION, EDIT over SHIFT, PLAY on
// the right; several at once), the engine run from the audio unit (its
// interrupt), the microphone as the line input, a folder in the app's
// Documents as the SD card ("L7 Card").
//
//   tools/ios/build.sh          (the iPhone Simulator: builds, installs, launches)
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <QuartzCore/QuartzCore.h>
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

// ---- What the stand-ins expect from their host (as the desktop app's)
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

static const auto t0 = std::chrono::steady_clock::now();
uint32_t micros() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
uint32_t millis() { return micros() / 1000; }
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void delayMicroseconds(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void yield() {}
static std::mutex irqLock;
static thread_local int irqDepth = 0;
void noInterrupts() { if (irqDepth++ == 0) irqLock.lock(); }
void interrupts() { if (irqDepth > 0 && --irqDepth == 0) irqLock.unlock(); }

// ---- Audio: RemoteIO, 44.1 kHz stereo 16-bit; each render pulls the
// microphone's frames, then runs the engine a block (128 frames) at a time.
static AudioUnit ioUnit;
static audio_block_t inL, inR, outL, outR;
static int have = 0, at = 0;
static int16_t micBuf[4096 * 2];

static OSStatus render(void *, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32, UInt32 frames,
                       AudioBufferList *io) {
  AudioBufferList in;
  in.mNumberBuffers = 1;
  in.mBuffers[0].mNumberChannels = 2;
  in.mBuffers[0].mDataByteSize = frames * 4;
  in.mBuffers[0].mData = micBuf;
  bool mic = frames <= 4096 && AudioUnitRender(ioUnit, flags, ts, 1, frames, &in) == noErr;
  int16_t *out = (int16_t *)io->mBuffers[0].mData;
  static int16_t pendIn[AUDIO_BLOCK_SAMPLES * 2];
  static int pendN = 0;
  for (UInt32 i = 0; i < frames; i++) {
    if (pendN < AUDIO_BLOCK_SAMPLES) {             // (the mic's frames gathered for the next block)
      pendIn[2 * pendN] = mic ? micBuf[2 * i] : 0;
      pendIn[2 * pendN + 1] = mic ? micBuf[2 * i + 1] : 0;
      pendN++;
    }
    if (at >= have) {
      for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
        inL.data[k] = k < pendN ? pendIn[2 * k] : 0;
        inR.data[k] = k < pendN ? pendIn[2 * k + 1] : 0;
      }
      pendN = 0;
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
  return noErr;
}

static void startAudio() {
  AVAudioSession *session = [AVAudioSession sharedInstance];
  [session setCategory:AVAudioSessionCategoryPlayAndRecord
           withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP
                 error:nil];
  [session setPreferredSampleRate:44100 error:nil];
  [session setPreferredIOBufferDuration:256.0 / 44100 error:nil];
  [session setActive:YES error:nil];
  AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
  AudioComponentInstanceNew(AudioComponentFindNext(nullptr, &d), &ioUnit);
  UInt32 one = 1;
  AudioUnitSetProperty(ioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one);
  AudioStreamBasicDescription f = {};
  f.mSampleRate = 44100;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  f.mChannelsPerFrame = 2;
  f.mBitsPerChannel = 16;
  f.mBytesPerFrame = f.mBytesPerPacket = 4;
  f.mFramesPerPacket = 1;
  AudioUnitSetProperty(ioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);   // (what we play)
  AudioUnitSetProperty(ioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);  // (the mic, as we take it)
  AURenderCallbackStruct cb = {render, nullptr};
  AudioUnitSetProperty(ioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
  AudioUnitInitialize(ioUnit);
  AudioOutputUnitStart(ioUnit);
}

// ---- The view: the screen, then the keys.
struct Key { uint8_t bit; const char *label; CGRect rect; };

@interface L7View : UIView
@end

@implementation L7View {
  CALayer *_screen;
  Key _keys[8];
  CATextLayer *_keyLayers[8];
  uint32_t _rgba[320 * 240];
  NSMutableSet<UITouch *> *_touches;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.backgroundColor = [UIColor colorWithWhite:0.1 alpha:1];
    self.multipleTouchEnabled = YES;
    _touches = [NSMutableSet set];
    _screen = [CALayer layer];
    _screen.magnificationFilter = kCAFilterNearest;
    _screen.backgroundColor = UIColor.blackColor.CGColor;
    [self.layer addSublayer:_screen];
    const Key keys[8] = {{m8::BTN_UP, "▲\uFE0E"}, {m8::BTN_LEFT, "◀\uFE0E"}, {m8::BTN_DOWN, "▼\uFE0E"}, {m8::BTN_RIGHT, "▶\uFE0E"},
                         {m8::BTN_OPT, "OPTION"}, {m8::BTN_EDIT, "EDIT"}, {m8::BTN_SHIFT, "SHIFT"}, {m8::BTN_PLAY, "PLAY"}};
    for (int i = 0; i < 8; i++) {
      _keys[i] = keys[i];
      CATextLayer *l = [CATextLayer layer];
      l.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1].CGColor;
      l.cornerRadius = 10;
      l.string = [NSString stringWithUTF8String:keys[i].label];
      l.fontSize = 15;
      l.foregroundColor = [UIColor colorWithWhite:0.85 alpha:1].CGColor;
      l.alignmentMode = kCAAlignmentCenter;
      l.contentsScale = UIScreen.mainScreen.scale;
      _keyLayers[i] = l;
      [self.layer addSublayer:l];
    }
    CADisplayLink *link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  }
  return self;
}

// Portrait: the screen the width across (4:3), the keys under it, the M8's
// way: the arrows a cross on the left; OPTION, EDIT over SHIFT, PLAY on the right.
- (void)layoutSubviews {
  [super layoutSubviews];
  CGRect b = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
  CGFloat m = 12, w = b.size.width - 2 * m, h = w * 0.75;
  _screen.frame = CGRectMake(b.origin.x + m, b.origin.y + m, w, h);
  CGFloat top = b.origin.y + m + h + 24, k = fmin((w - 3 * m) / 6, 70), cx = b.origin.x + m + 1.5 * k, cy = top + 1.5 * k;
  _keys[0].rect = CGRectMake(cx - k / 2, cy - 1.5 * k, k, k);   // up
  _keys[1].rect = CGRectMake(cx - 1.5 * k, cy - k / 2, k, k);   // left
  _keys[2].rect = CGRectMake(cx - k / 2, cy + k / 2, k, k);     // down
  _keys[3].rect = CGRectMake(cx + k / 2, cy - k / 2, k, k);     // right
  CGFloat rx = b.origin.x + b.size.width - m - 2 * k - m / 2, ry = cy - k - m / 4;
  _keys[4].rect = CGRectMake(rx, ry, k, k);                     // OPTION
  _keys[5].rect = CGRectMake(rx + k + m / 2, ry, k, k);         // EDIT
  _keys[6].rect = CGRectMake(rx, ry + k + m / 2, k, k);         // SHIFT
  _keys[7].rect = CGRectMake(rx + k + m / 2, ry + k + m / 2, k, k);   // PLAY
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (int i = 0; i < 8; i++) {
    _keyLayers[i].frame = _keys[i].rect;
    _keyLayers[i].fontSize = i < 4 ? k * 0.4 : k * 0.2;
    // (the label a little below the middle: CATextLayer draws it from the top)
    _keyLayers[i].contentsRect = CGRectMake(0, -0.5 + (_keyLayers[i].fontSize * 0.6) / k, 1, 1);
  }
  [CATransaction commit];
}

// The keys held: each touch over a key holds it (several at once).
- (void)updateKeys {
  uint8_t held = 0;
  for (UITouch *t in _touches) {
    CGPoint p = [t locationInView:self];
    for (int i = 0; i < 8; i++)
      if (CGRectContainsPoint(CGRectInset(_keys[i].rect, -6, -6), p)) held |= _keys[i].bit;
  }
  board.heldKeys = held;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (int i = 0; i < 8; i++)
    _keyLayers[i].backgroundColor = [UIColor colorWithWhite:(held & _keys[i].bit) ? 0.55 : 0.25 alpha:1].CGColor;
  [CATransaction commit];
}
- (void)touchesBegan:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [_touches unionSet:t]; [self updateKeys]; }
- (void)touchesMoved:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self updateKeys]; }
- (void)touchesEnded:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [_touches minusSet:t]; [self updateKeys]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [_touches minusSet:t]; [self updateKeys]; }

// Every frame: the screen, if drawn on since.
- (void)tick {
  auto &d = board.tft();
  if (!d.dirty) return;
  d.dirty = false;
  for (int i = 0; i < 320 * 240; i++) {
    uint16_t c = d.px[i];
    uint32_t r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, bl = (c & 31) * 255 / 31;
    _rgba[i] = r | g << 8 | bl << 16 | 0xFF000000u;
  }
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(_rgba, 320, 240, 8, 320 * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _screen.contents = (__bridge id)img;
  [CATransaction commit];
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
}
@end

@interface L7Controller : UIViewController
@end
@implementation L7Controller
- (void)loadView { self.view = [[L7View alloc] initWithFrame:UIScreen.mainScreen.bounds]; }
- (BOOL)prefersStatusBarHidden { return YES; }
@end

@interface L7AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation L7AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
  NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  simRoot = std::string(docs.UTF8String) + "/L7 Card";
  mkdir(simRoot.c_str(), 0755);
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [L7Controller new];
  [self.window makeKeyAndVisible];
  [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted) {
    dispatch_async(dispatch_get_main_queue(), ^{ startAudio(); });
  }];
  std::thread([] {                                 // the firmware: setup(), then loop() for ever (as on the M8)
    setup();
    for (;;) {
      loop();
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  }).detach();
  return YES;
}
@end

int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([L7AppDelegate class]));
  }
}
