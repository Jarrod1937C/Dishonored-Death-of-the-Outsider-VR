"""Dump id Tech typeinfo field registrations around a class registration site.
The registration code has the pattern (per field):
    mov dword ptr [rip+rec+8],  <offset>
    mov dword ptr [rip+rec+0xC], <size>
    ... lea rdx, "<fieldname>" ; lea rcx, rec ; call 0x140131f90
usage: python typeinfo.py <anchor-string> [span-hex=0x3000]
"""
import sys, re
from xref import pe, md

def find_function_start(va):
    """Walk back to the int3 padding / previous ret (heuristic)."""
    start = va - 0x4000
    code = pe.read(start, 0x4000)
    # find the last run of >=4 int3 (0xCC) before va
    idx = code.rfind(b'\xcc\xcc\xcc\xcc')
    while idx >= 0 and code[idx] == 0xCC: idx += 1
    return start + idx if idx > 0 else va - 0x800

def dump(anchor, span=0x3000):
    svas = pe.find_string(anchor.encode())
    for sva in svas:
        refs = pe.xrefs_rip(sva)
        for r in refs:
            fstart = find_function_start(r)
            print(f"\n=== '{anchor}' xref exe+0x{r-pe.imagebase:X}, function ~exe+0x{fstart-pe.imagebase:X}")
            code = pe.read(fstart, span)
            movs = {}   # target addr -> imm
            pending_name = None
            rows = []
            for i in md.disasm(code, fstart):
                m = re.match(r'dword ptr \[rip ([+-]) 0x([0-9a-f]+)\], (0x[0-9a-f]+|\d+)$', i.op_str)
                if i.mnemonic == 'mov' and m:
                    tgt = i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                    movs[tgt] = int(m.group(3), 16) if m.group(3).startswith('0x') else int(m.group(3))
                    continue
                m = re.match(r'rdx, \[rip ([+-]) 0x([0-9a-f]+)\]$', i.op_str)
                if i.mnemonic == 'lea' and m:
                    tgt = i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                    s = pe.cstr(tgt, 80)
                    if s and len(s) >= 2 and all(32 <= c < 127 for c in s):
                        pending_name = (s.decode(), tgt)
                    continue
                m = re.match(r'rcx, \[rip ([+-]) 0x([0-9a-f]+)\]$', i.op_str)
                if i.mnemonic == 'lea' and m and pending_name:
                    rec = i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                    rows.append((pending_name[0], rec))
                    pending_name = None
            for name, rec in rows:
                off, size = movs.get(rec + 8), movs.get(rec + 0xC)
                if off is None and size is None: continue
                print(f"  rec exe+0x{rec-pe.imagebase:X}  {name:40} off=+0x{off:X}" if off is not None else f"  rec exe+0x{rec-pe.imagebase:X}  {name:40} off=?",
                      f"size=0x{size:X}" if size is not None else "")

if __name__ == '__main__':
    dump(sys.argv[1], int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3000)
