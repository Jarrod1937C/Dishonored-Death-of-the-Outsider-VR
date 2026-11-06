"""Code that references a DATA address range RIP-relatively (a cvar object, a global), with the instruction.
usage: python dataref.py <rva-hex> [len-hex=0x80] [--max N]
Vectorised scan of .text for disp32 fields whose target falls in [rva, rva+len); each hit is confirmed by decoding an
instruction that ends at the disp (+0/+1/+4 immediate bytes) and actually uses rip.
"""
import sys
import numpy as np
from xref import pe, md, annotate

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    lo = int(args[0], 16); n = int(args[1], 16) if len(args) > 1 else 0x80
    mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 400
    text = next(x for x in pe.sections if x.name == '.text')
    raw = np.frombuffer(pe.data, dtype=np.uint8)[text.raw:text.raw + text.vsize]
    m = len(raw) - 8
    d = (raw[0:m].astype(np.int64) | (raw[1:m + 1].astype(np.int64) << 8) | (raw[2:m + 2].astype(np.int64) << 16)
         | (raw[3:m + 3].astype(np.int64) << 24))
    d = np.where(d >= 1 << 31, d - (1 << 32), d)
    pos = np.arange(m, dtype=np.int64) + text.va   # rva of the disp field
    seen = set(); out = []
    for imm in (0, 1, 2, 4):
        tgt = pos + 4 + imm + d
        for p in np.nonzero((tgt >= lo) & (tgt < lo + n))[0]:
            dr = int(pos[p])
            for start in range(dr - 1, dr - 12, -1):
                code = pe.read(pe.imagebase + start, 16)
                ins = next(md.disasm(code, pe.imagebase + start), None)
                if ins and 'rip' in ins.op_str and start + ins.size == dr + 4 + imm:
                    if start not in seen:
                        seen.add(start); out.append((start, ins))
                    break
    for start, ins in sorted(out)[:mx]:
        print(f"exe+0x{start:07X}  {annotate(ins)}")
    print(f"{len(out)} refs")

if __name__ == '__main__':
    main()
