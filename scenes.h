// scenes.h — scenes: snapshots of the whole looper (every track's clip and
// settings), kept in a list you step through. The tempo is the project's: the
// same in all its scenes.
//
// Position 0 is the empty scene (0*), never stored: changing anything there
// inserts a new scene 1 holding the change (the others move up one). Past the
// last scene N is N+1*, a copy of N that becomes a scene once changed. On a
// stored scene every change updates it.
//
// Projects: 12 slots, each a scene list with its current scene, the selected
// row and cell, and its tempo (the master volume stays global). A new project
// takes the tempo last set by hand (in any project; 120 BPM before any). The loaded one is
// saved as it changes. A slot's file holds no audio, only the scenes (which
// name their samples): /L7/projects/1.dat ... 12.dat, made once at full
// size: saving only overwrites 512-byte blocks.
#pragma once
#include <stdint.h>

namespace looper {

struct TrackUnit;                                  // (storage.h)

constexpr int MAX_SCENES = 249, PROJECTS = 12;   // (249: the list block keeps the tempo in the 250th's place)

// After storageBegin(): loads project `project` (0-11; -1: the dummy one). The tempo goes
// through the app (rounding, limits): getTempo returns it, setTempo applies a
// project's as it loads (not a change by hand), newTempo gives new projects'
// (the last set by hand; none: 120 BPM).
void scenesBegin(float (*getTempo)(), bool (*setTempo)(float), int project = 0, float (*newTempo)() = nullptr);
void projectTempoChanged();    // the tempo was set by hand: the loaded project keeps it (all its scenes)
void sceneStateChanged();      // something in the looper changed: the current scene follows
void serviceScenes();          // call from loop(): saves, one write at a time, when the buffers allow
void flushScenes();            // everything waiting saved now (power off)

// Moving around (false: nothing to do, or a recording is in progress).
bool sceneStep(int dir);       // -1: older; 1: newer, up to N+1* (none while there are no scenes)
bool sceneGoEmpty();           // to 0*
bool sceneGoEnd();             // to N+1*
bool sceneCut();               // to the scene clipboard; removed from the list, goes to the one before
bool scenePaste();             // inserted after the current scene (or over N+1*), and selected
bool sceneDuplicate();         // a copy inserted after the current scene, and selected
bool sceneClipboardFull();
// A resampling on track t has been saved: a new scene is inserted after the current one, holding just it, on the same track; it
// leaves the current scene. The new scene is selected and goes on on the next beat
// (works while another recording runs: that track is left alone). false: the
// list is full.
bool sceneFromResample(int t);
// While on (SHIFT held, playing), stepping only moves the selection shown;
// turning it off goes there (on the next bar line, see beginSceneChange()).
// Quick, at once and on the next beat: steps to and from N+1*, a step to the
// empty scene 0* (SHIFT held: once it's let go; from it: as usual), the jumps to the ends, cut, paste
// and duplicate.
bool sceneHold(bool on);       // true: it went somewhere
bool sceneHeld();              // a held selection not gone to yet

// Display: the current position (0: 0*; sceneCount() + 1: N+1*; a held
// selection while there is one) and how many scenes are stored.
int sceneCurrent();
int sceneCount();

// ---- Projects (slots 0-11)
// Reading project p (the Live Set export): its scene count and tempo (false:
// an empty slot), and scene pos's (1..count) units, track by track.
bool projectInfo(int p, int &scenes, float &tempo, int &current);   // current: its scene (0: the empty one)
bool projectScene(int p, int pos, TrackUnit *units);
int projectCurrent();          // the loaded one (-1: the dummy project, after the loaded one was cut)
bool projectFilled(int p);     // the slot holds a project (the loaded one always does)
// Loads project p: its current scene goes on the looper (playing: on a bar
// line, as a scene change, every track starting over from 1.1.1 there and
// the clock with them). An empty slot: a new project there, on its empty
// scene (the tracks cleared, the tempo last set by hand). Loading keeps the
// tempo for new projects as it was. false: recording.
bool projectLoad(int p);
// Cut and paste (projectsMayCopy(): not while recording or while another
// copy is under way; playing, the copying goes on in the background): cut copies the
// project to the project clipboard and empties its slot (the loaded one: an
// empty dummy project, in no slot, is loaded instead; for now); paste copies the clipboard into an empty
// slot (it stays on the clipboard).
bool projectsMayCopy();
// The loaded project holds a clip somewhere (on the tracks or in a scene);
// false for the dummy project.
bool projectHasClips();
bool projectCut(int p);
bool projectPaste(int p);
bool projectClipboardFull();
bool projectCopying(int p);    // slot p is being copied from or to (not loadable meanwhile)
// Empties slot p, the project gone (not to the clipboard); the loaded one:
// the dummy project instead. false: recording.
bool projectDelete(int p);
// Deletes every recording no project uses (any track of any scene, muted or
// not; the clipboards and the dummy project included), nor the tracks now.
// Stopped only; at boot and when the menu opens. Returns how many.
int deleteUnusedSamples();
// Each scene keeps the looper's selection (row and cell): when leaving it
// it's saved with the scene, and arriving there it comes back. get/set: the
// app's selection.
void scenesSelection(void (*get)(int &row, int &cell), void (*set)(int row, int cell));
// The loaded project's selected row and cell (its current scene's, else the
// project's; -1: none saved).
void projectSelection(int &row, int &cell);
void projectSetSelection(int row, int cell);

}  // namespace looper
