#!/usr/bin/env python3
# sf2_gm_bank.py — bakes sounds/Roland_SC-88.sf2 into flash for the GM engine,
# replacing sc88_bank.py's one-note rips with the soundfont's real multisamples
# and loop points. Bank 0 presets 0-127 become the melodic programs (up to
# GM_ZONE_MAX key zones each, the velocity layer nearest 100); bank 128's
# Standard kit becomes the GM drum map. Everything lands mono 22.05 kHz mu-law
# in n9_gm_pcm.cpp/.h (same tables as before, plus per-sample cents + gain).
# The outputs stay gitignored (the soundfont isn't ours to redistribute).
# Run from N9/: python3 tools/sf2_gm_bank.py
import os, struct, sys
import numpy as np

SF2 = os.path.join(os.path.dirname(__file__), '..', 'sounds', 'Roland_SC-88.sf2')
WAVDIR = os.path.join(os.path.dirname(__file__), '..', 'sounds', 'SC-88')
OUT_H = os.path.join(os.path.dirname(__file__), '..', 'n9_gm_pcm.h')
OUT_C = os.path.join(os.path.dirname(__file__), '..', 'n9_gm_pcm.cpp')
SR = 22050
BUDGET = 1340 * 1024                 # mu-law bytes of PCM, tops
ZONE_MAX = 4

# ---- SF2 parsing -------------------------------------------------------------
f = open(SF2, 'rb').read()
chunks = {}
def walk(pos, end, prefix=''):
    while pos < end - 8:
        cid = f[pos:pos+4]; sz = struct.unpack('<I', f[pos+4:pos+8])[0]
        body = pos + 8
        if cid == b'LIST': walk(body + 4, body + sz, prefix + f[body:body+4].decode() + '/')
        else: chunks[prefix + cid.decode().strip()] = (body, sz)
        pos = body + sz + (sz & 1)
walk(12, len(f))

