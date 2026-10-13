#!/usr/bin/env python3
# m1_bank.py — generates n9_m1_bank.h from the Korg M1 factory sysex dump
# (N9/sounds/M1 soundbank/Int_M1.syx): the 100 programs' names and the
# parameters the M1 engine plays from, plus a category per internal
# multisound for the stand-in oscillators (the PCM itself is not in the
# dump — the emulation pairs the real parameters with synthetic sources).
#
# Format: M1 MIDI implementation (the manual's Tables 1 and 5). An "All
# Program Parameter Dump" (function 4C) carries one bank byte, then 100 x 143
# parameter bytes packed 7-bit (one MSB byte before each 7 data bytes).
#
#   python3 tools/m1_bank.py "sounds/M1 soundbank/Int_M1.syx" n9_m1_bank.h
import sys

def messages(d):
    i = 0
    while i < len(d):
        if d[i] == 0xF0:
            j = d.index(0xF7, i)
            yield d[i:j + 1]
            i = j + 1
        else:
            i += 1

def unpack(payload):
    out = bytearray()
    for g in range(0, len(payload), 8):
        grp = payload[g:g + 8]
        for k, b in enumerate(grp[1:]):
            out.append(b | (0x80 if grp[0] & (1 << k) else 0))
    return bytes(out)

def s8(b):
    return b - 256 if b >= 128 else b

# The 100 internal multisounds (manual p.50), each mapped to a stand-in
# category: 0 SAW, 1 PULSE, 2 SUPER (detuned saws), 3 SINE, 4 ORGAN
# (additive sines), 5 BELL (FM 3.5:1), 6 EP (FM 1:1 soft), 7 PLUCK (FM
# fast-decay), 8 NOISE.
CATS = ['SAW', 'PULSE', 'SUPER', 'SINE', 'ORGAN', 'BELL', 'EP', 'PLUCK', 'NOISE']
MS_CAT = {
    0: 6, 1: 6, 2: 6, 3: 6, 4: 6,                      # PIANO, E.PIANO1/2, CLAV, HARPSICRD
    5: 4, 6: 4, 7: 4,                                  # ORGAN1/2, MAGICORG
    8: 7, 9: 7, 10: 7, 11: 7, 12: 7,                   # GUITAR1/2, E.GUITAR, SITAR1/2
    13: 7, 14: 7, 15: 7, 16: 7,                        # A.BASS, PICKBASS, E.BASS, FRETLESS
    17: 0, 18: 0,                                      # SYNBASS1/2
    19: 5, 20: 5, 21: 5, 22: 5, 23: 5, 24: 5, 25: 5,   # VIBES..SYNMALLET
    26: 3, 27: 3, 28: 3,                               # FLUTE, PANFLUTE, BOTTLES
    29: 2, 30: 2, 31: 2,                               # VOICES, CHOIR, STRINGS
    32: 0, 33: 0, 34: 0, 35: 0, 36: 0, 37: 0, 38: 0,   # BRASS..DBLREED
    39: 7, 40: 7,                                      # KOTOTREM, BMBOOTREM
    41: 8, 42: 8, 43: 8, 44: 5, 45: 5,                 # RHYTHM, LORE(NT), FLEXATONE, WINDBELLS
    46: 5, 47: 5, 48: 8, 49: 8,                        # POLE(NT), BLOCK(NT)
    50: 8, 51: 8, 52: 8, 53: 8, 54: 3, 55: 3,          # FINGRSNAP, POP, DROP(NT), BREATH(NT)
    56: 7, 57: 7, 58: 5, 59: 5, 60: 8, 61: 5, 62: 5,   # PLUCK(NT), VIBEHIT(NT), HAMMER, METALHIT(NT)
    63: 7, 64: 0, 65: 0,                               # PICK, DISTORTN(NT)
    66: 7, 67: 7, 68: 7, 69: 5,                        # BASSTHUMB, BASTHUM1/2, WIRE
    70: 1, 71: 5, 72: 1, 73: 1,                        # PANWAVE, PINGWAVE, FVWAVE, MVWAVE
    74: 2, 75: 2, 76: 2,                               # VOICEWV(1/2)
    77: 6, 78: 6, 79: 6, 80: 6, 81: 6, 82: 5,          # DWGS EP1-3, PIANO, CLAV, VIBE1
    83: 0, 84: 0, 85: 5, 86: 4, 87: 4, 88: 2,          # DWGS BASS1/2, BELL1, ORGN1/2, VOICE
    89: 1, 90: 0, 91: 0, 92: 0,                        # SQUAREWV, DIGITAL1, SAWWAVE, DIGITAL2
    93: 1, 94: 1, 95: 0, 96: 0, 97: 0, 98: 3, 99: 3,   # 25%/10%PULSE, DIGITAL3-5, DWGS TRI/SINE
}

