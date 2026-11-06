import struct, sys
from q import pa, pb
def iat(pe):
    d = pe.data; e = struct.unpack_from('<I', d, 0x3C)[0]; opt = e + 24
    rva, size = struct.unpack_from('<II', d, opt + 112 + 8)   # import dir (index 1)
    out = {}; o = pe.rva2off(rva)
    while True:
        oft, ts, fc, name, ft = struct.unpack_from('<IIIII', d, o); o += 20
        if not name: break
        dll = pe.cstr(pe.imagebase + name).decode()
        t = oft or ft; k = 0
        while True:
            v = struct.unpack_from('<Q', d, pe.rva2off(t) + k * 8)[0]
            if not v: break
            nm = f'#{v & 0xffff}' if v >> 63 else pe.cstr(pe.imagebase + (v & 0x7fffffff) + 2).decode()
            out[ft + k * 8] = f'{dll}!{nm}'; k += 1
    return out
A, B = iat(pa), iat(pb)
if __name__ == '__main__':
    for x in sys.argv[1:]:
        w, r = x.split(':'); r = int(r, 16); print(w, hex(r), (A if w == 'a' else B).get(r))
