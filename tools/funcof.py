"""The function (from .pdata, following chained unwind info to the primary entry) that contains an RVA.
usage: python funcof.py <rva> [<rva>...]   (hex) -> begin, end of the primary function
"""
import sys, struct, bisect
from xref import pe

def table():
    s = next(x for x in pe.sections if x.name == '.pdata')
    out = []
    for o in range(s.raw, s.raw + s.vsize - 11, 12):
        b, e, u = struct.unpack_from('<III', pe.data, o)
        if b == 0: break
        out.append((b, e, u))
    return out

T = table(); B = [t[0] for t in T]

def rva2off(rva):
    for s in pe.sections:
        if s.va <= rva < s.va + max(s.vsize, s.rsize): return rva - s.va + s.raw
    return None

def primary(i):
    """follow UNW_FLAG_CHAININFO (flag 4) to the parent RUNTIME_FUNCTION"""
    b, e, u = T[i]
    for _ in range(16):
        o = rva2off(u & ~1)
        if o is None: break
        ver_flags = pe.data[o]; flags = ver_flags >> 3
        if not flags & 4: break
        ncodes = pe.data[o + 2]
        co = o + 4 + ((ncodes + 1) & ~1) * 2
        pb, pe_, pu = struct.unpack_from('<III', pe.data, co)
        b, e, u = pb, pe_, pu
    return b, e

def find(rva):
    i = bisect.bisect_right(B, rva) - 1
    if i < 0 or not (T[i][0] <= rva < T[i][1]): return None
    return T[i][:2], primary(i)

if __name__ == '__main__':
    for a in sys.argv[1:]:
        r = find(int(a, 16))
        if not r: print(a, 'not in a function'); continue
        (cb, ce), (pb, pe_) = r
        print(f"exe+0x{int(a,16):X}: chunk exe+0x{cb:X}..0x{ce:X}  primary exe+0x{pb:X}..0x{pe_:X}")
