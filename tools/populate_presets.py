"""Write fitted PresetMaker presets into the preset slots of a .synthedit project.

usage: python populate_presets.py <in.synthedit> <out.synthedit> <presets.json>

presets.json: [{"slot": 1, "name": "Nylon Guitar", "category": "Guitar", "preset": "path\\to\\preset.txt"}, ...]
preset.txt lines are "<vst param id>\t<normalised 0..1>\t<title>", as written by `pmcli fit`.

Project values are stored in native units, linear between RangeMinimum (default 0) and RangeMaximum
(default 10); enum parameters (type 0) store the item index. Slots not listed keep their old values.
"""
import json
import re
import sys
from xml.sax.saxutils import escape

SLOTS = 128


def expand(patch_list_body):
    vals = []
    for m in re.finditer(r'<s(?: repeat="(\d+)")?>(.*?)</s>|<s(?: repeat="(\d+)")?/>', patch_list_body, re.S):
        n = int(m.group(1) or m.group(3) or 1)
        vals += [m.group(2) or ''] * n
    return (vals + [vals[-1] if vals else ''] * SLOTS)[:SLOTS]


def encode(vals, indent):
    out, i = [], 0
    while i < len(vals):
        j = i
        while j < len(vals) and vals[j] == vals[i]:
            j += 1
        rep = f' repeat="{j - i}"' if j - i > 1 else ''
        out.append(f'{indent}<s{rep}>{vals[i]}</s>')
        i = j
    return out


def native(attrs, norm):
    if attrs.get('type') == '0':  # enum: index
        items = attrs.get('MetaData', '').split(',')
        return str(int(round(norm * (len(items) - 1))))
    lo = float(attrs.get('RangeMinimum', 0))
    hi = float(attrs.get('RangeMaximum', 10))
    return f'{lo + norm * (hi - lo):.6g}'


def main(src, dst, presets_json):
    raw = open(src, 'rb').read()
    text = raw.decode('utf-8')
    nl = '\r\n' if '\r\n' in text else '\n'
    text = text.replace('\r\n', '\n')

    presets = json.load(open(presets_json))
    loaded = []
    for p in presets:
        norm = {}
        for line in open(p['preset']):
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2:
                norm[int(parts[0])] = float(parts[1])
        loaded.append((p, norm))

    changed = 0

    def patch_param(m):
        nonlocal changed
        block = m.group(0)
        head = re.match(r'<param\b([^>]*)>', block).group(1)
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', head))
        pl = re.search(r'(\n(\s*)<patch-list>\n)(.*?)(\n\s*</patch-list>)', block, re.S)
        if not pl:
            return block
        vals = expand(pl.group(3))
        if attrs.get('shortName') == 'ProgramCategory':
            for p, _ in loaded:
                vals[p['slot']] = escape(p.get('category', ''))
        elif 'vstParameterNumber' in attrs:
            vid = int(attrs['vstParameterNumber'])
            for p, norm in loaded:
                if vid in norm:
                    vals[p['slot']] = native(attrs, norm[vid])
        else:
            return block
        indent = pl.group(2) + '    '
        new = pl.group(1) + '\n'.join(encode(vals, indent)) + pl.group(4)
        changed += 1
        return block[:pl.start()] + new + block[pl.end():]

    text = re.sub(r'<param\b[^>]*>.*?</param>', patch_param, text, flags=re.S)

    # The names live in the exported container's <ProgramNames>, which is the one with entries when
    # the project already has presets (an outer container carries an empty one). Merge: listed slots
    # take the new name, every other slot keeps its own.
    blocks = list(re.finditer(r'<ProgramNames\s*/>|<ProgramNames>.*?</ProgramNames>', text, flags=re.S))
    assert blocks, 'no <ProgramNames> element'
    target = next((b for b in blocks if '<Name ' in b.group(0)), blocks[0])
    names = dict(re.findall(r'<Name idx="(\d+)" text="([^"]*)"', target.group(0)))
    for p, _ in loaded:
        names[str(p['slot'])] = escape(p['name'])
    indent = text[text.rfind('\n', 0, target.start()) + 1:target.start()]
    body = ''.join(f'\n{indent}    <Name idx="{k}" text="{v}"/>' for k, v in sorted(names.items(), key=lambda kv: int(kv[0])))
    text = text[:target.start()] + f'<ProgramNames>{body}\n{indent}</ProgramNames>' + text[target.end():]

    open(dst, 'wb').write(text.replace('\n', nl).encode('utf-8'))
    print(f'{len(loaded)} presets written, {changed} parameter lists rewritten -> {dst}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
