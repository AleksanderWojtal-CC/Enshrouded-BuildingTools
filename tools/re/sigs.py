"""Generate unique wildcard signatures for the mod's targets and verify them.

A signature wildcards every rel32 branch target and rip-relative displacement, so it
survives small layout shifts between game builds."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
import sys, re
sys.path.insert(0, HERE)
from cg import *
import capstone as cs

TEXT = p.sec('.text')
tn, tva, tvs, tro, trs = TEXT
CODE = p.d[tro:tro + trs]

def ins_mask(i):
    """Return list of (byte or None) for one instruction."""
    b = list(i.bytes)
    out = b[:]
    # rel32 branches
    if (i.mnemonic == 'call' or i.mnemonic.startswith('j')) and i.operands and i.operands[0].type == cs.x86.X86_OP_IMM:
        if len(b) >= 5:
            for k in range(len(b) - 4, len(b)):
                out[k] = None
    # rip-relative displacement
    if i.disp_size == 4 and any(op.type == cs.x86.X86_OP_MEM and op.mem.base == cs.x86.X86_REG_RIP for op in i.operands):
        o = i.disp_offset
        for k in range(o, o + 4):
            out[k] = None
    return out

def to_regex(mask):
    return re.compile(b''.join(b'.' if x is None else re.escape(bytes([x])) for x in mask), re.S)

def count(mask, limit=3):
    rx = to_regex(mask)
    n = 0
    for _ in rx.finditer(CODE):
        n += 1
        if n >= limit:
            break
    return n

def make(start, minlen=24, maxlen=96):
    md = p.md
    mask = []
    a = start
    while len(mask) < maxlen:
        i = next(md.disasm(p.read(a, 16), a))
        mask += ins_mask(i)
        a += i.size
        if len(mask) >= minlen and count(mask) == 1:
            return mask
    raise RuntimeError(f'no unique sig at {start:x}')

def fmt(mask):
    return ' '.join('??' if x is None else f'{x:02X}' for x in mask)

def find(mask):
    rx = to_regex(mask)
    return [tva + m.start() for m in rx.finditer(CODE)]

TARGETS = {
    # name: (rva of pattern start, what the mod derives from it)
    'PlaceShape':  0x48a660,   # hooked
    'RemoveShape': 0x48b180,   # called
    'ShapeBounds': 0x48e8a0,   # called
    # mov rbx,[rip+X] -> removal material mask (dismantle default filter)
    'RemoveMask':  0x2d43f7,
    # caller layout check: cursor component is [rbp-0x58], transform copy at rbp+0xc0
    'CursorCall':  0x2d33fb,
    'PlaceCall':   0x2d3743,
}

if __name__ == '__main__':
    out = {}
    for name, rva in TARGETS.items():
        m = make(rva)
        hits = find(m)
        assert hits == [rva], (name, [hex(h) for h in hits])
        out[name] = m
        print(f'{name:12} @{rva:x} len={len(m)} unique  {fmt(m)}')
    import json
    json.dump({k: fmt(v) for k, v in out.items()}, open(os.path.join(HERE, 'sigs.json'), 'w'), indent=1)