def arr(name, fmt, size):
    o, s = chunks[name]
    return [struct.unpack(fmt, f[o+i*size:o+(i+1)*size]) for i in range(s // size)]

PHDR = arr('pdta/phdr', '<20sHHHIII', 38)
PBAG = arr('pdta/pbag', '<HH', 4)
PGEN = arr('pdta/pgen', '<Hh', 4)
INST = arr('pdta/inst', '<20sH', 22)
IBAG = arr('pdta/ibag', '<HH', 4)
IGEN = arr('pdta/igen', '<Hh', 4)
SHDR = arr('pdta/shdr', '<20sIIIIIBbHH', 46)
smpl_off, smpl_sz = chunks['sdta/smpl']
SMPL = np.frombuffer(f, dtype='<i2', count=smpl_sz // 2, offset=smpl_off)

G_KEYR, G_VELR, G_INST, G_COARSE, G_FINE, G_SMP, G_MODES, G_ROOT = 43, 44, 41, 51, 52, 53, 54, 58

def inst_zones(inst_ix, pk_lo=0, pk_hi=127):
    """Instrument zones: (klo, khi, vlo, vhi, smp, root, cents, modes)."""
    zones, glob = [], {}
    b0, b1 = INST[inst_ix][1], INST[inst_ix + 1][1]
    for b in range(b0, b1):
        gens = dict(IGEN[g] for g in range(IBAG[b][0], IBAG[b + 1][0]))
        if G_SMP not in gens:
            glob = gens
            continue
        g = dict(glob); g.update(gens)
        klo, khi = (g.get(G_KEYR, 0x7F00) & 0xFF), (g.get(G_KEYR, 0x7F00) >> 8) & 0x7F
        if G_KEYR in g: klo, khi = g[G_KEYR] & 0xFF, (g[G_KEYR] >> 8) & 0xFF
        else: klo, khi = 0, 127
        vlo, vhi = (g[G_VELR] & 0xFF, (g[G_VELR] >> 8) & 0xFF) if G_VELR in g else (0, 127)
        klo, khi = max(klo, pk_lo), min(khi, pk_hi)
        if klo > khi: continue
        smp = g[G_SMP] & 0xFFFF
        sh = SHDR[smp]
        root = g.get(G_ROOT, -1)
        if root < 0 or root > 127: root = sh[5] if sh[5] < 128 else 60
        cents = g.get(G_FINE, 0) + sh[6] + 100 * g.get(G_COARSE, 0)
        zones.append((klo, khi, vlo, vhi, smp, root, cents, g.get(G_MODES, 0) & 3))
    return zones

def preset_zones(bank, prog):
    for i in range(len(PHDR) - 1):
        name, preset, bk, bag = PHDR[i][0], PHDR[i][1], PHDR[i][2], PHDR[i][3]
        if bk != bank or preset != prog: continue
        out = []
        for b in range(bag, PHDR[i + 1][3]):
            gens = dict(PGEN[g] for g in range(PBAG[b][0], PBAG[b + 1][0]))
            if G_INST not in gens: continue
            klo, khi = (gens[G_KEYR] & 0xFF, (gens[G_KEYR] >> 8) & 0xFF) if G_KEYR in gens else (0, 127)
            out.extend(inst_zones(gens[G_INST] & 0xFFFF, klo, khi))
        return out
    return []

# ---- sample processing -------------------------------------------------------
blob = bytearray()
samples = []                         # (name, off, len, ls, le, root, cents, gain)
cache = {}

def resample(x, srate):
    if srate == SR * 2:              # the common case: clean halfband decimate
        t = np.arange(-31, 32)
        h = np.sinc(t / 2.0) * np.hamming(63); h /= h.sum()
        return np.convolve(x, h, mode='same')[::2], 0.5
    r = SR / srate
    if srate > SR:                   # lowpass to the new Nyquist, then interp
        t = np.arange(-31, 32)
        h = np.sinc(t * r) * np.hamming(63); h /= h.sum()
        x = np.convolve(x, h, mode='same')
    grid = np.arange(0, len(x) - 1, 1.0 / r)
    return np.interp(grid, np.arange(len(x)), x), r

def add_sample(smp_ix, looped, caps):
    BODY, ONESHOT = caps             # kept loop length / one-shot cap, seconds
    ATTACK, PRE = int(0.16 * SR), int(0.07 * SR)
    key = (smp_ix, looped, caps)
    if key in cache: return cache[key]
    name, start, end, lstart, lend, srate, pitch, corr, link, typ = SHDR[smp_ix]
    x = SMPL[start:end].astype(np.float32) / 32768.0
    ls, le = lstart - start, lend - start
    use_loop = looped and 8 < le - ls and le <= len(x)
    y, r = resample(x, srate)
    if use_loop:
        y = np.ascontiguousarray(y).copy()
        nls, nle = int(round(ls * r)), int(round(le * r))
        nle = min(nle, len(y) - 2)
        # the loop kept to BODY seconds (its tail: quasi-stationary)
        if nle - nls > int(BODY * SR): nls = nle - int(BODY * SR)
        # the middle before the loop spliced out (attack kept, crossfaded in)
        if nls - ATTACK > PRE + 512:
            c0, c1 = ATTACK, nls - PRE
            fade = 256
            seg = y[c1:].copy()
            w = np.linspace(0, 1, fade)
            seg[:fade] = y[c0:c0 + fade] * (1 - w) + seg[:fade] * w
            y = np.concatenate((y[:c0], seg))
            nls -= c1 - c0; nle -= c1 - c0
        y = y[:nle + 2]
        fade = min(96, (nle - nls) // 2)
        w = np.linspace(0, 1, fade)
        y[nle - fade:nle] = y[nle - fade:nle] * (1 - w) + y[nls - fade:nls] * w   # the wrap's seam
    else:
        nls = nle = 0
        cap = int(ONESHOT * SR)
        y = np.ascontiguousarray(y[:cap]).copy()
        fd = min(len(y) // 4, int(0.06 * SR))
        if fd > 0: y[-fd:] *= np.linspace(1, 0, fd)
    peak = float(np.max(np.abs(y))) or 1.0
    gain = max(16, min(64, int(round(64 * min(1.0, peak / 0.55)))))
    y = y / peak * 0.92
    mu = 255.0
    enc = np.round((np.sign(y) * np.log1p(mu * np.abs(y)) / np.log1p(mu) + 1) * 127.5).astype(np.uint8)
    off = len(blob)
    blob.extend(enc.tobytes())
    ix = len(samples)
    nm = name.rstrip(b'\0 ').decode('latin1')
    samples.append((nm, off, len(y), nls, nle, 0, 0, gain))   # root/cents live per zone
    cache[key] = ix
    return ix

def pick_vel(zs):
    out = {}
    for z in zs:
        k = (z[0], z[1])
        if k not in out or (z[2] <= 100 <= z[3]) and not (out[k][2] <= 100 <= out[k][3]): out[k] = z
    return list(out.values())

def build(caps, zone_max):
    global blob, samples, cache
    blob, samples, cache = bytearray(), [], {}
    zones_out = [[(-1, 127, 0, 0)] * ZONE_MAX for _ in range(128)]
    for prog in range(128):
        zs = pick_vel(preset_zones(0, prog))
        zs = [z for z in zs if z[1] >= 24 and z[0] <= 96]
        zs.sort(key=lambda z: z[0])
        if not zs: continue
        if len(zs) > zone_max:       # spread the picks over the keyboard
            want = [zs[int(round(i * (len(zs) - 1) / (zone_max - 1)))] for i in range(zone_max)]
            seen, zs2 = set(), []
            for z in want:
                if id(z) not in seen: seen.add(id(z)); zs2.append(z)
            zs = zs2
        for zi, z in enumerate(zs[:zone_max]):
            klo, khi, vlo, vhi, smp, root, cents, modes = z
            six = add_sample(smp, modes in (1, 3), caps)
            hi = khi if zi < len(zs) - 1 and zi < zone_max - 1 else 127
            zones_out[prog][zi] = (six, hi, root, cents)
    drums_out = [(-1, 256, 64)] * 128
    for note, (stem, shift) in WAV_DRUMS.items():
        six = add_wav_drum(stem, caps)
        drums_out[note] = (six, int(round(256 * 2 ** (shift / 12.0))), 64)
    return zones_out, drums_out

# ---- the GM drum map: the freewavesamples one-shots (the soundfont's
# Standard kit holds only 11 hits - no toms, no Latin percussion).
import wave as wavemod, re
WAV_DRUMS = {
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
LONG_DRUM = {'Crash-Cymbal','Crash-Cymbal-2','Splash-Cymbal','Chinese-Cymbal','Ride-Cymbal',
             'Ride-Cymbal-2','Ride-Bell','Open-Hi-Hat','Timpani','Snare-Roll','Taiko-Drum',
             'Open-Cuica','Open-Surdo','Jingle-Bell'}

def add_wav_drum(stem, caps):
    key = ('wav', stem)
    if key in cache: return cache[key]
    w = wavemod.open(os.path.join(WAVDIR, 'Roland-SC-88-' + stem + '.wav'))
    d = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float32).reshape(-1, 2)
    w.close()
    x = d.mean(axis=1) / 32768.0
    t = np.arange(-31, 32)
    h = np.sinc(t / 2.0) * np.hamming(63); h /= h.sum()
    y = np.convolve(x, h, mode='same')[::2]
    i0 = int(np.argmax(np.abs(y) > 0.004))
    y = y[max(0, i0 - 16):]
    cap = int((0.62 if stem in LONG_DRUM else 0.30) * SR)
    y = y[:cap]
    fd = min(len(y) // 4, int(0.05 * SR))
    if fd > 0: y[-fd:] *= np.linspace(1, 0, fd)
    peak = float(np.max(np.abs(y))) or 1.0
    y = y / peak * 0.92
    mu = 255.0
    enc = np.round((np.sign(y) * np.log1p(mu * np.abs(y)) / np.log1p(mu) + 1) * 127.5).astype(np.uint8)
    off = len(blob)
    blob.extend(enc.tobytes())
    ix = len(samples)
    samples.append((stem, off, len(y), 0, 0, 0, 0, 60))
    cache[key] = ix
    return ix

ATTEMPTS = [(4, 0.30, 0.50), (4, 0.24, 0.45), (4, 0.19, 0.42), (3, 0.24, 0.45),
            (3, 0.19, 0.40), (3, 0.15, 0.36), (2, 0.18, 0.40), (2, 0.14, 0.34)]
for zone_max, body, oneshot in ATTEMPTS:
    caps = (body, oneshot)
    zones_out, drums_out = build(caps, zone_max)
    total = len(blob)
    print(f'zones={zone_max} loop={body}s shot={oneshot}s: {len(samples)} samples, {total/1024:.0f} KB')
    if total <= BUDGET: break

with open(OUT_H, 'w') as out:
    out.write('// n9_gm_pcm.h — the GM engine\'s baked samples, from the SC-88 soundfont\n'
              '// (tools/sf2_gm_bank.py). Mu-law, 22050 Hz mono; loopE == 0: one-shot.\n'
              '// GENERATED and GITIGNORED (not ours to redistribute), do not edit.\n'
              '#pragma once\n#include <stdint.h>\n\n'
              'struct GmSample { uint32_t off, len, loopS, loopE; uint8_t gain; };\n'
              'struct GmZone { int16_t smp; uint8_t hi, root; int16_t cents; };\n'
              'struct GmDrumHit { int16_t smp; uint16_t rate256; uint8_t gain; };\n\n'
              f'constexpr uint32_t GM_PCM_BYTES = {len(blob)}u;\n'
              f'constexpr uint16_t GM_SAMPLE_COUNT = {len(samples)};\n'
              f'constexpr uint8_t GM_ZONE_MAX = {ZONE_MAX};\n'
              'extern const uint8_t GM_PCM[GM_PCM_BYTES];\n'
              'extern const GmSample GM_SAMPLES[GM_SAMPLE_COUNT];\n'
              'extern const GmZone GM_ZONES[128][GM_ZONE_MAX];\n'
              'extern const GmDrumHit GM_DRUMMAP[128];\n')

with open(OUT_C, 'w') as out:
    out.write('// n9_gm_pcm.cpp — GENERATED by tools/sf2_gm_bank.py, do not edit.\n'
              '#include "n9_gm_pcm.h"\n#include <Arduino.h>\n\n')
    out.write('PROGMEM const GmSample GM_SAMPLES[GM_SAMPLE_COUNT] = {\n')
    for (nm, off, ln, ls, le, _r, _c, gain) in samples:
        out.write(f'  {{{off}u, {ln}u, {ls}u, {le}u, {gain}}},  // {nm}\n')
    out.write('};\n\nPROGMEM const GmZone GM_ZONES[128][GM_ZONE_MAX] = {\n')
    for p in range(128):
        z = ', '.join(f'{{{s}, {hi}, {root}, {cents}}}' for (s, hi, root, cents) in zones_out[p])
        out.write(f'  {{{z}}},\n')
    out.write('};\n\nPROGMEM const GmDrumHit GM_DRUMMAP[128] = {\n')
    for n in range(128):
        s, r, g = drums_out[n]
        out.write(f'  {{{s}, {r}, {g}}},\n')
    out.write('};\n\nPROGMEM const uint8_t GM_PCM[GM_PCM_BYTES] = {\n')
    for i in range(0, len(blob), 24):
        out.write(' ' + ''.join(f'{b},' for b in blob[i:i + 24]) + '\n')
    out.write('};\n')
covered = sum(1 for p in range(128) if zones_out[p][0][0] >= 0)
dnotes = sum(1 for n in range(128) if drums_out[n][0] >= 0)
print(f'wrote bank: {covered}/128 programs, {dnotes} drum notes, {len(blob)/1024:.0f} KB')
