// k6_gm_bank.h — the GM engine's 128 melodic presets (a Sound Canvas-style
// General MIDI map, synthesized: no PCM, each program a two-oscillator
// subtractive patch played by the pool voices, k6_synth.cpp O_GM). Authored
// by the sixteen GM families; the INST cells sit on top of a patch as
// offsets (50 = as authored). Drum kits (channel 10) are not here yet.
#pragma once
#include <stdint.h>

// Oscillator waves.
enum : uint8_t { GW_SAW, GW_SQR, GW_PUL, GW_TRI, GW_SIN, GW_NSE };

struct GmPatch {
  uint8_t w1, w2;   // the two oscillators' waves
  int8_t semi2;     // osc 2's interval, semitones
  uint8_t det;      // osc 2's extra detune, cents
  uint8_t mix;      // osc 2's level (0-99; osc 1 is full)
  uint8_t cut, res, env;  // filter: base cutoff, resonance, env amount
  uint8_t fd;       // the filter env's decay (log scale)
  uint8_t a, d, s, r;     // amp ADSR (times log-scaled; s a level)
  uint8_t vib;      // vibrato depth (fades in)
  uint8_t lvl;      // level trim
};

// {w1,w2, semi2,det,mix, cut,res,env,fd, a,d,s,r, vib,lvl}
static const GmPatch GM_BANK[128] = {
  // 1-8 piano
  {GW_SAW,GW_SIN, 12, 4,30, 52, 8,42,62,  0,86, 0,46,  0,85},   // ACOU GRAND
  {GW_SAW,GW_SIN, 12, 6,38, 62,10,48,60,  0,84, 0,44,  0,85},   // BRIGHT ACOU
  {GW_SAW,GW_SQR, 12, 8,30, 58,12,45,58,  0,84, 0,44,  0,82},   // ELEC GRAND
  {GW_SAW,GW_SAW, 12,22,45, 56,10,45,58,  0,82, 0,42,  0,80},   // HONKY-TONK
  {GW_SIN,GW_SIN, 24, 3,32, 65, 5,30,55,  0,82, 0,48,  0,88},   // E PIANO 1
  {GW_SIN,GW_TRI, 12, 5,40, 58, 5,28,58,  0,84, 0,50,  0,88},   // E PIANO 2
  {GW_PUL,GW_PUL, 12, 4,35, 70,18,40,45,  0,74, 0, 8,  0,78},   // HARPSICHORD
  {GW_PUL,GW_SAW,  0, 6,30, 68,45,58,38,  0,76, 0,10,  0,80},   // CLAVINET
  // 9-16 chromatic percussion
  {GW_SIN,GW_SIN, 24, 2,40, 75, 0,20,45,  0,74, 0,30,  0,85},   // CELESTA
  {GW_SIN,GW_SIN, 36, 2,35, 80, 0,15,40,  0,68, 0,25,  0,85},   // GLOCKENSPIEL
  {GW_SIN,GW_SIN, 31, 3,38, 72, 0,18,48,  0,76, 0,35,  0,85},   // MUSIC BOX
  {GW_SIN,GW_SIN, 24, 2,25, 62, 0,15,55,  3,84, 0,40,  8,85},   // VIBRAPHONE
  {GW_SIN,GW_TRI, 12, 3,30, 60, 0,30,35,  0,62, 0,12,  0,88},   // MARIMBA
  {GW_SIN,GW_TRI, 24, 3,30, 75, 0,30,28,  0,52, 0, 8,  0,85},   // XYLOPHONE
  {GW_SIN,GW_SIN, 29, 4,45, 55, 0,15,60,  0,90, 0,60,  0,82},   // TUBULAR BELL
  {GW_SAW,GW_TRI, 12, 5,35, 58, 8,30,50,  0,78, 0,25,  0,80},   // DULCIMER
  // 17-24 organ
  {GW_SQR,GW_SIN, 12, 2,55, 55, 0, 0, 0,  2,50,99,14,  5,75},   // DRAWBAR ORG
  {GW_SQR,GW_SIN, 19, 2,45, 62, 0,35,30,  1,50,99,12,  5,75},   // PERC ORGAN
  {GW_SQR,GW_SAW,  0, 8,45, 65,20, 0, 0,  2,50,99,12,  6,78},   // ROCK ORGAN
  {GW_SIN,GW_SIN, 12, 3,60, 48, 0, 0, 0, 16,50,99,52,  0,80},   // CHURCH ORG
  {GW_PUL,GW_SAW,  0, 5,40, 52,10, 0, 0, 10,50,99,25,  8,75},   // REED ORGAN
  {GW_PUL,GW_PUL,  0,10,50, 55, 8, 0, 0, 12,50,99,20, 12,72},   // ACCORDION
  {GW_PUL,GW_SQR,  0, 7,40, 58,15, 0, 0, 18,50,99,15, 22,75},   // HARMONICA
  {GW_PUL,GW_PUL,-12, 8,45, 55,10, 0, 0, 12,50,99,18, 10,72},   // TANGO ACCORD
  // 25-32 guitar
  {GW_TRI,GW_SAW, 12, 4,28, 55, 5,32,52,  0,80, 0,20,  0,85},   // NYLON GTR
  {GW_SAW,GW_TRI, 12, 5,30, 62, 8,38,50,  0,80, 0,18,  0,82},   // STEEL GTR
  {GW_TRI,GW_SIN,  0, 4,45, 45, 5,25,55,  0,82, 0,22,  0,85},   // JAZZ GTR
  {GW_SAW,GW_PUL,  0, 6,30, 58,12,35,48,  0,78, 0,16,  0,80},   // CLEAN GTR
  {GW_SAW,GW_SAW,  0, 4,30, 40,15,40,30,  0,58, 0,10,  0,80},   // MUTED GTR
  {GW_SQR,GW_SAW,  0, 8,45, 55,30,30,55,  2,80,45,25,  0,78},   // OVERDRIVE
  {GW_SQR,GW_SQR,-12, 9,55, 60,35,28,55,  2,82,52,28,  0,78},   // DISTORTION
  {GW_SIN,GW_SIN, 12, 2,20, 68, 0,15,50,  0,76, 0,25,  0,82},   // GTR HARMON
  // 33-40 bass
  {GW_TRI,GW_SIN,-12, 3,40, 38, 5,25,55,  0,80, 0,14,  0,90},   // ACOUSTIC BS
  {GW_TRI,GW_SAW,  0, 4,30, 42,10,32,52,  0,80,10,15,  0,90},   // FINGER BASS
  {GW_SAW,GW_PUL,  0, 5,30, 50,12,40,42,  0,74, 8,10,  0,88},   // PICK BASS
  {GW_TRI,GW_SIN,  0, 6,45, 40, 8,30,65,  5,84,30,25,  8,88},   // FRETLESS BS
  {GW_SAW,GW_SIN,-12, 4,40, 55,25,50,32,  0,68, 5, 8,  0,90},   // SLAP BASS 1
  {GW_SAW,GW_SQR,  0, 5,35, 60,28,52,35,  0,70, 5,10,  0,90},   // SLAP BASS 2
  {GW_SAW,GW_SQR,-12, 6,45, 45,35,55,45,  0,76,25,12,  0,92},   // SYNTH BASS 1
  {GW_SQR,GW_SAW,-12, 8,40, 40,30,50,55,  2,80,35,15,  0,92},   // SYNTH BASS 2
  // 41-48 strings
  {GW_SAW,GW_SAW,  0, 8,60, 55,10,15,70, 48,65,88,42, 25,78},   // VIOLIN
  {GW_SAW,GW_SAW,  0, 8,60, 48,10,15,70, 52,65,88,45, 22,78},   // VIOLA
  {GW_SAW,GW_SAW,  0, 7,60, 42,10,15,72, 55,68,88,48, 20,80},   // CELLO
  {GW_SAW,GW_SAW,  0, 6,60, 35,10,15,72, 58,68,88,50, 15,82},   // CONTRABASS
  {GW_SAW,GW_SAW,  0,12,65, 50,12,20,60, 45,55,85,40, 45,76},   // TREM STRINGS
  {GW_TRI,GW_SAW, 12, 4,25, 55, 8,35,35,  0,62, 0,10,  0,82},   // PIZZICATO
  {GW_TRI,GW_SIN, 12, 3,35, 60, 0,25,55,  0,82, 0,35,  0,85},   // HARP
  {GW_SIN,GW_NSE,-12, 0,25, 30, 5,35,45,  0,72, 0,30,  0,90},   // TIMPANI
  // 49-56 ensemble
  {GW_SAW,GW_SAW,  0,10,70, 52, 8,12,75, 58,62,90,55, 18,78},   // STRINGS 1
  {GW_SAW,GW_SAW, 12, 9,45, 48, 8,12,75, 65,62,90,58, 16,76},   // STRINGS 2
  {GW_SAW,GW_SAW,  0,14,75, 45,10,15,80, 62,60,92,62, 12,78},   // SYN STRING 1
  {GW_SAW,GW_PUL,  0,16,60, 42,12,15,82, 68,60,92,65, 12,76},   // SYN STRING 2
  {GW_TRI,GW_SIN, 12, 6,50, 42, 5, 8,80, 60,55,92,55, 12,80},   // CHOIR AAHS
  {GW_TRI,GW_TRI,  0, 8,55, 36, 5, 8,80, 58,55,92,52, 12,80},   // VOICE OOHS
  {GW_PUL,GW_TRI,  0,10,55, 45,15,12,78, 58,58,90,55, 15,76},   // SYNTH VOX
  {GW_SAW,GW_SQR,-12, 8,70, 72,15,60,40,  0,70, 0,20,  0,88},   // ORCH HIT
  // 57-64 brass
  {GW_SAW,GW_SQR,  0, 4,25, 48,10,55,68, 22,58,88,22, 15,82},   // TRUMPET
  {GW_SAW,GW_SAW,  0, 5,30, 42,10,52,70, 28,60,88,25, 12,82},   // TROMBONE
  {GW_SAW,GW_SIN,-12, 4,40, 35, 8,45,72, 35,62,90,28,  8,85},   // TUBA
  {GW_PUL,GW_SAW,  0, 5,30, 55,20,50,55, 20,55,85,20, 15,78},   // MUTED TRUMP
  {GW_SAW,GW_TRI,  0, 6,40, 40, 8,45,75, 38,65,90,35, 10,80},   // FRENCH HORN
  {GW_SAW,GW_SAW,  0, 9,60, 50,10,55,70, 25,60,90,28, 12,82},   // BRASS SECT
  {GW_SAW,GW_SAW,-12, 8,55, 52,18,60,65, 18,62,90,25, 10,82},   // SYN BRASS 1
  {GW_SAW,GW_PUL,  0,10,55, 48,22,58,68, 25,64,90,30, 10,80},   // SYN BRASS 2
  // 65-72 reed
  {GW_SAW,GW_PUL,  0, 5,40, 52,12,42,62, 25,58,90,28, 18,80},   // SOPRANO SAX
  {GW_SAW,GW_PUL,  0, 5,42, 48,12,42,64, 28,58,90,30, 18,80},   // ALTO SAX
  {GW_SAW,GW_PUL,  0, 5,45, 44,12,40,66, 30,60,90,32, 16,82},   // TENOR SAX
  {GW_SAW,GW_PUL,  0, 5,45, 38,12,38,68, 34,60,90,34, 14,82},   // BARI SAX
  {GW_PUL,GW_SAW,  0, 4,30, 55,15,35,60, 25,55,90,25, 18,78},   // OBOE
  {GW_PUL,GW_SAW,  0, 4,35, 48,15,35,62, 28,55,90,28, 15,78},   // ENGLISH HORN
  {GW_PUL,GW_SAW,-12, 4,35, 40,12,35,65, 32,58,90,30, 12,80},   // BASSOON
  {GW_SQR,GW_SIN, 12, 3,15, 50, 8,30,62, 25,55,92,28, 15,80},   // CLARINET
  // 73-80 pipe
  {GW_SIN,GW_NSE, 12, 0,10, 72, 0,20,55, 20,50,92,20, 22,80},   // PICCOLO
  {GW_SIN,GW_NSE,  0, 0,12, 60, 0,20,58, 25,52,92,22, 20,82},   // FLUTE
  {GW_TRI,GW_SIN, 12, 3,25, 58, 0,18,55, 22,50,92,20, 15,80},   // RECORDER
  {GW_SIN,GW_NSE,  0, 0,22, 55, 0,22,60, 30,54,92,25, 18,82},   // PAN FLUTE
  {GW_SIN,GW_NSE,  0, 0,35, 45, 5,25,62, 35,56,90,30, 12,80},   // BLOWN BOTTLE
  {GW_SIN,GW_NSE,  0, 0,30, 50, 5,28,65, 40,58,88,35, 20,80},   // SHAKUHACHI
  {GW_SIN,GW_SIN, 12, 4,15, 68, 0,15,50, 15,48,92,18, 28,80},   // WHISTLE
  {GW_SIN,GW_TRI,  0, 4,25, 55, 0,15,52, 18,48,92,18, 15,82},   // OCARINA
  // 81-88 synth lead
  {GW_SQR,GW_SQR,  0,10,55, 62,20,45,55,  5,55,95,20, 12,82},   // LEAD SQUARE
  {GW_SAW,GW_SAW,  0,12,60, 65,18,50,55,  5,55,95,22, 10,84},   // LEAD SAW
  {GW_SIN,GW_NSE, 12, 0,18, 60, 0,30,55, 25,45,92,25, 18,80},   // CALLIOPE
  {GW_SIN,GW_NSE,  0, 0,30, 65,10,55,30,  5,50,92,20, 10,80},   // CHIFF
  {GW_SAW,GW_PUL,  0, 9,50, 58,35,55,45,  4,60,92,20, 12,80},   // CHARANG
  {GW_TRI,GW_SIN,  0, 8,55, 48, 8,15,75, 45,60,92,45, 20,80},   // LEAD VOICE
  {GW_SAW,GW_SAW,  7, 6,70, 60,15,50,55,  8,58,92,25, 10,80},   // LEAD FIFTHS
  {GW_SAW,GW_SQR,-12, 8,65, 55,30,55,48,  4,70,92,20,  8,86},   // BASS + LEAD
  // 89-96 synth pad
  {GW_SIN,GW_TRI, 12, 5,45, 50, 0,10,85, 70,70,92,70, 10,78},   // NEW AGE PAD
  {GW_SAW,GW_SAW,  0, 8,65, 38, 5, 8,85, 72,72,92,72,  8,78},   // WARM PAD
  {GW_SAW,GW_PUL,  0,12,60, 52,10,20,80, 62,68,92,62, 10,78},   // POLYSYNTH
  {GW_TRI,GW_SIN,  0, 9,55, 42, 5, 8,85, 70,70,92,68, 12,78},   // CHOIR PAD
  {GW_SAW,GW_TRI,  0,10,55, 45,20,15,88, 72,72,92,70,  8,76},   // BOWED PAD
  {GW_SQR,GW_SIN, 19, 6,40, 55,25,20,80, 65,68,90,65,  8,74},   // METALLIC PAD
  {GW_SIN,GW_SIN, 12, 7,55, 48, 0,10,88, 75,72,92,75, 10,76},   // HALO PAD
  {GW_SAW,GW_SAW,  0,10,60, 25,30,80,95, 78,70,92,72,  6,76},   // SWEEP PAD
  // 97-104 synth fx
  {GW_SIN,GW_NSE, 24, 0,30, 55, 5,40,75, 55,75, 0,70, 15,76},   // FX RAIN
  {GW_SAW,GW_SAW,  7,10,60, 45,10,25,85, 68,70,90,68, 10,76},   // SOUNDTRACK
  {GW_SIN,GW_SIN, 31, 3,45, 70, 0,25,65,  5,85, 0,60, 10,80},   // CRYSTAL
  {GW_TRI,GW_SIN, 19, 8,45, 45, 5,15,85, 65,72,88,70, 12,76},   // ATMOSPHERE
  {GW_SQR,GW_SIN, 24, 4,40, 75,10,40,70, 10,78, 0,55,  8,78},   // BRIGHTNESS
  {GW_SAW,GW_TRI, -5,15,55, 35,40,35,90, 70,70,85,65, 35,74},   // GOBLINS
  {GW_SIN,GW_SIN, 12, 6,40, 55, 5,20,80, 45,80, 0,75, 15,76},   // ECHOES
  {GW_SAW,GW_NSE,  5,20,45, 40,45,60,85, 60,72,80,60, 40,74},   // SCI-FI
  // 105-112 ethnic
  {GW_SAW,GW_SQR, 12, 8,35, 58,30,45,55,  0,80,20,25, 12,80},   // SITAR
  {GW_PUL,GW_SAW, 12, 5,30, 68,15,40,35,  0,68, 0,10,  0,80},   // BANJO
  {GW_PUL,GW_TRI, 12, 6,30, 62,20,42,40,  0,66, 0,10,  0,80},   // SHAMISEN
  {GW_TRI,GW_SAW, 12, 4,25, 58,10,35,48,  0,76, 0,18,  0,82},   // KOTO
  {GW_SIN,GW_SIN, 24, 3,25, 60, 0,25,32,  0,60, 0,12,  0,85},   // KALIMBA
  {GW_SAW,GW_SAW,  7, 9,65, 50,15, 5, 0, 15,50,95,20, 10,78},   // BAGPIPE
  {GW_SAW,GW_SAW,  0, 8,55, 55,10,15,70, 48,62,88,42, 28,78},   // FIDDLE
  {GW_PUL,GW_SAW,  0, 6,40, 60,20,40,55, 22,55,90,25, 22,76},   // SHANAI
  // 113-120 percussive
  {GW_SIN,GW_SIN, 36, 3,40, 78, 0,15,35,  0,56, 0,15,  0,82},   // TINKLE BELL
  {GW_SIN,GW_SIN, 17, 2,45, 70, 5,20,25,  0,45, 0, 8,  0,82},   // AGOGO
  {GW_SIN,GW_TRI, 12, 8,50, 58,15,30,45,  0,72, 0,25,  8,84},   // STEEL DRUMS
  {GW_SIN,GW_NSE, 12, 0,20, 65, 0,35,15,  0,35, 0, 5,  0,85},   // WOODBLOCK
  {GW_SIN,GW_NSE,-12, 0,30, 28, 5,40,40,  0,66, 0,20,  0,92},   // TAIKO DRUM
  {GW_SIN,GW_NSE,  0, 0,25, 40, 5,45,35,  0,62, 0,15,  0,90},   // MELODIC TOM
  {GW_SIN,GW_NSE,-12, 0,40, 45,15,60,30,  0,58, 0,12,  0,90},   // SYNTH DRUM
  {GW_NSE,GW_SIN,  0, 0,20, 30,20,75,90, 75,88, 0, 5,  0,80},   // REV CYMBAL
  // 121-128 sfx
  {GW_NSE,GW_SAW, 12, 0,25, 55,10,40,20,  0,40, 0, 8,  0,75},   // FRET NOISE
  {GW_NSE,GW_SIN,  0, 0,15, 45, 0,30,35,  5,50, 0,20,  0,75},   // BREATH NOISE
  {GW_NSE,GW_NSE,  0, 0,50, 25, 5,15,90, 80,80,85,80,  0,72},   // SEASHORE
  {GW_SIN,GW_SIN, 24, 8,45, 80, 0,30,20,  0,40, 0,10, 60,78},   // BIRD TWEET
  {GW_SQR,GW_SQR,  2, 0,60, 60,10, 0, 0,  0,50,95, 5,  0,78},   // TELEPHONE
  {GW_NSE,GW_SAW,-24, 0,45, 20,25,20,70, 55,70,90,55, 25,80},   // HELICOPTER
  {GW_NSE,GW_NSE,  0, 0,50, 35, 0,10,85, 65,78,88,70,  0,74},   // APPLAUSE
  {GW_NSE,GW_SIN,-12, 0,30, 50,10,80,15,  0,42, 0,10,  0,90},   // GUNSHOT
};

