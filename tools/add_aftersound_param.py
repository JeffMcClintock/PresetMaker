"""One-off: add the Aftersound knob/parameter to FretPluckedOsc.synthedit by cloning the Decay Tilt one.
usage: python add_aftersound_param.py <project.synthedit>   (edits in place; caller keeps the backup)"""
import random
import re
import sys

OSC = '1590000411'            # SE Fret Plucked Osc instance
SRC_CONTAINER = '2132223300'  # the Decay Tilt knob container
SRC_PATCHMEM = '1387860514'   # its PatchMemGet, named in the <param>
NEW_PIN_1BASED = 24           # Aftersound is pin 23 of the oscillator
VST_ID = '19'


def main(path):
    raw = open(path, 'rb').read()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'
    lines = raw.decode('utf-8').replace('\r\n', '\n').split('\n')
    text = '\n'.join(lines)
    used = set(re.findall(r'handle="(\d+)"', text))

    def fresh():
        while True:
            h = str(random.randint(1, 2**31 - 1))
            if h not in used:
                used.add(h)
                return h

    # locate the container block
    start = next(i for i, l in enumerate(lines) if f'handle="{SRC_CONTAINER}"' in l and 'name="Decay Tilt"' in l)
    indent = lines[start][:len(lines[start]) - len(lines[start].lstrip())]
    end = next(i for i in range(start + 1, len(lines)) if lines[i] == indent + '</module>')
    block = lines[start:end + 1]

    handles = sorted(set(re.findall(r'handle="(\d+)"', '\n'.join(block))))
    remap = {h: fresh() for h in handles}

    def rehandle(s):
        return re.sub(r'\b(handle|fMod|tMod|tiedtomod|module)="(\d+)"',
                      lambda m: f'{m.group(1)}="{remap.get(m.group(2), m.group(2))}"', s)

    new_block = [rehandle(l) for l in block]
    new_block[0] = new_block[0].replace('name="Decay Tilt"', 'name="Aftersound"')
    new_block = [l.replace('<plug default="Decay Tilt"/>', '<plug default="Aftersound"/>') for l in new_block]
    # move the knob so it doesn't sit on top of the original
    for i, l in enumerate(new_block[:12]):
        m = re.match(r'(\s*<structRect l=")(\d+)(" r=")(\d+)(".*)', l)
        if m:
            new_block[i] = f'{m.group(1)}{int(m.group(2)) + 130}{m.group(3)}{int(m.group(4)) + 130}{m.group(5)}'
            break
    assert 'Aftersound' in new_block[0] and any('default="Aftersound"' in l for l in new_block)

    # the <param> for the PatchMemGet
    pstart = next(i for i, l in enumerate(lines) if f'module="{SRC_PATCHMEM}"' in l and '<param ' in l)
    pend = next(i for i in range(pstart, len(lines)) if lines[i].strip() == '</param>')
    head = lines[pstart]
    head = re.sub(r'handle="\d+"', f'handle="{fresh()}"', head, count=1)
    head = head.replace('shortName="Decay Tilt"', 'shortName="Aftersound"')
    head = re.sub(r'vstParameterNumber="\d+"', f'vstParameterNumber="{VST_ID}"', head)
    head = head.replace(f'module="{SRC_PATCHMEM}"', f'module="{remap[SRC_PATCHMEM]}"')
    head = re.sub(r'RangeMaximum="[^"]*"', 'RangeMaximum="1"', head)
    pind = lines[pstart][:len(lines[pstart]) - len(lines[pstart].lstrip())]
    new_param = [head, pind + '    <patch-list>', pind + '        <s repeat="128">0</s>', pind + '    </patch-list>', pind + '</param>']

    # wire the new container to the oscillator's new pin
    lstart = next(i for i, l in enumerate(lines) if f'fMod="{SRC_CONTAINER}" tMod="{OSC}"' in l)
    lind = lines[lstart][:len(lines[lstart]) - len(lines[lstart].lstrip())]
    new_line = f'{lind}<line type="Line" handle="{fresh()}" lineType="1" fMod="{remap[SRC_CONTAINER]}" tMod="{OSC}" fPlg="7" tPlg="{NEW_PIN_1BASED}"/>'

    # the cached module description
    cstart = next(i for i, l in enumerate(lines) if '<Plugin id="SE Fret Plucked Osc"' in l)
    cpin = next(i for i in range(cstart, len(lines)) if '<Pin name="Envelope Data"' in lines[i] and 'direction="out"' in lines[i])
    cind = lines[cpin][:len(lines[cpin]) - len(lines[cpin].lstrip())]
    new_pin = f'{cind}<Pin name="Aftersound" datatype="float"/>'

    # splice bottom-up so earlier indices stay valid
    for pos, ins in sorted([(cpin + 1, [new_pin]), (lstart + 1, [new_line]), (end + 1, new_block), (pend + 1, new_param)], reverse=True):
        lines[pos:pos] = ins

    open(path, 'wb').write(nl.join(l.encode('utf-8') for l in lines))
    print(f'container {SRC_CONTAINER} -> {remap[SRC_CONTAINER]}, {len(remap)} handles cloned, param id {VST_ID}, pin {NEW_PIN_1BASED}')


main(sys.argv[1])
