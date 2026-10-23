"""Minimal PE x64 analysis helpers for enshrouded.exe."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
import struct, bisect, re, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'pylib'))
import capstone as cs

class PE:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        ns = struct.unpack_from('<H', d, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<Q', d, opt + 24)[0]
        sh = opt + struct.unpack_from('<H', d, pe + 20)[0]
        self.sections = []
        for i in range(ns):
            n, vs, va, rs, ro = struct.unpack_from('<8sIIII', d, sh + 40 * i)
            self.sections.append((n.rstrip(b'\0').decode(), va, vs, ro, rs))
        # data directories
        dd = opt + 112
        self.exc_rva, self.exc_size = struct.unpack_from('<II', d, dd + 8 * 3)
        self.md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
        self.md.detail = True
        self._load_funcs()

    def sec(self, name):
        return next(s for s in self.sections if s[0] == name)

    def rva2off(self, rva):
        for n, va, vs, ro, rs in self.sections:
            if va <= rva < va + max(vs, rs):
                if rva - va >= rs:
                    return None
                return ro + rva - va
        return None

    def off2rva(self, off):
        for n, va, vs, ro, rs in self.sections:
            if ro <= off < ro + rs:
                return va + off - ro
        return None

    def read(self, rva, n):
        o = self.rva2off(rva)
        return self.d[o:o + n]

    def cstr(self, rva):
        o = self.rva2off(rva)
        e = self.d.index(b'\0', o)
        return self.d[o:e].decode('latin1')

    def _load_funcs(self):
        o = self.rva2off(self.exc_rva)
        f = []
        for i in range(self.exc_size // 12):
            b, e, u = struct.unpack_from('<III', self.d, o + 12 * i)
            if b:
                f.append((b, e))
        f.sort()
        self.funcs = f
        self.fstarts = [x[0] for x in f]

    def func_of(self, rva):
        i = bisect.bisect_right(self.fstarts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def find_bytes(self, pat, section=None):
        """All RVAs where bytes occur."""
        out = []
        for m in re.finditer(re.escape(pat), self.d):
            r = self.off2rva(m.start())
            if r is not None:
                out.append(r)
        return out

    def find_cstring(self, s):
        """RVAs of NUL-terminated exact string s (preceded by NUL)."""
        b = s.encode() + b'\0'
        return [r for r in self.find_bytes(b) if self.d[self.rva2off(r) - 1] == 0]

    def build_riprefs(self):
        """Index of every rip-relative disp32 target in .text for common
        opcodes (lea/mov/cmp...). Approximate, byte-pattern based: for each
        ModRM with mod=00 rm=101 we compute target = next_ip + disp."""
        if hasattr(self, 'refs'):
            return
        n, va, vs, ro, rs = self.sec('.text')
        code = self.d[ro:ro + rs]
        refs = {}
        # REX.W? 8D (lea) / 8B (mov load) / 89 (mov store) / C7 / 3B / 39 etc. ModRM 00xxx101
        rx = re.compile(rb'[\x40-\x4f]?[\x8d\x8b\x89\x3b\x39\x80\x81\x83\xc7\xc6\x88\x8a\x63\xff\x85]([\x05\x0d\x15\x1d\x25\x2d\x35\x3d])', re.S)
        rx2 = re.compile(rb'\x0f[\x10\x11\x28\x29\xb6\xb7\x2e\x2f\x58\x59\x5c\x5e\xbf\xbe]([\x05\x0d\x15\x1d\x25\x2d\x35\x3d])', re.S)
        for r in (rx, rx2):
            for m in r.finditer(code):
                dpos = m.end()
                disp = struct.unpack_from('<i', code, dpos)[0]
                op = code[m.start():m.end() - 1]
                extra = 0
                if op[-1:] in (b'\x80', b'\x83', b'\xc6'):
                    extra = 1
                elif op[-1:] in (b'\x81', b'\xc7'):
                    extra = 4
                nxt = va + dpos + 4 + extra
                refs.setdefault(nxt + disp, []).append(va + m.start())
        # call rel32 / jmp rel32
        self.refs = refs
        calls = {}
        for m in re.finditer(rb'[\xe8\xe9]', code):
            p = m.start()
            if p + 5 > len(code):
                continue
            t = va + p + 5 + struct.unpack_from('<i', code, p + 1)[0]
            if t in self._fstartset():
                calls.setdefault(t, []).append(va + p)
        self.calls = calls

    def _fstartset(self):
        if not hasattr(self, '_fss'):
            self._fss = set(self.fstarts)
        return self._fss

    def xrefs(self, target):
        self.build_riprefs()
        return self.refs.get(target, [])

    def callers(self, target):
        self.build_riprefs()
        return self.calls.get(target, [])

    def disasm(self, rva, n=None, end=None):
        f = self.func_of(rva)
        if end is None:
            end = f[1] if f else rva + (n or 0x200)
        code = self.read(rva, end - rva)
        return list(self.md.disasm(code, rva))

    def fmt(self, ins, ann=True):
        s = f'{ins.address:08x}: {ins.mnemonic:7} {ins.op_str}'
        if ann:
            t = self.target(ins)
            if t is not None:
                a = self.annotate(t)
                if a:
                    s += '   ; ' + a
        return s

    def target(self, ins):
        if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
            if ins.operands and ins.operands[0].type == cs.x86.X86_OP_IMM:
                return ins.operands[0].imm
        for op in ins.operands:
            if op.type == cs.x86.X86_OP_MEM and op.mem.base == cs.x86.X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp
        return None

    def annotate(self, t):
        o = self.rva2off(t)
        name = getattr(self, 'names', {}).get(t)
        if name:
            return name
        if o is None:
            return f'[{t:x}]'
        b = self.d[o:o + 80]
        if len(b) > 3 and all(32 <= c < 127 for c in b[:4]):
            e = b.find(b'\0')
            if e > 0:
                return repr(b[:e].decode('latin1'))
        if t in self._fstartset():
            return f'sub_{t:x}'
        return ''

    def dump(self, rva, end=None, n=None):
        for i in self.disasm(rva, n=n, end=end):
            print(self.fmt(i))

def load():
    p = PE(os.environ.get('ENSHROUDED_EXE') or os.path.join(os.path.dirname(__file__), 'enshrouded.exe'))
    return p

def _unwind_entry(self, rva):
    import bisect, struct
    o = self.rva2off(self.exc_rva)
    i = bisect.bisect_right(self.fstarts, rva) - 1
    return i

def real_start(self, rva):
    """Follow UNW_FLAG_CHAININFO to the primary function start."""
    import struct
    o = self.rva2off(self.exc_rva)
    if not hasattr(self, '_pd'):
        self._pd = {}
        for i in range(self.exc_size // 12):
            b, e, u = struct.unpack_from('<III', self.d, o + 12 * i)
            if b:
                self._pd[b] = (e, u)
    f = self.func_of(rva)
    guard = 0
    while f and guard < 20:
        guard += 1
        e, u = self._pd[f[0]]
        if u & 1:  # indirect
            break
        uo = self.rva2off(u)
        ver_flags, prolog, ncodes, frame = self.d[uo:uo + 4]
        flags = ver_flags >> 3
        if not (flags & 4):
            return f[0]
        n = (ncodes + 1) & ~1
        cb = uo + 4 + 2 * n
        pb, pe_, pu = struct.unpack_from('<III', self.d, cb)
        f = (pb, pe_)
    return f[0] if f else None

PE.real_start = real_start
