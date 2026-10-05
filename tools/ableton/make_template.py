#!/usr/bin/env python3
"""Makes ableton_template.h (the Live Set export's templates, ableton.cpp) from a
Live Set saved by Ableton Live (docs/design/ableton/L7 Test 1 Project/L7 Test 1.als,
Live 11.3): its first track (with its two Auto Filters; any other device left
out), an empty clip slot, a clip, a scene and the set around them, as text
with fields marked "\\001" + a letter for ableton.cpp to fill in.

  python3 tools/ableton/make_template.py "docs/design/ableton/L7 Test 1 Project/L7 Test 1.als" ableton_template.h

Fields:
  set:   N NextPointeeId   T tempo   R the tracks   C the scenes
  track: I its list id   n its number   c its colour index   i a fresh id (in order: ALS_TRACK_IDS of them)
         M its clip slots   F its freeze slots   S the slot playing when saved (-2: none)
  slot:  k its index
  clip:  c colour index   k index  b/e start/end  s/l loop start/end  r start, from the loop's start
         o end marker  m name  E its envelopes  d disabled (muted)  p/P file path
         z file size  u frames  w/W the second warp marker's seconds/beats
  scene: k its index   m its name
"""
import gzip
import re
import sys
import xml.etree.ElementTree as ET

MARK = '\001'
ID_BASE = 20000          # the tracks' ids from here (the set's own stay below)


def element(text, start_tag, start=0):
    """The element starting with start_tag (an opening tag's beginning) at or after start: (begin, end)."""
    i = text.index(start_tag, start)
    name = re.match(r'<(\w+)', start_tag).group(1)
    depth, k = 0, i
    pat = re.compile(r'<(/?)' + name + r'\b[^>]*?(/?)>')
    for m in pat.finditer(text, i):
        if m.group(1):
            depth -= 1
        elif not m.group(2):
            depth += 1
        else:
            if depth == 0:
                return i, m.end()
            continue
        if depth == 0:
            return i, m.end()
    raise ValueError(start_tag)


def cut(text, start_tag, start=0):
    """text without the element (and the whitespace before it)."""
    i, j = element(text, start_tag, start)
    while i > 0 and text[i - 1] in ' \t':
        i -= 1
    if i > 0 and text[i - 1] == '\n':
        i -= 1
    return text[:i] + text[j:]


def field(text, pattern, code, count=1):
    """The first `count` captures of pattern's group 1 replaced by the marker."""
    n = 0
    def rep(m):
        nonlocal n
        n += 1
        if n > count:
            return m.group(0)
        return m.group(0)[:m.start(1) - m.start(0)] + MARK + code + m.group(0)[m.end(1) - m.start(0):]
    out = re.sub(pattern, rep, text)
    if n < count:
        raise ValueError(pattern)
    return out


