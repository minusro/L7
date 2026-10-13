// n9_project.h — projects: the whole app state saved to the card, one file a
// menu card (/N9/PROJECTS/A.N9P .. L.N9P), L7's card grammar. The current
// slot's file is written over on every project switch, before storage mode,
// before an app switch and at power off. Loading while the clock runs
// commits on the next beat (L7's switch-on-the-beat); the loaded project's
// armed clips then start from their tops on the next bar line (the grid's
// launch quantum). SHIFT+A on a card cuts its file to a clipboard file
// (/N9/PROJECTS/CLIP.N9P); on an empty card it pastes it back.
#pragma once
#include <stdint.h>

extern int8_t projCurrent;          // the loaded card (always one; A at first)
extern bool projExists[12];         // which cards hold a file (projectScan)

void projectsBegin();               // boot: scan, load the last card (/N9/CURRENT.TXT)
void projectScan();                 // refresh projExists (and the clipboard's state)
bool projectSaveCurrent();          // the live state into the current card's file
void projectRequestLoad(uint8_t slot);   // saves, then loads (on the beat while playing)
void projectReloadCurrent();             // after storage mode: the card's version wins
bool projectService();              // loop(): commits a waiting beat-load (true: redraw)
void projectNew(uint8_t slot);      // saves, then a fresh state on that card
bool projectCut(uint8_t slot);      // the card's file to the clipboard (not the current card)
bool projectPaste(uint8_t slot);    // the clipboard onto an empty card
bool projectClipFull();             // something on the clipboard
