# L7 (m8_looper) — project notes

Custom firmware that turns a Dirtywave M8 Model:01 (Teensy 4.0, board variant B)
into L7, a 7-track stereo looper with scenes. These notes are for whoever works on
the code next: what it does, how it is put together, and why it is the way it is.
Comments in the source stay the authority on details; these pages give the map.

| Page | What's in it |
|---|---|
| [architecture.md](architecture.md) | Modules, threading (interrupt vs `loop()`), RAM budget |
| [storage.md](storage.md) | SD card layout, sample files and the pool, recording, the "no file-system changes while playing" rule |
| [playback.md](playback.md) | Streaming, loop ranges, start positions, gapless clip switches |
| [scenes.md](scenes.md) | Scene list, switching rules and timing, SHIFT hold, quick operations |
| [controls-and-ui.md](controls-and-ui.md) | Keys, screen layout, icons, colours, splash screen |
| [build-and-test.md](build-and-test.md) | Building, the PC simulator, hardware logs |
| [decisions.md](decisions.md) | Agreed behaviour, preferences, known limits, open ideas |
| [sibling-app-notes.md](sibling-app-notes.md) | What a sibling app on this hardware (N9, ...) reuses from L7: bring-up, audio, build, RAM, UI language |
| [design/](design/) | The Elektron-style restyle: `l8_restyle.html` (before/after and every state, self-contained) and the screenshots in `screens/` (rendered with tools/ui_preview) |

Hardware pinout and board variants: [HARDWARE.md](HARDWARE.md).
Work log and to-do notes kept by the author: `../M8 Looper.txt`.
