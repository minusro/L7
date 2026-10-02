// scenes.h — scenes: snapshots of the whole looper (every track's clip and
// settings, and the tempo), kept in a list you step through.
//
// Position 0 is the empty scene (0*), never stored: changing anything there
// inserts a new scene 1 holding the change (the others move up one). Past the
// last scene N is N+1*, a copy of N that becomes a scene once changed. On a
// stored scene every change updates it. The list lives in /looper/scenes.dat,
// made once at full size: saving only overwrites 512-byte blocks.
#pragma once
#include <stdint.h>

namespace looper {

constexpr int MAX_SCENES = 250;

// After storageBegin(): loads the list. The tempo goes through the app
// (rounding, limits): getTempo returns it, setTempo applies it.
void scenesBegin(float (*getTempo)(), bool (*setTempo)(float));
void sceneStateChanged();      // something in the looper changed: the current scene follows
void serviceScenes();          // call from loop(): saves, one write at a time, when the buffers allow

// Moving around (false: nothing to do, or a recording is in progress).
bool sceneStep(int dir);       // -1: older; 1: newer, up to N+1*
bool sceneGoEmpty();           // to 0*
bool sceneGoEnd();             // to N+1*
bool sceneCut();               // to the scene clipboard; removed from the list, goes to the one before
bool scenePaste();             // inserted after the current scene (or over N+1*), and selected
bool sceneDuplicate();         // a copy inserted after the current scene, and selected
bool sceneClipboardFull();

// Display: the current position (0: 0*; sceneCount() + 1: N+1*) and how many
// scenes are stored.
int sceneCurrent();
int sceneCount();

}  // namespace looper
