"""Call-graph helpers that follow chained code (disassemble until ret / int3 / tail-jmp out)."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
import sys
sys.path.insert(0, HERE)
from q import *

_cache = {}

def body(start, limit=0x6000):
    """Linear-ish disassembly following forward jumps inside a region; stops at ret
    when no pending in-range jump targets remain."""
    if start in _cache:
        return _cache[start]
    seen = {}
    work = [start]
    while work:
        a = work.pop()
        while a not in seen and start <= a < start + limit:
            b = p.read(a, 16)
            ins = next(p.md.disasm(b, a), None)
            if ins is None:
                break
            seen[a] = ins
            m = ins.mnemonic
            t = p.target(ins) if (m.startswith('j')) else None
            if m == 'ret' or m == 'int3':
                break
            if m == 'jmp':
                if t is not None and start <= t < start + limit and t not in p._fstartset():
                    a = t
                    continue
                break  # tail call or indirect
            if m.startswith('j') and t is not None and start <= t < start + limit:
                work.append(t)
            a += ins.size
    res = [seen[k] for k in sorted(seen)]
    _cache[start] = res
    return res

def callees(start):
    out = []
    for i in body(start):
        if i.mnemonic == 'call' or (i.mnemonic == 'jmp' and p.target(i) in p._fstartset()):
            t = p.target(i)
            if t is not None:
                out.append((i.address, t))
    return out

def tree(start, depth=2, ind=0, seen=None):
    seen = seen if seen is not None else set()
    for a, t in callees(start):
        nc = len(callers(t))
        tag = '' if t not in seen else ' (seen)'
        print('  ' * ind + f'{a:x} -> {t:x} [callers {nc}]{tag}')
        if t not in seen and depth > 1 and nc < 400:
            seen.add(t)
            tree(t, depth - 1, ind + 1, seen)
