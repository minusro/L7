#!/usr/bin/env python3
# sc88_bank.py — bakes the SC-88 samples (sounds/SC-88/*.wav, freewavesamples.com)
# into flash for the GM engine: stereo 44.1k -> mono 22.05k (halfband FIR, decimate),
# trimmed, normalized, sustained instruments auto-looped (best-match loop with a
# crossfaded seam), mu-law encoded. Emits n9_gm_pcm.h (tables) + n9_gm_pcm.cpp
# (the PCM blob + GM program zones + the GM drum map). Run from N9/: python3 tools/sc88_bank.py
import os, re, wave, sys
import numpy as np

SRC = os.path.join(os.path.dirname(__file__), '..', 'sounds', 'SC-88')
OUT_H = os.path.join(os.path.dirname(__file__), '..', 'n9_gm_pcm.h')
OUT_C = os.path.join(os.path.dirname(__file__), '..', 'n9_gm_pcm.cpp')
SR = 22050

# ---- classification ---------------------------------------------------------
# Sustained (auto-looped): everything blown, bowed, held or pad-like.
SUSTAIN = {
 'Accordion','Tango-Accordion','Harmonica','Drawbar-Organ','Percussive-Organ',
 'Violin','Viola','Cello','Contrabass','Tremolo-Strings','String-Ensemble',
 'Choir-Aah','Choir-Pad','Voice-Lead','Trombone','Tuba','Muted-Trumpet','French-Horn',
 'Soprano-Sax','Alto-Sax','Tenor-Sax','Baritone-Sax','Oboe','English-Horn','Bassoon',
 'Piccolo','Recorder','Pan-Flute','Blown-Bottle','Shakuhachi','Whistle','Ocarina',
 'Calliope-Lead','Chiff-Lead','Charang-Lead','Fifths-Lead','Bass-Lead',
 'New-Age-Pad','Bowed-Pad','Metallic-Pad','Halo-Pad','Bagpipe','Fiddle','Shanai',
 'Rain-FX','Seashore','Telephone-Ring',
}
LONG_SHOT = {'Grand-Piano','Harpsichord','Tubular-Bell','Crystal-FX','Echo-FX','Reverse-Cymbal',
             'Chirping-Birds','Applause','Vibraphone'}
# drum one-shots that ring long
LONG_DRUM = {'Crash-Cymbal','Crash-Cymbal-2','Splash-Cymbal','Chinese-Cymbal','Ride-Cymbal',
             'Ride-Cymbal-2','Ride-Bell','Open-Hi-Hat','Timpani','Snare-Roll','Taiko-Drum',
             'Open-Cuica','Open-Surdo','Jingle-Bell'}

# ---- GM program -> sample zones ({upper note or 127: stem}); C4 = MIDI 60 ----
MELODIC = {  # program (1-based): [(hiNote, stem)], root from the filename
  1: [(36,'Grand-Piano-C1'),(60,'Grand-Piano-C3'),(127,'Grand-Piano-C5')],
  7: [(48,'Harpsichord-C2'),(127,'Harpsichord-C4')],
  9: 'Celesta-C4', 10:'Glockenspiel-C5', 11:'Music-Box-C5', 12:'Vibraphone-C5',
  13:'Marimba-C4', 14:'Xylophone-C6', 15:'Tubular-Bell-C6', 16:'Dulcimer-C5',
  17:'Drawbar-Organ-C3', 18:'Percussive-Organ-C3', 22:'Accordion-C4', 23:'Harmonica-C5',
  24:'Tango-Accordion-C5',
  25:'Nylon-String-Guitar-C4', 26:'Steel-String-Guitar-C3', 27:'Jazz-Guitar-C4',
  29:'Muted-Electric-Guitar-C3', 30:'Overdriven-Guitar-C3', 31:'Distorted-Guitar-C3',
  33:'Acoustic-Bass-C2', 35:'Picked-Electric-Bass-C2', 36:'Fretless-Bass-C3', 37:'Slap-Bass-C2',
  41:'Violin-C5', 42:'Viola-C4', 43:'Cello-C3', 44:'Contrabass-C2', 45:'Tremolo-Strings-C5',
  47:'Harp-C4', 48:'Timpani',
  49:'String-Ensemble-C5', 53:'Choir-Aah-C5', 56:'Orchestra-Hit',
  58:'Trombone-C5', 59:'Tuba-C3', 60:'Muted-Trumpet-C5', 61:'French-Horn-C4',
  65:'Soprano-Sax-C5', 66:'Alto-Sax-C5', 67:'Tenor-Sax-C4', 68:'Baritone-Sax-C3',
  69:'Oboe-C5', 70:'English-Horn-C5', 71:'Bassoon-C4',
  73:'Piccolo-C6', 75:'Recorder-C6', 76:'Pan-Flute-C5', 77:'Blown-Bottle-C5',
  78:'Shakuhachi-C6', 79:'Whistle-C6', 80:'Ocarina-C6',
  83:'Calliope-Lead-C5', 84:'Chiff-Lead-C5', 85:'Charang-Lead-C5', 86:'Voice-Lead-C5',
  87:'Fifths-Lead-C5', 88:'Bass-Lead-C3',
  89:'New-Age-Pad-C4', 92:'Choir-Pad-C5', 93:'Bowed-Pad-C4', 94:'Metallic-Pad-C4',
  95:'Halo-Pad-C5',
  97:'Rain-FX-C4', 99:'Crystal-FX-C6', 101:'Brightness-FX-C5', 103:'Echo-FX-C6',
  105:'Sitar-C4', 106:'Banjo-C4', 107:'Shamisen-C4', 108:'Koto-C5', 109:'Kalimba-C5',
  110:'Bagpipe-C4', 111:'Fiddle-C5', 112:'Shanai-C5',
  114:'Agogo-C6', 115:'Steel-Drum-C5', 117:'Taiko-Drum', 118:'Melodic-Tom-C3',
  119:'Synth-Drum-C3', 120:'Reverse-Cymbal',
  121:'Fret-Noise', 122:'Breath-Noise', 123:'Seashore', 124:'Chirping-Birds',
  125:'Telephone-Ring', 127:'Applause', 128:'Gunshot',
}
# Unpitched one-shots get this root (played back as recorded at C4).
UNPITCHED_ROOT = 60

