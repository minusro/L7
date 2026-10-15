#!/usr/bin/env python3
# sf2_gm_bank.py — the GM engine's two outputs:
#  * n9_gm_pcm.cpp/.h — the baked flash part: ONLY the GM drum map now (the
#    freewavesamples one-shots).
#  * ../apps_sd/GM.BNK — the SD bank: all melodic programs at FULL quality
#    (loops and pre-loop middles whole, one-shots to 2 s), one self-contained
#    record per program, each at most a cache slot (104 KB minus the header).
#    N9 flashes the records a project needs into a 960 KB cache region above
#    the app image (n9_gm_cache.cpp); programs stay on synth patches until
#    their record is cached. Copy GM.BNK to the card as /N9/GM.BNK.
# Originally: bakes sounds/Roland_SC-88.sf2 into flash for the GM engine,
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
SLOT_BYTES = 0x1A000          # a cache slot (26 flash sectors)
HDR_BYTES = 128                 # mu-law bytes of PCM, tops
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

RATES = (22050, 18375, 14700, 11025)

def pick_rate(x, srate):
    """The lowest storage rate whose Nyquist still clears the content: the
    frequency below which 99.6% of the energy sits, with 20% headroom."""
    n = min(len(x), 1 << 15)
    mag = np.abs(np.fft.rfft(x[:n] * np.hanning(n))) ** 2
    cum = np.cumsum(mag)
    if cum[-1] <= 0: return RATES[-1]
    f996 = np.searchsorted(cum, 0.996 * cum[-1]) * (srate / 2) / len(mag)
    need = max(2400.0, f996 * 1.2) * 2
    for sr in reversed(RATES):
        if sr >= need: return sr
    return RATES[0]

def encode(y):
    peak = float(np.max(np.abs(y))) or 1.0
    gain = max(16, min(64, int(round(64 * min(1.0, peak / 0.55)))))
    y = y / peak * 0.92
    mu = 255.0
    return np.round((np.sign(y) * np.log1p(mu * np.abs(y)) / np.log1p(mu) + 1) * 127.5).astype(np.uint8), gain

def lowpass_for(x, r):
    if r >= 1.0: return x
    t = np.arange(-31, 32)
    h = np.sinc(t * r) * np.hamming(63)
    return np.convolve(x, (h / h.sum()).astype(np.float32), mode='same')

