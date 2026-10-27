#!/usr/bin/env python3
"""Translation files helper.

  python3 tools/lang.py en          rewrite lang/en.txt from src/strings.h
  python3 tools/lang.py status      list missing / extra keys of every lang/*.txt
  python3 tools/lang.py sync        put every lang/*.txt in the order and sections of
                                    en.txt; missing texts are added as "# key = English"

src/strings.h is the source of the English texts; the unit tests (./build.sh test)
check every lang/*.txt against it.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECTIONS = {'num': 'Numbers', 'msg': 'Messages (shown for a few seconds)',
            'bar': 'Info bar at the bottom of the screen (short words)',
            'hud': "Rows in the game's building box, bottom right (narrow: keep them short)",
            'float': 'Floating text under the crosshair', 'key': 'Key names',
            'menu': 'Building Tools menu', 'option': 'Options tab'}


NOTES = {
    'num.decimal': 'decimal mark in distances like 4.0 m',
    'menu.translation_note': 'shown at the bottom of the menu while a translation is unreviewed; empty = not shown',
    'bar.size': 'area size: width x depth, h = height, = number of shapes',
    'msg.placed': 'numbers can be 1 or 2000: write it so any number reads well (e.g. "Shapes placed: {0}")',
}


def strings():
    src = open(os.path.join(ROOT, 'src', 'strings.h'), encoding='utf-8').read()
    return re.findall(r'X\((S_\w+), "([^"]+)", "((?:[^"\\]|\\.)*)"\)', src)


def write_en():
    out = ['# Enshrouded Building Tools: English texts.',
           '# English is built into the mod; this file is the template for translations.',
           '# Copy it to <code>.txt (for example pl.txt), translate the text after "=",',
           '# keep {0} {1} ... (the mod fills in numbers and key names) and keep the keys.',
           '# How to test and send a translation: see TRANSLATING.md in the GitHub repository',
           '# https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools',
           '']
    last = None
    for _, key, en in strings():
        sec = key.split('.')[0]
        if sec != last:
            if last: out.append('')
            out.append('# ' + SECTIONS.get(sec, sec))
            last = sec
        if key in NOTES: out.append('# ' + NOTES[key])
        out.append(f'{key} = {en}')
    open(os.path.join(ROOT, 'lang', 'en.txt'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out) + '\n')


def read(path):
    head, texts, body = [], {}, False
    for line in open(path, encoding='utf-8-sig'):
        line = line.rstrip('\r\n')
        t = line.strip()
        if t and t[0] not in '#;' and '=' in t:
            k, v = t.split('=', 1)
            texts[k.strip()] = v.strip()
            body = True
        elif not body and t.startswith('#'):
            head.append(line)
    return head, texts


def sync():
    for f in sorted(os.listdir(os.path.join(ROOT, 'lang'))):
        if f == 'en.txt' or not f.endswith('.txt'):
            continue
        path = os.path.join(ROOT, 'lang', f)
        head, texts = read(path)
        out, last = head + [''], None
        for _, key, en in strings():
            sec = key.split('.')[0]
            if sec != last:
                if last: out.append('')
                out.append('# ' + SECTIONS.get(sec, sec))
                last = sec
            out.append(f'{key} = {texts[key]}' if key in texts else f'# {key} = {en}')
        open(path, 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out) + '\n')


def status():
    keys = [k for _, k, _ in strings()]
    for f in sorted(os.listdir(os.path.join(ROOT, 'lang'))):
        have = {}
        for line in open(os.path.join(ROOT, 'lang', f), encoding='utf-8-sig'):
            line = line.strip()
            if line and line[0] not in '#;' and '=' in line:
                k, v = line.split('=', 1)
                have[k.strip()] = v.strip()
        missing = [k for k in keys if k not in have]
        extra = [k for k in have if k not in keys]
        print(f'{f:10} {len(have):4} texts, missing {len(missing)}, unknown {len(extra)}')
        for k in missing: print('   missing', k)
        for k in extra: print('   unknown', k)


if __name__ == '__main__':
    {'en': write_en, 'status': status, 'sync': sync}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: print(__doc__))()
