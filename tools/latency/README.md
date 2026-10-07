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

`clocksync.c`: L7 following a MIDI clock over USB, its audio recorded over
USB audio, as a DAW would see it: a scheduled 120 BPM clock (or the BPM
given) to the MIDI destination "L7", Start and Stop; its metronome's clicks
(on 1, in an empty scene, nothing on the line input; the firmware built with
`USB_OUT_CLICK` true, engine.cpp, so the click is on USB) found on the input
device "L7" and placed less CoreAudio's input latency and safety offset;
prints the median offset of the clicks on beats (`VERBOSE=1`: each one).
Nothing else may send L7 clock meanwhile (a DAW's sync output). 1.7-2.1 ms
late without play-ahead; with `AHEAD_US` 2000 for USB (midi_in.cpp), within
about ±1 ms.

```sh
clang -O1 -o clocksync clocksync.c -framework CoreAudio -framework CoreFoundation -framework CoreMIDI && ./clocksync 120
```

`midimon.c`: what L7 sends over USB MIDI while this sends it a Song Position
and Continue (or Start: `./midimon -1`), 4 beats of clock and Stop: every
non-clock byte, and the count of its ticks.
