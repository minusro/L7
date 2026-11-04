# wip — work in progress, not releases

Builds here have not been tested; releases are on GitHub's releases page.

- `L7-M8-Model02-test-2026-11-04c.hex`: a Model:02 build (a Teensy MicroMod,
  `-b teensy:avr:teensyMM:usb=serialmsc`, commit 306045d). The earlier builds
  start and draw on a Model:02 but stopped at "card full" on an exFAT card
  (any card of 64 GB or more): fixed in this one (recording space and
  project files on exFAT). If it still stops, its error screen says why
  (the failing step, the card's error codes, the file system, a test
  file's write and read-back). docs/HARDWARE.md says what is inferred and
  docs/testing-checklist.md what to check first.

Download: [L7-M8-Model02-test-2026-11-04c.hex](https://github.com/minusro/L7/raw/main/wip/L7-M8-Model02-test-2026-11-04c.hex)
(raw file). Saving the GitHub page with "Save link as" gets HTML, which
TyUploader reports as an IHEX parse error on line 1.