def main(src, dst):
    xml = gzip.open(src, 'rb').read().decode('utf-8')
    root = ET.fromstring(xml)
    ls = root.find('LiveSet')
    track0 = ls.find('Tracks')[0]

    # The ids the clip envelopes point at (modulation targets): the mixer's
    # volume and pan, the two Auto Filters' cutoffs (lowpass first).
    mixer = track0.find('DeviceChain/Mixer')
    want = {'VOLUME': mixer.find('Volume/ModulationTarget').get('Id'),
            'PAN': mixer.find('Pan/ModulationTarget').get('Id')}
    filters = [d for d in track0.find('DeviceChain/DeviceChain/Devices') if d.tag == 'AutoFilter']
    lp = [d for d in filters if d.find('FilterType/Manual').get('Value') == '0'][0]
    hp = [d for d in filters if d.find('FilterType/Manual').get('Value') == '1'][0]
    want['LP_CUTOFF'] = lp.find('Cutoff/ModulationTarget').get('Id')
    want['HP_CUTOFF'] = hp.find('Cutoff/ModulationTarget').get('Id')

    # ---- The track
    i, j = element(xml, '<AudioTrack Id="26">')
    track = xml[i:j]
    # Devices other than the Auto Filters out (the template's Utility).
    k = track.index('<Devices>') + len('<Devices>')
    while True:
        m = re.compile(r'<(\w+) Id="\d+">').search(track, k)
        if not m or m.start() > track.index('</Devices>'):
            break
        a, b = element(track, m.group(0), m.start())
        if m.group(1) == 'AutoFilter':
            k = b
        else:
            track = cut(track, m.group(0), m.start())
    track = field(track, r'<AudioTrack Id="(\d+)"', 'I')
    track = field(track, r'<EffectiveName Value="([^"]*)"', 'n')
    track = field(track, r'<Color Value="([^"]*)"', 'c')
    track = re.sub(r'<MemorizedFirstClipName Value="[^"]*"', '<MemorizedFirstClipName Value=""', track)
    track = field(track, r'<SavedPlayingSlot Value="([^"]*)"', 'S')
    # The clip slots (the main sequencer's, then the freeze sequencer's).
    for code in 'MF':
        k = track.index('<ClipSlotList>')
        e = track.index('</ClipSlotList>', k)
        track = track[:k] + '<ClipSlotList>\n' + MARK + code + track[e:]
        track = track.replace('<ClipSlotList>', '<ClipSlotLis\002>', 1)   # (the next one)
    track = track.replace('<ClipSlotLis\002>', '<ClipSlotList>')
    # Global ids: fresh ones, in order; the ones the envelopes need, by place.
    places = {}
    count = 0
    def fresh(m):
        nonlocal count
        for name, old in want.items():
            if m.group(2) == old:
                places[name] = count
        count += 1
        return '<' + m.group(1) + ' Id="' + MARK + 'i"'
    track = re.sub(r'<(\w*(?:Target|Pointee)) Id="(\d+)"', fresh, track)
    assert len(places) == 4, places

    # ---- Clip slots
    i, j = element(xml, '<ClipSlotList>', xml.index('<AudioTrack Id="26">'))
    slot_text = xml[i:j]
    a, b = element(slot_text, '<ClipSlot Id="0">')
    clip = slot_text[a:b]
    tracks_end = xml.index('</Tracks>')
    last = xml.rindex('<AudioTrack Id=', 0, tracks_end)
    a, b = element(xml, '<ClipSlot Id="0">', last)
    empty = xml[a:b]
    empty = field(empty, r'<ClipSlot Id="(\d+)"', 'k')

    clip = field(clip, r'<ClipSlot Id="(\d+)"', 'k')
    clip = field(clip, r'<AudioClip Id="(\d+)"', 'k')
    clip = field(clip, r'<Color Value="([^"]*)"', 'c')
    for tag, code in [('CurrentStart', 'b'), ('CurrentEnd', 'e'), ('LoopStart', 's'), ('LoopEnd', 'l'),
                      ('StartRelative', 'r'), ('OutMarker', 'o'), ('HiddenLoopStart', 's'), ('HiddenLoopEnd', 'l'),
                      ('Name', 'm'), ('Disabled', 'd'), ('RelativePath', 'p'), ('Path', 'P'),
                      ('OriginalFileSize', 'z'), ('DefaultDuration', 'u'), ('OtherTime', 'e')]:
        clip = field(clip, r'<' + tag + r' Value="([^"]*)"', code)
    clip = re.sub(r'<OriginalCrc Value="[^"]*"', '<OriginalCrc Value="0"', clip)
    clip = re.sub(r'<DefaultSampleRate Value="[^"]*"', '<DefaultSampleRate Value="44100"', clip)
    clip = re.sub(r'<RelativePathType Value="[^"]*"', '<RelativePathType Value="3"', clip)
    clip = field(clip, r'<WarpMarker Id="1" SecTime="([^"]*)"', 'w')
    clip = field(clip, r'<WarpMarker Id="1" SecTime="[^"]*" BeatTime="([^"]*)"', 'W')
    inner = re.search(r'<Envelopes>\s*(<Envelopes />|<Envelopes>.*?</Envelopes>)\s*</Envelopes>', clip, re.S)
    clip = clip[:inner.start(1)] + MARK + 'E' + clip[inner.end(1):]

    # ---- Scenes
    i, j = element(xml, '<Scenes>')
    scenes = xml[i:j]
    a, b = element(scenes, '<Scene Id="0">')
    scene = scenes[a:b]
    scene = field(scene, r'<Scene Id="(\d+)"', 'k')
    scene = field(scene, r'<Name Value="([^"]*)"', 'm')

    # ---- The set
    s = xml[:xml.index('<Tracks>') + len('<Tracks>')] + '\n' + MARK + 'R' + xml[tracks_end:]
    i, j = element(s, '<Scenes>')
    s = s[:i] + '<Scenes>\n' + MARK + 'C</Scenes>' + s[j:]
    s = field(s, r'<NextPointeeId Value="(\d+)"', 'N')
    m = re.search(r'<Tempo>\s*<LomId Value="0" />\s*<Manual Value="([^"]*)"', s)
    s = s[:m.start(1)] + MARK + 'T' + s[m.end(1):]
    tempo_target = ls.find('MasterTrack/DeviceChain/Mixer/Tempo/AutomationTarget').get('Id')
    m = re.search(r'<PointeeId Value="' + tempo_target + r'" />\s*</EnvelopeTarget>\s*<Automation>\s*<Events>\s*<FloatEvent [^>]*Value="([^"]*)"', s)
    s = s[:m.start(1)] + MARK + 'T' + s[m.end(1):]
    others = [int(v) for v in re.findall(r'<\w*(?:Target|Pointee) Id="(\d+)"', s)]
    assert max(others) < ID_BASE, max(others)

    def lit(name, text):
        text = '\n'.join(l.lstrip('\t') for l in text.split('\n'))   # (no indents: smaller)
        out = ['PROGMEM static const char ' + name + '[] =']
        for line in text.split('\n'):
            esc = line.replace('\\', '\\\\').replace('"', '\\"').replace(MARK, '\\001')
            out.append('    "' + esc + '\\n"')
        out[-1] += ';'
        return '\n'.join(out) + '\n'

    with open(dst, 'w') as f:
        f.write('// ableton_template.h — made by tools/ableton/make_template.py from a Live Set\n'
                '// saved by ' + root.get('Creator') + ' (see there for the fields). Don\'t edit.\n'
                '#pragma once\n\n')
        f.write('static constexpr int ALS_ID_BASE = %d, ALS_TRACK_IDS = %d;\n' % (ID_BASE, count))
        f.write('// Each track\'s ids: ALS_ID_BASE + its place (0-6) * ALS_TRACK_IDS + these.\n')
        for name in ['VOLUME', 'PAN', 'LP_CUTOFF', 'HP_CUTOFF']:
            f.write('static constexpr int ALS_%s_MOD = %d;\n' % (name, places[name]))
        f.write('\n')
        f.write(lit('ALS_SET', s))
        f.write(lit('ALS_TRACK', track))
        f.write(lit('ALS_SLOT', empty))
        f.write(lit('ALS_CLIP_SLOT', clip))
        f.write(lit('ALS_SCENE', scene))
    print('track ids', count, places, 'bytes', len(s) + len(track) + len(empty) + len(clip) + len(scene))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
