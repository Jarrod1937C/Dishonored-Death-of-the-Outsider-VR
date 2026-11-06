"""Find every `call rel32` to the given target RVAs across the .text section.
usage: python callers.py <rva> [<rva>...] [--range lo-hi]   (hex)
"""
import sys, struct
from xref import pe

def callers(target_rva):
    tva = pe.imagebase + target_rva
    text = [s for s in pe.sections if s.name == '.text'][0]
    data = pe.data[text.raw:text.raw + text.rsize]
    out = []
    i = data.find(b'\xe8')
    while i >= 0:
        if i + 5 <= len(data):
            rel = struct.unpack_from('<i', data, i + 1)[0]
            va = pe.imagebase + text.va + i
            if va + 5 + rel == tva: out.append(va - pe.imagebase)
        i = data.find(b'\xe8', i + 1)
    return out

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    rng = None
    for a in sys.argv[1:]:
        if a.startswith('--range'):
            lo, hi = a.split('=')[1].split('-'); rng = (int(lo, 16), int(hi, 16))
    for a in args:
        cs = callers(int(a, 16))
        if rng: cs = [c for c in cs if rng[0] <= c <= rng[1]]
        print(f"exe+0x{int(a,16):X}: {len(cs)} call sites")
        for c in cs: print(f"  exe+0x{c:X}")
