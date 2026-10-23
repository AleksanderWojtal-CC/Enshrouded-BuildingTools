"""Dump keen reflection type descriptors.

Descriptor layout observed (64-bit):
 +00 name*      +08 nameLen
 +10 scoped*    +18 scopedLen      (e.g. actor.ChangeVoxel)
 +20 full*      +28 fullLen        (keen::actor::ChangeVoxel)
 +30 ptr        +38 ptr            (kind vtables?)
 +40 u32 size   u16 align u16 ?
 +48 u32 count  u32 kind
 +50 u64 hash
 +58 members*
Members (stride 0x30): name*, nameLen, type*, offset, ?, ?
Enum members unknown yet.
"""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
import sys, struct, json, pickle
sys.path.insert(0, HERE)
import pe as pem

p = pem.load()
B = p.base

def q(rva):
    return struct.unpack('<Q', p.read(rva, 8))[0]

def sptr(v, n):
    if not (B <= v < B + 0x3000000) or not (0 < n < 300):
        return None
    try:
        b = p.read(v - B, n + 1)
    except Exception:
        return None
    if len(b) != n + 1 or b[n] != 0:
        return None
    if not all(32 <= c < 127 for c in b[:n]):
        return None
    return b[:n].decode()

def scan():
    types = {}
    for sname in ('.rdata', '.data'):
        n, va, vs, ro, rs = p.sec(sname)
        d = p.d[ro:ro + rs]
        for off in range(0, len(d) - 0x60, 8):
            a0, l0, a1, l1, a2, l2 = struct.unpack_from('<6Q', d, off)
            if not (8 < l2 < 300 and B <= a2 < B + 0x3000000):
                continue
            full = sptr(a2, l2)
            if not full or not full.startswith('keen::'):
                continue
            name = sptr(a0, l0)
            if not name:
                continue
            size, align, x, cnt, kind = struct.unpack_from('<IHHII', d, off + 0x40)
            h, mem = struct.unpack_from('<QQ', d, off + 0x50)
            if (kind & 0xff) in (0x0c, 0x0d) or (kind & 0xff) == 0x11 and False:
                mem = struct.unpack_from('<Q', d, off + 0x60)[0]
            rva = va + off
            types[rva] = dict(rva=rva, name=name, full=full, size=size, align=align,
                              count=cnt, kind=kind, hash=h, members=mem - B if mem else 0)
    return types

def enum_members(t):
    out = []
    for i in range(min(t['count'], 400)):
        r = t['members'] + 0x28 * i
        try:
            na, nl, val = struct.unpack('<3Q', p.read(r, 24))
        except Exception:
            break
        nm = sptr(na, nl)
        if nm is None:
            break
        out.append((val, nm, ''))
    return out

def members(t, bytype):
    if (t['kind'] & 0xff) in (0x0c, 0x0d):
        return enum_members(t) if t['members'] else []
    out = []
    if not t['members'] or t['count'] > 400:
        return out
    for i in range(t['count']):
        r = t['members'] + 0x30 * i
        try:
            na, nl, ty, offs = struct.unpack('<4Q', p.read(r, 32))
        except Exception:
            break
        nm = sptr(na, nl)
        if nm is None:
            return None
        tn = bytype.get(ty - B, {}).get('full', f'?{ty - B:x}') if ty else ''
        out.append((offs, nm, tn))
    return out

if __name__ == '__main__':
    types = scan()
    print('types', len(types))
    for t in types.values():
        m = members(t, types)
        t['fields'] = m
    pickle.dump(types, open(os.path.join(HERE, 'types.pkl'), 'wb'))
    with open(os.path.join(HERE, 'types.txt'), 'w') as f:
        for t in sorted(types.values(), key=lambda t: t['full']):
            f.write(f"{t['full']}  size=0x{t['size']:x} kind=0x{t['kind']:x} n={t['count']} @{t['rva']:x}\n")
            if t['fields']:
                for o, nm, tn in t['fields']:
                    f.write(f"    +0x{o:x} {nm}: {tn}\n" if tn or (t['kind']&0xff) not in (0xc,0xd) else f"    = {o} {nm}\n")