# ---- GM drum map: MIDI note -> (stem, semitone shift) -----------------------
DRUMS = {
  27:('High-Q',0), 29:('Scratch-Push',0), 30:('Scratch-Pull',0), 31:('Sticks',0),
  32:('Square-Click',0), 34:('Metronome-Bell',0),
  35:('Bass-Drum-2',0), 36:('Bass-Drum',0), 37:('Sticks',-2), 38:('Snare-Drum',0),
  39:('Hand-Clap',0), 40:('Snare-Drum-2',0), 41:('Low-Tom-2',0), 42:('Closed-Hi-Hat',0),
  43:('Low-Tom',0), 44:('Pedal-Hi-Hat',0), 45:('Mid-Tom-2',0), 46:('Open-Hi-Hat',0),
  47:('Mid-Tom',0), 48:('High-Tom-2',0), 49:('Crash-Cymbal',0), 50:('High-Tom',0),
  51:('Ride-Cymbal',0), 52:('Chinese-Cymbal',0), 53:('Ride-Bell',0), 54:('Tambourine',0),
  55:('Splash-Cymbal',0), 56:('Cowbell',0), 57:('Crash-Cymbal-2',0), 58:('Jingle-Bell',-5),
  59:('Ride-Cymbal-2',0), 60:('High-Bongo',0), 61:('Low-Bongo',0), 62:('Mute-High-Conga',0),
  63:('Open-High-Conga',0), 64:('Low-Conga',0), 65:('Mid-Tom',4), 66:('Low-Tom',4),
  67:('Agogo-C6',0), 68:('Agogo-C6',-5), 69:('Cabasa',0), 70:('Maracas',0),
  71:('Whistle-C6',7), 72:('Whistle-C6',0), 73:('Cabasa',-7), 74:('Maracas',-7),
  75:('Claves',0), 76:('Wood-Block',0), 77:('Wood-Block',-5), 78:('Mute-Cuica',0),
  79:('Open-Cuica',0), 80:('Ride-Bell',12), 81:('Ride-Bell',7),
  82:('Cabasa',2), 83:('Jingle-Bell',0), 85:('Claves',-4), 86:('Mute-Surdo',0), 87:('Open-Surdo',0),
}

NOTE_RE = re.compile(r'-([A-G]#?)(\d)$')
SEMI = {'C':0,'C#':1,'D':2,'D#':3,'E':4,'F':5,'F#':6,'G':7,'G#':8,'A':9,'A#':10,'B':11}
def root_of(stem):
    m = NOTE_RE.search(stem)
    return 12 * (int(m.group(2)) + 1) + SEMI[m.group(1)] if m else UNPITCHED_ROOT

def load(stem):
    w = wave.open(os.path.join(SRC, 'Roland-SC-88-' + stem + '.wav'))
    n = w.getnframes()
    d = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float32).reshape(-1, 2)
    w.close()
    mono = d.mean(axis=1) / 32768.0
    # halfband FIR (63 taps, Hamming) then decimate by 2
    t = np.arange(-31, 32)
    h = np.sinc(t / 2.0) * np.hamming(63); h /= h.sum()
    return np.convolve(mono, h, mode='same')[::2]

def trim_lead(x):
    idx = np.argmax(np.abs(x) > 0.004)
    return x[max(0, idx - 16):]

