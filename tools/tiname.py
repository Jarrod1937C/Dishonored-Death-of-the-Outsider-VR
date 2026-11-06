"""Resolve typeinfo record addresses to class names: the registration code does
    lea rdx, "<name>" ; lea rcx, [rec] ; call 0x1400010b0
usage: python tiname.py <recRva> [<recRva>...]
"""
import sys, re, struct
from xref import pe, md

def name_of(rec_rva):
    tva = pe.imagebase + rec_rva
    text = [s for s in pe.sections if s.name == '.text'][0]
    data = pe.data[text.raw:text.raw + text.rsize]
    # lea rcx,[rip+disp32] = 48 8D 0D disp32
    i = data.find(b'\x48\x8d\x0d')
    hits = []
    while i >= 0:
        disp = struct.unpack_from('<i', data, i + 3)[0]
        va = pe.imagebase + text.va + i
        if va + 7 + disp == tva: hits.append(va)
        i = data.find(b'\x48\x8d\x0d', i + 1)
    names = []
    for h in hits:
        code = pe.read(h - 0x20, 0x20 + 7)
        for skew in range(0, 16):
            insns = list(md.disasm(code[skew:], h - 0x20 + skew))
            if any(x.address == h for x in insns):
                prev = [x for x in insns if x.address < h]
                if prev:
                    p = prev[-1]
                    m = re.match(r'rdx, \[rip ([+-]) 0x([0-9a-f]+)\]', p.op_str)
                    if p.mnemonic == 'lea' and m:
                        t = p.address + p.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                        s = pe.cstr(t, 100)
                        if s: names.append(s.decode(errors='replace'))
                break
    return hits, names

if __name__ == '__main__':
    for a in sys.argv[1:]:
        hits, names = name_of(int(a, 16))
        print(f"exe+0x{int(a,16):X}: {names}  (lea rcx sites: {[hex(h-pe.imagebase) for h in hits]})")
