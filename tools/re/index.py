"""Full xref index: disassemble every .pdata function with capstone (lite mode)
and record rip-relative memory targets, call targets and jump targets."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
import sys, os, re, pickle, multiprocessing as mp
sys.path.insert(0, HERE)
import pe as pem

RIP = re.compile(r'rip ([+-]) 0x([0-9a-f]+)')
P = None

def init():
    global P
    P = pem.load()

def work(chunk):
    import capstone as cs
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
    md.skipdata = True
    refs, calls = [], []
    for b, e in chunk:
        code = P.read(b, e - b)
        for addr, size, mn, ops in md.disasm_lite(code, b):
            if mn == 'call' or mn == 'jmp':
                if ops.startswith('0x'):
                    calls.append((int(ops, 16), addr, mn == 'call'))
                    continue
            m = RIP.search(ops)
            if m:
                d = int(m.group(2), 16)
                t = addr + size + (d if m.group(1) == '+' else -d)
                refs.append((t, addr, mn))
    return refs, calls

if __name__ == '__main__':
    p = pem.load()
    # linear sweep over whole .text in 256 KiB chunks (split at pdata starts)
    n, va, vs, ro, rs = p.sec('.text')
    import bisect
    cuts = [va]
    step = 1 << 18
    x = va + step
    while x < va + vs:
        i = bisect.bisect_left(p.fstarts, x)
        x = p.fstarts[i] if i < len(p.fstarts) else va + vs
        cuts.append(x); x += step
    cuts.append(va + vs)
    chunks = [[(cuts[i], cuts[i+1])] for i in range(len(cuts)-1) if cuts[i] < cuts[i+1]]
    refs, calls = {}, {}
    with mp.Pool(2, initializer=init) as pool:
        for r, c in pool.imap_unordered(work, chunks):
            for t, a, mn in r:
                refs.setdefault(t, []).append((a, mn))
            for t, a, iscall in c:
                calls.setdefault(t, []).append((a, iscall))
    pickle.dump((refs, calls), open(os.path.join(HERE, 'xidx.pkl'), 'wb'))
    print('refs targets', len(refs), 'total', sum(map(len, refs.values())))
    print('call targets', len(calls))