def add_sample(smp_ix, looped, caps):
    """Loops are NEVER shortened: the font's own loop region is kept whole
    (the middle between the attack and the loop is still spliced out). The
    flash budget comes from a content-aware storage rate per sample (dark
    sounds store slower, compensated in the playback rate) and the one-shot
    caps. The loop region is resampled to an exact integer length, the
    sub-sample rounding compensated in cents (returned with the index)."""
    ONESHOT, ATK_S, PRE_S = caps
    key = (smp_ix, looped, caps)
    if key in cache: return cache[key]
    name, start, end, lstart, lend, srate, pitch, corr, link, typ = SHDR[smp_ix]
    x = SMPL[start:end].astype(np.float32) / 32768.0
    ls, le = lstart - start, lend - start
    use_loop = looped and 8 < le - ls and le <= len(x)
    cents_fix = 0.0
    out_sr = pick_rate(x[ls:le] if use_loop else x, srate)
    r = out_sr / srate
    if use_loop:
        x = x.copy()
        A, PRE = int(ATK_S * srate), int(PRE_S * srate)
        if ls - A > PRE + 1024:                    # splice out the middle before the loop
            c0, c1 = A, ls - PRE
            fade = 384
            seg = x[c1:].copy()
            w = np.linspace(0, 1, fade, dtype=np.float32)
            seg[:fade] = x[c0:c0 + fade] * (1 - w) + seg[:fade] * w
            x = np.concatenate((x[:c0], seg))
            ls -= c1 - c0; le -= c1 - c0
        L = le - ls
        x = lowpass_for(x, r)
        Lp = max(8, int(round(L * r)))
        atk = np.interp(np.arange(0, ls, 1.0 / r), np.arange(len(x)), x) if ls > 0 else np.zeros(0, np.float32)
        loop = np.interp(ls + np.arange(Lp) * (L / Lp), np.arange(len(x)), x)
        cents_fix = 1200.0 * np.log2(Lp / (L * r))
        nls, nle = len(atk), len(atk) + Lp
        y = np.concatenate((atk, loop, loop[:2]))  # the pad the reader's ip+1 needs
    else:
        x = lowpass_for(x, r)
        y = np.interp(np.arange(0, len(x) - 1, 1.0 / r), np.arange(len(x)), x)
        nls = nle = 0
        y = np.ascontiguousarray(y[:int(ONESHOT * out_sr)]).copy()
        fd = min(len(y) // 4, int(0.06 * out_sr))
        if fd > 0: y[-fd:] *= np.linspace(1, 0, fd)
    enc, gain = encode(y)
    off = len(blob)
    blob.extend(enc.tobytes())
    ix = len(samples)
    nm = name.rstrip(b'\0 ').decode('latin1')
    rateQ = int(round(256 * out_sr / 44100))
    samples.append((nm, off, len(y), nls, nle, rateQ, cents_fix, gain))
    cache[key] = ix
    return ix

def pick_vel(zs):
    out = {}
    for z in zs:
        k = (z[0], z[1])
        if k not in out or (z[2] <= 100 <= z[3]) and not (out[k][2] <= 100 <= out[k][3]): out[k] = z
    return list(out.values())

def process(smp_ix, looped, mid_keep, oneshot):
    """One sample at full quality: the loop whole, mid_keep seconds of the
    pre-loop middle (None: all of it), one-shots capped at `oneshot`.
    Returns (mu-law bytes, len, ls, le, rateQ, cents, gain)."""
    name, start, end, lstart, lend, srate, pitch, corr, link, typ = SHDR[smp_ix]
    x = SMPL[start:end].astype(np.float32) / 32768.0
    ls, le = lstart - start, lend - start
    use_loop = looped and 8 < le - ls and le <= len(x)
    cents_fix = 0.0
    out_sr = pick_rate(x[ls:le] if use_loop else x, srate)
    r = out_sr / srate
    if use_loop:
        x = x.copy()
        if mid_keep is not None:
            A, PRE = int(0.16 * srate), int(max(0.04, mid_keep - 0.16) * srate)
            if ls - A > PRE + 1024:
                c0, c1 = A, ls - PRE
                fade = 384
                seg = x[c1:].copy()
                w = np.linspace(0, 1, fade, dtype=np.float32)
                seg[:fade] = x[c0:c0 + fade] * (1 - w) + seg[:fade] * w
                x = np.concatenate((x[:c0], seg))
                ls -= c1 - c0; le -= c1 - c0
        L = le - ls
        x = lowpass_for(x, r)
        Lp = max(8, int(round(L * r)))
        atk = np.interp(np.arange(0, ls, 1.0 / r), np.arange(len(x)), x) if ls > 0 else np.zeros(0, np.float32)
        loop = np.interp(ls + np.arange(Lp) * (L / Lp), np.arange(len(x)), x)
        cents_fix = 1200.0 * np.log2(Lp / (L * r))
        nls, nle = len(atk), len(atk) + Lp
        y = np.concatenate((atk, loop, loop[:2]))
    else:
        x = lowpass_for(x, r)
        y = np.interp(np.arange(0, len(x) - 1, 1.0 / r), np.arange(len(x)), x)
        nls = nle = 0
        y = np.ascontiguousarray(y[:int(oneshot * out_sr)]).copy()
        fd = min(len(y) // 4, int(0.06 * out_sr))
        if fd > 0: y[-fd:] *= np.linspace(1, 0, fd)
    enc, gain = encode(y)
    return enc.tobytes(), len(y), nls, nle, int(round(256 * out_sr / 44100)), cents_fix, gain

def program_record(prog, ver):
    """A self-contained SD record: header + its samples, under SLOT_BYTES.
    Quality steps down (middle, then one-shots, then zones) only if a
    program can't fit a slot whole."""
    zs = pick_vel(preset_zones(0, prog))
    zs = [z for z in zs if z[1] >= 24 and z[0] <= 96]
    zs.sort(key=lambda z: z[0])
    if not zs: return None
    for zone_max in (4, 3, 2, 1):
        zl = zs
        if len(zl) > zone_max:
            zl = [zs[int(round(i * (len(zs) - 1) / max(1, zone_max - 1)))] for i in range(zone_max)]
            seen, z2 = set(), []
            for z in zl:
                if id(z) not in seen: seen.add(id(z)); z2.append(z)
            zl = z2
        for mid_keep, oneshot in ((None, 2.0), (1.2, 1.5), (0.7, 1.0), (0.42, 0.7), (0.25, 0.5)):
            local, datas, zone_rows = {}, [], []
            for (klo, khi, vlo, vhi, smp, root, cents, modes) in zl:
                k = (smp, modes in (1, 3))
                if k not in local:
                    local[k] = len(datas)
                    datas.append(process(smp, modes in (1, 3), mid_keep, oneshot))
                zone_rows.append((local[k], khi, root, cents))
            size = HDR_BYTES + sum(len(d[0]) for d in datas)
            if size <= SLOT_BYTES and len(datas) <= 4:
                hdr = struct.pack('<IIBBBBI', 0x5047394E, ver, prog, len(zone_rows), len(datas), 0,
                                  size - HDR_BYTES)
                for zi in range(4):
                    if zi < len(zone_rows):
                        li, khi, root, cents = zone_rows[zi]
                        hi = khi if zi < len(zone_rows) - 1 else 127
                        c = int(round(cents + datas[li][5]))
                        hdr += struct.pack('<hBBhH', li, hi, root & 0xFF, c, 0)
                    else:
                        hdr += struct.pack('<hBBhH', -1, 127, 60, 0, 0)
                off = 0
                for di in range(4):
                    if di < len(datas):
                        d, ln, ls_, le_, rateQ, _c, gain = datas[di]
                        hdr += struct.pack('<IIIIHBB', off, ln, ls_, le_, rateQ, gain, 0)
                        off += len(d)
                    else:
                        hdr += struct.pack('<IIIIHBB', 0, 0, 0, 0, 0, 0, 0)
                assert len(hdr) == HDR_BYTES
                return hdr + b''.join(d[0] for d in datas), mid_keep is None
    return None

# ---- the GM drum map: the freewavesamples one-shots (the soundfont's
# Standard kit holds only 11 hits - no toms, no Latin percussion).
import wave as wavemod
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
    i0 = int(np.argmax(np.abs(x) > 0.004))
    x = x[max(0, i0 - 32):]
    out_sr = pick_rate(x, 44100)
    r = out_sr / 44100.0
    x = lowpass_for(x, r)
    y = np.interp(np.arange(0, len(x) - 1, 1.0 / r), np.arange(len(x)), x)
    cap = int((0.62 if stem in LONG_DRUM else 0.30) * out_sr)
    y = np.ascontiguousarray(y[:cap]).copy()
    fd = min(len(y) // 4, int(0.05 * out_sr))
    if fd > 0: y[-fd:] *= np.linspace(1, 0, fd)
    enc, gain = encode(y)
    off = len(blob)
    blob.extend(enc.tobytes())
    ix = len(samples)
    samples.append((stem, off, len(y), 0, 0, int(round(256 * out_sr / 44100)), 0.0, min(60, gain)))
    cache[key] = ix
    return ix

def build_drums():
    global blob, samples, cache
    blob, samples, cache = bytearray(), [], {}
    drums_out = [(-1, 256, 64)] * 128
    for note, (stem, shift) in WAV_DRUMS.items():
        six = add_wav_drum(stem, (0, 0, 0))
        drums_out[note] = (six, int(round(256 * 2 ** (shift / 12.0))), 64)
    return drums_out

import zlib
drums_out = build_drums()
print(f'drums baked: {len(samples)} samples, {len(blob)/1024:.0f} KB')

ver = zlib.crc32(open(SF2, 'rb').read(1 << 16)) ^ 0x4E394202   # the bank's identity
records, trimmed = {}, []
for prog in range(128):
    r = program_record(prog, ver)
    if r is None: continue
    records[prog], full = r
    if not full: trimmed.append(prog + 1)
bnk = bytearray()
bnk += struct.pack('<III', 0x4247394E, ver, 128)
dir_off = len(bnk)
bnk += b'\0' * (128 * 8)
for prog in range(128):
    if prog in records:
        rec = records[prog]
        struct.pack_into('<II', bnk, dir_off + prog * 8, len(bnk), len(rec))
        bnk += rec
BNK_PATH = os.path.join(os.path.dirname(__file__), '..', '..', 'apps_sd', 'GM.BNK')
open(BNK_PATH, 'wb').write(bnk)
print(f'GM.BNK: {len(records)}/128 programs, {len(bnk)/1024/1024:.2f} MB '
      f'({len(trimmed)} trimmed to fit a slot: {trimmed})')

with open(OUT_H, 'w') as out:
    out.write('// n9_gm_pcm.h — the baked flash part of GM: the DRUM MAP only (the\n'
              '// melodic programs live on the card as /N9/GM.BNK and cache into flash:\n'
              '// n9_gm_cache.h). Mu-law, mono. GENERATED by tools/sf2_gm_bank.py and\n'
              '// GITIGNORED (not ours to redistribute), do not edit.\n'
              '#pragma once\n#include <stdint.h>\n\n'
              'struct GmSample { uint32_t off, len, loopS, loopE; uint16_t rateQ; uint8_t gain; };\n'
              'struct GmDrumHit { int16_t smp; uint16_t rate256; uint8_t gain; };\n\n'
              f'constexpr uint32_t GM_PCM_BYTES = {len(blob)}u;\n'
              f'constexpr uint16_t GM_SAMPLE_COUNT = {len(samples)};\n'
              f'constexpr uint32_t GM_BANK_VER = {ver}u;   // GM.BNK must match (n9_gm_cache)\n'
              'extern const uint8_t GM_PCM[GM_PCM_BYTES];\n'
              'extern const GmSample GM_SAMPLES[GM_SAMPLE_COUNT];\n'
              'extern const GmDrumHit GM_DRUMMAP[128];\n')

with open(OUT_C, 'w') as out:
    out.write('// n9_gm_pcm.cpp — GENERATED by tools/sf2_gm_bank.py, do not edit.\n'
              '#include "n9_gm_pcm.h"\n#include <Arduino.h>\n\n')
    out.write('PROGMEM const GmSample GM_SAMPLES[GM_SAMPLE_COUNT] = {\n')
    for (nm, off, ln, ls, le, rateQ, _c, gain) in samples:
        out.write(f'  {{{off}u, {ln}u, {ls}u, {le}u, {rateQ}, {gain}}},  // {nm}\n')
    out.write('};\n\nPROGMEM const GmDrumHit GM_DRUMMAP[128] = {\n')
    for n in range(128):
        sm, r_, g = drums_out[n]
        out.write(f'  {{{sm}, {r_}, {g}}},\n')
    out.write('};\n\nPROGMEM const uint8_t GM_PCM[GM_PCM_BYTES] = {\n')
    for i in range(0, len(blob), 24):
        out.write(' ' + ''.join(f'{b},' for b in blob[i:i + 24]) + '\n')
    out.write('};\n')
print(f'wrote the baked drum part: {len(blob)/1024:.0f} KB')