static const char *const GM_NAMES[128] = {
  "ACOU GRAND", "BRIGHT ACOU", "ELEC GRAND", "HONKY-TONK", "E PIANO 1", "E PIANO 2", "HARPSICHORD", "CLAVINET",
  "CELESTA", "GLOCKENSPIEL", "MUSIC BOX", "VIBRAPHONE", "MARIMBA", "XYLOPHONE", "TUBULAR BELL", "DULCIMER",
  "DRAWBAR ORG", "PERC ORGAN", "ROCK ORGAN", "CHURCH ORG", "REED ORGAN", "ACCORDION", "HARMONICA", "TANGO ACCORD",
  "NYLON GTR", "STEEL GTR", "JAZZ GTR", "CLEAN GTR", "MUTED GTR", "OVERDRIVE", "DISTORTION", "GTR HARMON",
  "ACOUSTIC BS", "FINGER BASS", "PICK BASS", "FRETLESS BS", "SLAP BASS 1", "SLAP BASS 2", "SYNTH BASS 1", "SYNTH BASS 2",
  "VIOLIN", "VIOLA", "CELLO", "CONTRABASS", "TREM STRINGS", "PIZZICATO", "HARP", "TIMPANI",
  "STRINGS 1", "STRINGS 2", "SYN STRING 1", "SYN STRING 2", "CHOIR AAHS", "VOICE OOHS", "SYNTH VOX", "ORCH HIT",
  "TRUMPET", "TROMBONE", "TUBA", "MUTED TRUMP", "FRENCH HORN", "BRASS SECT", "SYN BRASS 1", "SYN BRASS 2",
  "SOPRANO SAX", "ALTO SAX", "TENOR SAX", "BARI SAX", "OBOE", "ENGLISH HORN", "BASSOON", "CLARINET",
  "PICCOLO", "FLUTE", "RECORDER", "PAN FLUTE", "BLOWN BOTTLE", "SHAKUHACHI", "WHISTLE", "OCARINA",
  "LEAD SQUARE", "LEAD SAW", "CALLIOPE", "CHIFF", "CHARANG", "LEAD VOICE", "LEAD FIFTHS", "BASS + LEAD",
  "NEW AGE PAD", "WARM PAD", "POLYSYNTH", "CHOIR PAD", "BOWED PAD", "METALLIC PAD", "HALO PAD", "SWEEP PAD",
  "FX RAIN", "SOUNDTRACK", "CRYSTAL", "ATMOSPHERE", "BRIGHTNESS", "GOBLINS", "ECHOES", "SCI-FI",
  "SITAR", "BANJO", "SHAMISEN", "KOTO", "KALIMBA", "BAGPIPE", "FIDDLE", "SHANAI",
  "TINKLE BELL", "AGOGO", "STEEL DRUMS", "WOODBLOCK", "TAIKO DRUM", "MELODIC TOM", "SYNTH DRUM", "REV CYMBAL",
  "FRET NOISE", "BREATH NOISE", "SEASHORE", "BIRD TWEET", "TELEPHONE", "HELICOPTER", "APPLAUSE", "GUNSHOT",
};
