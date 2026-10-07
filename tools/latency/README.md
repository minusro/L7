# USB audio latency (macOS)

`latency.c` prints what CoreAudio reports for the "L7" devices (latency and
safety offset, input and output); `roundtrip.c` measures the real round trip:
clicks played to L7's USB output device, found coming back on its USB input
device (L7 monitors its input to its output), timed with CoreAudio's host
times. The clicks are heard on the M8.

```sh
clang -O1 -o latency latency.c -framework CoreAudio -framework CoreFoundation && ./latency
clang -O1 -o roundtrip roundtrip.c -framework CoreAudio -framework CoreFoundation && ./roundtrip
```

Measured 2026-10-07 (Teensyduino's stereo USB audio, 128-sample blocks):
round trip 10.9 ms (10.5-11.2), of which CoreAudio reports 5.2 ms (output
46 + 46 frames, input 68 + 68 frames), so ~5.7 ms is the M8's own (USB in
filling a block, the engine's block, USB out's queue), which no DAW knows of.
The streams' bDelay (the USB audio descriptors' delay field, set by the
patch: `AUDIO_TX_DELAY_MS` / `AUDIO_RX_DELAY_MS`) doesn't change what macOS
reports: 10 ms each made no difference. A DAW's own offset for the device
(e.g. Ableton Live's Driver Error Compensation, Logic's Recording Delay) is
where the rest goes.
