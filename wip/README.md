# wip — work in progress, not releases

Builds here have not been tested; releases are on GitHub's releases page.

- `L7-M8-Model02-test-2026-11-04b.hex`: a Model:02 build (a Teensy MicroMod,
  `-b teensy:avr:teensyMM:usb=serialmsc`, commit dc88659), a diagnostic one. The
  earlier builds start and draw on a Model:02 but stop at "no recording space"
  with 58 GB free; this one shows why on that screen: the step that failed,
  the card's error codes, the file system, and whether a small test file
  writes and reads back. docs/HARDWARE.md says what is inferred and
  docs/testing-checklist.md what to check first.

Download: [L7-M8-Model02-test-2026-11-04b.hex](https://github.com/minusro/L7/raw/main/wip/L7-M8-Model02-test-2026-11-04b.hex)
(raw file). Saving the GitHub page with "Save link as" gets HTML, which
TyUploader reports as an IHEX parse error on line 1.
