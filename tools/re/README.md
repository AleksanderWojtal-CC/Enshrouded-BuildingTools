# Reverse-engineering scripts

These are the Python helpers used to analyse `enshrouded.exe` while building the mod. They
read the executable; they never modify it. The executable is not part of this
repository: point the scripts at your own copy.

```sh
pip install capstone
export ENSHROUDED_EXE="/path/to/Enshrouded/enshrouded.exe"
python3 index.py      # one-off: cross-reference index -> xidx.pkl (takes a few minutes)
python3 refl.py       # one-off: dump the engine's reflection data -> types.txt / types.pkl
```

| Script | Purpose |
|---|---|
| `pe.py` | PE loader: sections, `.pdata` function table, chained-unwind function starts, capstone disassembly |
| `index.py` | Full sweep: rip-relative data references, call and jump targets (`xidx.pkl`) |
| `q.py` | Queries on the index: string → code refs, callers, function of an address, hexdump |
| `cg.py` | Function bodies (follows in-function jumps), callees, call trees |
| `refl.py` | Engine reflection dump: about 12,600 types with field offsets and enum values |
| `sigs.py` | Builds the shortest unique byte signature for an address (wildcards for rip/rel32 displacements) and verifies it |

Typical use from Python:

```python
from cg import *                       # also imports p (PE), callers(), fn()
for i in body(0x48a660): print(hex(i.address), i.mnemonic, i.op_str)
print([hex(c) for c in callers(0x48a660)])

from sigs import make, fmt, find
m = make(0x48a660); print(fmt(m), [hex(x) for x in find(m)])
```

The findings, with every address used by the mod, are in
[`docs/reverse-engineering-notes.md`](../../docs/reverse-engineering-notes.md).
