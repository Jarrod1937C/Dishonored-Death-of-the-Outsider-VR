"""Find functions (from .pdata) whose code references ALL given displacements, e.g.
   python scan_disp.py 0x300 0xa8      -> functions touching [reg+0x300] and [reg+0xa8]
Linear-disassembles .text once (slow-ish, ~minutes) and caches per-function displacement sets.
"""
import sys, re, struct, pickle, os
from xref import pe, md

CACHE = os.path.join(os.path.dirname(__file__), '.disp_cache.pkl')

def pdata_functions():
    s = next(x for x in pe.sections if x.name == '.pdata')
    out = []
    for o in range(s.raw, s.raw + s.vsize - 11, 12):
        begin, end, unwind = struct.unpack_from('<III', pe.data, o)
        if begin == 0: break
        out.append((begin, end))
    return out

def build():
    funcs = pdata_functions()
    text = next(x for x in pe.sections if x.name == '.text')
    disp_re = re.compile(r'\+ 0x([0-9a-f]+)\]')
    table = []
    for n, (b, e) in enumerate(funcs):
        code = pe.read(pe.imagebase + b, e - b)
        disps = set()
        for i in md.disasm(code, pe.imagebase + b):
            for m in disp_re.finditer(i.op_str):
                disps.add(int(m.group(1), 16))
        table.append((b, e, disps))
        if n % 5000 == 0: print(f"  {n}/{len(funcs)} functions...", file=sys.stderr)
    pickle.dump(table, open(CACHE, 'wb'))
    return table

def load():
    return pickle.load(open(CACHE, 'rb')) if os.path.exists(CACHE) else build()

if __name__ == '__main__':
    want = [int(a, 16) for a in sys.argv[1:]]
    table = load()
    hits = [(b, e) for b, e, d in table if all(w in d for w in want)]
    print(f"{len(hits)} functions reference all of {[hex(w) for w in want]}:")
    for b, e in hits[:80]:
        print(f"  exe+0x{b:X} .. exe+0x{e:X} ({e-b} bytes)")