def find_loop(x, attack, max_end):
    # loop end fixed near max_end; pick the loop length whose start window best
    # matches the end window, then crossfade the seam.
    end = min(len(x) - 64, max_end)
    W = 384
    best, bestL = 1e9, 1600
    for L in range(700, min(3600, end - attack - W), 16):
        a = x[end - W:end]; b = x[end - L - W:end - L]
        d = float(np.mean((a - b) ** 2))
        if d < best: best, bestL = d, L
    s, e = end - bestL, end
    fade = min(256, bestL // 2)
    w = np.linspace(0, 1, fade)
    x[e - fade:e] = x[e - fade:e] * (1 - w) + x[s - fade:s] * w
    return x[:e], s, e

def mulaw(x):
    x = np.clip(x, -1, 1)
    mu = 255.0
    y = np.sign(x) * np.log1p(mu * np.abs(x)) / np.log1p(mu)
    return np.round((y + 1) * 127.5).astype(np.uint8)

blob = bytearray()
samples = []     # (name, off, len, loopS, loopE, root, gain)
index = {}

def add(stem, drum=False):
    key = stem
    if key in index: return index[key]
    x = trim_lead(load(stem))
    base = stem.rsplit('-C', 1)[0] if NOTE_RE.search(stem) else stem
    if base in SUSTAIN:
        atk = int(0.22 * SR)
        x, ls, le = find_loop(x, atk, int(0.38 * SR))
    else:
        if base in LONG_SHOT: cap = int(0.70 * SR)
        elif drum: cap = int((0.70 if stem in LONG_DRUM or base in LONG_DRUM else 0.32) * SR)
        else: cap = int(0.45 * SR)
        x = x[:cap]
        f = min(len(x) // 4, int(0.08 * SR))
        if f > 0: x[-f:] *= np.linspace(1, 0, f)
        ls = le = 0
    peak = float(np.max(np.abs(x))) or 1.0
    x = x / peak * 0.92
    off = len(blob)
    blob.extend(mulaw(x).tobytes())
    idx = len(samples)
    samples.append((stem, off, len(x), ls, le, root_of(stem), 64))
    index[key] = idx
    return idx

# melodic zones
zones = [[(-1, 127)] * 3 for _ in range(128)]
for prog, spec in MELODIC.items():
    zs = spec if isinstance(spec, list) else [(127, spec)]
    for zi, (hi, stem) in enumerate(zs):
        zones[prog - 1][zi] = (add(stem), hi)

# drum map
drums = [(-1, 256, 64)] * 128
for note, (stem, shift) in DRUMS.items():
    si = add(stem, drum=True)
    drums[note] = (si, int(round(256 * 2 ** (shift / 12.0))), 64)

total = len(blob)
print(f'{len(samples)} samples, {total} bytes ({total/1024:.0f} KB)')

with open(OUT_H, 'w') as f:
    f.write('// n9_gm_pcm.h — the GM engine\'s baked SC-88 samples (tools/sc88_bank.py).\n'
            '// Mu-law, 22050 Hz mono; loopE == 0 means one-shot. GENERATED, do not edit.\n'
            '#pragma once\n#include <stdint.h>\n\n'
            'struct GmSample { uint32_t off, len, loopS, loopE; uint8_t root, gain; };\n'
            'struct GmZone { int16_t smp; uint8_t hi; };      // per program, up to 3; smp -1: none\n'
            'struct GmDrumHit { int16_t smp; uint16_t rate256; uint8_t gain; };\n\n'
            f'constexpr uint32_t GM_PCM_BYTES = {total}u;\n'
            f'constexpr uint16_t GM_SAMPLE_COUNT = {len(samples)};\n'
            'extern const uint8_t GM_PCM[GM_PCM_BYTES];\n'
            'extern const GmSample GM_SAMPLES[GM_SAMPLE_COUNT];\n'
            'extern const GmZone GM_ZONES[128][3];\n'
            'extern const GmDrumHit GM_DRUMMAP[128];\n')

with open(OUT_C, 'w') as f:
    f.write('// n9_gm_pcm.cpp — GENERATED by tools/sc88_bank.py, do not edit.\n'
            '#include "n9_gm_pcm.h"\n#include <Arduino.h>\n\n')
    f.write('PROGMEM const GmSample GM_SAMPLES[GM_SAMPLE_COUNT] = {\n')
    for (name, off, ln, ls, le, root, gain) in samples:
        f.write(f'  {{{off}u, {ln}u, {ls}u, {le}u, {root}, {gain}}},  // {name}\n')
    f.write('};\n\nPROGMEM const GmZone GM_ZONES[128][3] = {\n')
    for p in range(128):
        z = ', '.join(f'{{{s}, {hi}}}' for (s, hi) in zones[p])
        f.write(f'  {{{z}}},\n')
    f.write('};\n\nPROGMEM const GmDrumHit GM_DRUMMAP[128] = {\n')
    for n in range(128):
        s, r, g = drums[n]
        f.write(f'  {{{s}, {r}, {g}}},\n')
    f.write('};\n\nPROGMEM const uint8_t GM_PCM[GM_PCM_BYTES] = {\n')
    for i in range(0, total, 24):
        f.write(' ' + ''.join(f'{b},' for b in blob[i:i + 24]) + '\n')
    f.write('};\n')
print('wrote', OUT_H, 'and', OUT_C)
