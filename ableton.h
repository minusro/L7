// ableton.h — a project exported as an Ableton Live Set (Live 11), on the card.
//
// /Ableton/L7 A005/ (APP_NAME, the project's letter, the next number for that
// letter: one more than the highest there, 001-999) holds "L7 A005.als" and
// the samples it plays, copied to Samples/Imported/ under their own names
// (A30012.wav). The set: the project's tempo; 7 audio tracks (track 1 first,
// as numbered on the screen), each with two Auto Filters (lowpass, highpass;
// fully open); a scene per scene of the project, each track's clip in it
// where the track has one: the sample in Re-Pitch warp mode, its loop and
// start position as the track's range and start position, deactivated if the
// track is muted there; the project's current scene's clips saved as playing.
// Made from a set Live saved (ableton_template.h, made
// by tools/ableton/make_template.py), written as gzip with stored blocks.
#pragma once
#include <stdint.h>

namespace looper {

// Exports project p (0-11): stopped and not recording only. progress(done,
// total) as it goes (the samples copied, then the set). Returns the folder's
// number (1-999), or 0 if it couldn't.
int exportProject(int p, void (*progress)(int done, int total) = nullptr);

}  // namespace looper
