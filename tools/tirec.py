"""Class name -> component typeinfo record (the one GetComponent takes; the inverse of what BodyHide's constants are).
The class registration does
    lea r8, "<parent>" ; lea rdx, "<name>" ; ... ; lea rcx, [rec] ; ...
e.g. exe+0x13D71E1: r8 "arkComponent", rdx "arkCpntInventory", rcx exe+0x39DCD30 (= BodyHide::kEquipCpntTypeInfo).
Other xrefs of the name (idStrId registrations: lea rdx name ; lea rcx rec ; call exe+0x131F90) are listed as
`strid` and are NOT what GetComponent wants.
usage: python tirec.py <ClassName> [<ClassName>...]
"""
import sys, re
from xref import pe, md

def rip_target(i):
    m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', i.op_str)
    if not m: return None
    return i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)

def recs_of(name):
    out = []
    for sva in pe.find_string(name.encode() + b'\0'):
        if pe.cstr(sva, 120) != name.encode(): continue
        for r in pe.xrefs_rip(sva):
            start = r - 0x18
            code = pe.read(start, 0x48)
            for skew in range(0, 12):
                insns = list(md.disasm(code[skew:], start + skew))
                hit = [x for x in insns if x.address <= r < x.address + x.size]
                if not hit: continue
                r0 = hit[0].address
                parent = None; rec = None
                for x in insns:
                    if x.mnemonic != 'lea': continue
                    t = rip_target(x)
                    if t is None: continue
                    if x.op_str.startswith('r8,') and x.address < r0:
                        s = pe.cstr(t, 80)
                        if s: parent = s.decode(errors='replace')
                    if x.op_str.startswith('rcx,') and x.address > r0 and rec is None:
                        rec = t
                out.append((r - pe.imagebase, parent, (rec - pe.imagebase) if rec else None))
                break
    return out

if __name__ == '__main__':
    for n in sys.argv[1:]:
        for r, parent, rec in recs_of(n):
            kind = f'component (parent {parent})' if parent else 'strid/other'
            print(f'{n}: rec exe+0x{rec:X} {kind}  [lea rdx at exe+0x{r:X}]' if rec else f'{n}: no rec [exe+0x{r:X}]')
