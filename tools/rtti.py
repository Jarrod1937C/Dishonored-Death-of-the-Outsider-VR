"""MSVC RTTI -> vtable RVAs for Dishonored_DO.exe.
usage: python rtti.py <substring> [<substring>...]   -> every class whose mangled name contains one of them
TypeDescriptor (name at +0x10) <- CompleteObjectLocator (sig 1, TD rva at +0xC, self rva at +0x14) <- the qword
just before the vtable. Validated by arkCpntPlayerCamera = exe+0x1C72218.
"""
import sys, struct, re
from xref import pe

def type_descriptors(subs):
    out = {}
    for m in re.finditer(rb'\.\?AV[A-Za-z0-9_@]+@@', pe.data):
        name = m.group(0).decode()
        if not any(s in name for s in subs): continue
        td_rva = pe.off2rva(m.start() - 0x10)
        out[td_rva] = name
    return out

def locators(tds):
    """COLs: signature 1, pTypeDescriptor rva at +0xC, pSelf rva at +0x14 == own rva."""
    res = {}
    d = pe.data
    for sec in pe.sections:
        if sec.name not in ('.rdata', '.data'): continue
        base = sec.raw
        for off in range(base, base + sec.rsize - 0x18, 4):
            if struct.unpack_from('<I', d, off)[0] != 1: continue
            td = struct.unpack_from('<I', d, off + 0xC)[0]
            if td not in tds: continue
            self_rva = struct.unpack_from('<I', d, off + 0x14)[0]
            if self_rva != pe.off2rva(off): continue
            objoff = struct.unpack_from('<I', d, off + 4)[0]
            res.setdefault(tds[td], []).append((pe.off2rva(off), objoff))
    return res

def vtables(col_rva):
    col_va = pe.imagebase + col_rva
    target = struct.pack('<Q', col_va)
    out = []
    i = pe.data.find(target)
    while i >= 0:
        out.append(pe.off2rva(i) + 8)
        i = pe.data.find(target, i + 1)
    return out

if __name__ == '__main__':
    tds = type_descriptors(sys.argv[1:])
    cols = locators(tds)
    for name in sorted(tds.values()):
        for col, objoff in cols.get(name, []):
            for vt in vtables(col):
                print(f"{name:60} vtable exe+0x{vt:X}  (offset 0x{objoff:X})")
