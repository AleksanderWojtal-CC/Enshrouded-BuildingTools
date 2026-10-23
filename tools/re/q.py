"""Query helpers: string -> code refs / data pointer refs."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
import sys, struct, pickle
sys.path.insert(0, HERE)
import pe as pem

p = pem.load()
refs, calls = pickle.load(open(os.path.join(HERE, 'xidx.pkl'), 'rb'))

def code_refs(rva):
    return [a for a, _ in refs.get(rva, [])]

def ptr_refs(rva):
    return p.find_bytes(struct.pack('<Q', p.base + rva))

def fn(a):
    f = p.func_of(a)
    return f[0] if f else None

def callers(f):
    return [a for a, c in calls.get(f, [])]

def strinfo(s, show=True):
    out = []
    for r in p.find_cstring(s):
        cr = code_refs(r)
        pr = ptr_refs(r)
        out.append((r, cr, pr))
        if show:
            print(f'{s!r} @{r:x}  code:{[hex(x) for x in cr[:8]]}  funcs:{[hex(fn(x) or 0) for x in cr[:8]]}  dataptrs:{[hex(x) for x in pr[:6]]}')
    return out

def hexdump(rva, n=0x80):
    b = p.read(rva, n)
    for i in range(0, n, 16):
        row = b[i:i + 16]
        print(f'{rva + i:08x}: ' + ' '.join(f'{c:02x}' for c in row) + '  ' + ''.join(chr(c) if 32 <= c < 127 else '.' for c in row))

def qwords(rva, n=8):
    out = []
    for i in range(n):
        v = struct.unpack('<Q', p.read(rva + 8 * i, 8))[0]
        ann = ''
        if p.base <= v < p.base + 0x3000000:
            ann = p.annotate(v - p.base) or f'rva {v - p.base:x}'
        out.append((rva + 8 * i, v, ann))
        print(f'{rva + 8 * i:08x}: {v:016x} {ann}')
    return out