def main(syx_path, out_path):
    data = open(syx_path, 'rb').read()
    body = None
    for m in messages(data):
        if m[4] == 0x4C:                       # All Program Parameter Dump
            body = unpack(m[6:-1])             # m[5] is the bank byte (0 = internal)
    assert body and len(body) >= 100 * 143, 'no all-program dump found'

    progs = []
    for p in range(100):
        r = body[p * 143:(p + 1) * 143]
        # this dump stores some in-name spaces as NUL bytes
        name = ''.join(' ' if c == 0 else chr(c) for c in r[0:10])
        assert all(c == 0 or 32 <= c < 127 for c in r[0:10]), f'bad name in program {p}: {name!r}'
        progs.append({
            'name': name,
            'oscMode': r[10],                  # 0 single, 1 double, 2 drums
            'ms1': r[12], 'oct1': s8(r[13]),   # multisound, octave -1/0/1 (16'/8'/4')
            'ms2': r[14], 'oct2': s8(r[15]),
            'interval': s8(r[16]), 'detune': s8(r[17]),
            'vdfCutoff': r[71], 'vdfEgInt': r[74],
            'vdaLevel': r[86], 'ampVel': s8(r[89]),
            'aA': r[92], 'aAl': r[93], 'aD': r[94], 'aB': r[95],
            'aSl': r[96], 'aS': r[97], 'aR': r[98],
            # VDF EG (times 0-99, levels signed)
            'fA': r[78], 'fAl': s8(r[79]), 'fD': r[80], 'fB': s8(r[81]),
            'fSl': r[82], 'fS': s8(r[83]), 'fR': r[84], 'fRl': s8(r[85]),
            'kbdCenter': r[72], 'cutKbd': s8(r[73]), 'egIntVel': s8(r[77]),
            # osc2's own VDA EG and level (double mode)
            'vda2Level': r[126], 'amp2Vel': s8(r[129]),
            'bA': r[132], 'bAl': r[133], 'bD': r[134], 'bB': r[135],
            'bSl': r[136], 'bS': r[137], 'bR': r[138],
            # pitch EG
            'pStartL': s8(r[63]), 'pAtkT': r[64], 'pAtkL': s8(r[65]),
            'pDecT': r[66], 'pRelT': r[67], 'pRelL': s8(r[68]),
            # pitch MG and VDF MG (waveform bits, freq, delay, intensity)
            'mgWave': r[19], 'mgFreq': r[20], 'mgInt': r[22],
            'fmgWave': r[23], 'fmgFreq': r[24], 'fmgInt': r[26],
            # effects: pattern (raw 0-32 = 1-33, 33 = thru), wet = balance/100
            'fx1': r[38], 'fx1Wet': (r[40] + r[41]) // 2,
            'fx2': r[39], 'fx2Wet': (r[42] + r[43]) // 2,
        })

    with open(out_path, 'w') as f:
        f.write('// n9_m1_bank.h — generated by tools/m1_bank.py from the M1 factory dump\n')
        f.write('// (sounds/M1 soundbank/Int_M1.syx). Do not edit; run the tool again.\n')
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write('constexpr uint8_t M1_PROGRAMS = 100;\n\n')
        f.write('// Stand-in oscillator per internal multisound: ' + ' '.join(f'{i}={c}' for i, c in enumerate(CATS)) + '\n')
        f.write('enum M1Cat : uint8_t { M1C_SAW, M1C_PULSE, M1C_SUPER, M1C_SINE, M1C_ORGAN, M1C_BELL, M1C_EP, M1C_PLUCK, M1C_NOISE };\n')
        cats = ', '.join(str(MS_CAT[i]) for i in range(100))
        f.write(f'PROGMEM const uint8_t M1_MS_CAT[100] = {{{cats}}};\n\n')
        f.write('struct M1Program {\n'
                '  char name[11];\n'
                '  uint8_t oscMode;              // 0 single, 1 double, 2 drums\n'
                '  uint8_t ms1; int8_t oct1;     // multisound (>=100: card), octave -1/0/1\n'
                '  uint8_t ms2; int8_t oct2;\n'
                '  int8_t interval, detune;      // osc2: semitones, cents\n'
                '  uint8_t vdfCutoff, vdfEgInt;\n'
                '  uint8_t vdaLevel; int8_t ampVel;\n'
                '  uint8_t aA, aAl, aD, aB, aSl, aS, aR;   // VDA1 EG\n'
                '  uint8_t fA; int8_t fAl; uint8_t fD; int8_t fB; uint8_t fSl; int8_t fS; uint8_t fR; int8_t fRl;   // VDF EG\n'
                '  uint8_t kbdCenter; int8_t cutKbd, egIntVel;\n'
                '  uint8_t vda2Level; int8_t amp2Vel;\n'
                '  uint8_t bA, bAl, bD, bB, bSl, bS, bR;   // VDA2 EG\n'
                '  int8_t pStartL; uint8_t pAtkT; int8_t pAtkL; uint8_t pDecT, pRelT; int8_t pRelL;   // pitch EG\n'
                '  uint8_t mgWave, mgFreq, mgInt;          // pitch MG\n'
                '  uint8_t fmgWave, fmgFreq, fmgInt;       // VDF MG\n'
                '  uint8_t fx1, fx1Wet, fx2, fx2Wet;       // effects: raw pattern 0-32=1-33, 33=thru; wet 0-100\n'
                '};\n\n')
        f.write('PROGMEM const M1Program M1_BANK[100] = {\n')
        for p in progs:
            n = p['name'].replace('\\', '\\\\').replace('"', '\\"')
            f.write('    {"%s", ' % n + ', '.join(str(p[k]) for k in ['oscMode', 'ms1', 'oct1', 'ms2', 'oct2', 'interval', 'detune', 'vdfCutoff', 'vdfEgInt', 'vdaLevel', 'ampVel', 'aA', 'aAl', 'aD', 'aB', 'aSl', 'aS', 'aR', 'fA', 'fAl', 'fD', 'fB', 'fSl', 'fS', 'fR', 'fRl', 'kbdCenter', 'cutKbd', 'egIntVel', 'vda2Level', 'amp2Vel', 'bA', 'bAl', 'bD', 'bB', 'bSl', 'bS', 'bR', 'pStartL', 'pAtkT', 'pAtkL', 'pDecT', 'pRelT', 'pRelL', 'mgWave', 'mgFreq', 'mgInt', 'fmgWave', 'fmgFreq', 'fmgInt', 'fx1', 'fx1Wet', 'fx2', 'fx2Wet']) + '},\n')
        f.write('};\n')
    print(f'wrote {out_path}: 100 programs')
    for i in range(0, 100, 5):
        print(' '.join(f'{i+k:02d}:{progs[i+k]["name"]}' for k in range(5)))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
