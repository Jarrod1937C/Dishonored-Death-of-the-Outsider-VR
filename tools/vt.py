"""Vtable explorer for Dishonored_DO.exe.
usage: python vt.py ctor <ctorRva>          -> vtable(s) stored by the ctor
       python vt.py dump <vtableRva> [n]    -> entries, each with a quick scan (size, calls, cam-field reads)
       python vt.py scan <fnRva> [maxlen]   -> the scan of a single function
"""
import sys, re
from xref import pe, md, annotate

def read_ptr(va):
    import struct
    return struct.unpack('<Q', pe.read(va, 8))[0]

def fn_end(va, maxlen=0x3000):
    """Heuristic end: first int3 padding run after a ret, or maxlen."""
    code = pe.read(va, maxlen)
    insns = list(md.disasm(code, va))
    last = None
    for i in insns:
        if i.mnemonic == 'int3' and last is not None and last.mnemonic in ('ret', 'jmp'):
            return i.address
        last = i
    return va + maxlen

def scan(va, maxlen=0x3000, verbose=False):
    end = fn_end(va, maxlen)
    code = pe.read(va, end - va)
    calls, fields, strings = [], set(), []
    for i in md.disasm(code, va):
        if i.mnemonic == 'call' and i.op_str.startswith('0x'):
            calls.append(int(i.op_str, 16) - pe.imagebase)
        m = re.search(r'\[(r\w+) \+ 0x([0-9a-f]+)\]', i.op_str)
        if m:
            fields.add((m.group(1), int(m.group(2), 16)))
        a = annotate(i)
        if '; "' in a: strings.append(a.split('; ', 1)[1])
        if verbose: print(f"exe+0x{i.address-pe.imagebase:07X}  {a}")
    return end - va, calls, fields, strings

def ctor_vtables(rva):
    va = pe.imagebase + rva
    code = pe.read(va, 0x400)
    lea = {}
    out = []
    for i in md.disasm(code, va):
        m = re.match(r'(r\w+), \[rip ([+-]) 0x([0-9a-f]+)\]', i.op_str)
        if i.mnemonic == 'lea' and m:
            lea[m.group(1)] = i.address + i.size + int(m.group(3), 16) * (1 if m.group(2) == '+' else -1)
        m = re.match(r'qword ptr \[(r\w+)(?: \+ 0x([0-9a-f]+))?\], (r\w+)$', i.op_str)
        if i.mnemonic == 'mov' and m and m.group(3) in lea:
            out.append((m.group(1), int(m.group(2) or '0', 16), lea[m.group(3)]))
        if i.mnemonic == 'ret': break
    return out

def is_code(va):
    s = pe.sec_of_rva(va - pe.imagebase)
    return s is not None and (s.chars & 0x20000000) != 0

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'ctor':
        for reg, off, vt in ctor_vtables(int(sys.argv[2], 16)):
            print(f"[{reg}+0x{off:X}] = exe+0x{vt-pe.imagebase:X}")
    elif cmd == 'dump':
        vt = pe.imagebase + int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 64
        for k in range(n):
            p = read_ptr(vt + 8 * k)
            if not is_code(p): print(f"  [{k}] not code -> stop"); break
            size, calls, fields, strings = scan(p)
            cam = sorted({f for f in fields if f[1] in (0xd8, 0xfc, 0x2c, 0x38, 0x300, 0x30c)})
            print(f"  [{k:3}] +0x{8*k:03X} exe+0x{p-pe.imagebase:X} size=0x{size:X} calls={len(calls)} camlike={[(r, hex(o)) for r, o in cam]} {strings[:3]}")
    elif cmd == 'scan':
        va = pe.imagebase + int(sys.argv[2], 16)
        maxlen = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x3000
        size, calls, fields, strings = scan(va, maxlen, verbose=True)
        print(f"size=0x{size:X} calls={[hex(c) for c in calls]}")

def func_start(rva):
    va = pe.imagebase + rva
    start = va - 0x3000
    code = pe.read(start, 0x3000)
    idx = code.rfind(b'\xcc\xcc')
    while idx >= 0 and idx < len(code) and code[idx] == 0xCC: idx += 1
    return start + idx

if __name__ == '__main__' and sys.argv[1] == 'func':
    s = func_start(int(sys.argv[2], 16))
    print(f"--- function start exe+0x{s-pe.imagebase:X}")
    size, calls, fields, strings = scan(s, 0x3000, verbose=True)
    print(f"size=0x{size:X}")
