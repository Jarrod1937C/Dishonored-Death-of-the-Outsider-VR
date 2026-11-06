"""Code xrefs (RIP-relative) to strings + disassembly context.
usage: python xref.py <string> [<string>...] [--ctx N]
"""
import sys
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Dishonored_DeathOfTheOutsider\Dishonored_DO.exe"
pe = PE(EXE)
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False

def annotate(insn):
    """Append a comment when the operand is RIP-relative to a string / known area."""
    txt = f"{insn.mnemonic} {insn.op_str}"
    if 'rip' in insn.op_str:
        # capstone prints [rip + 0x...] ; compute target
        import re
        m = re.search(r'rip ([+-]) 0x([0-9a-f]+)', insn.op_str)
        if m:
            disp = int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
            tgt = insn.address + insn.size + disp
            s = pe.cstr(tgt, 60)
            if s and len(s) >= 3 and all(32 <= c < 127 for c in s):
                txt += f'    ; "{s.decode()}"'
            else:
                txt += f'    ; exe+0x{tgt - pe.imagebase:X}'
    return txt

def disasm_around(va, before=0x60, after=0x80):
    start = va - before
    code = pe.read(start, before + after)
    # align: try to find a decode start that lands exactly on va
    for skew in range(0, 16):
        insns = list(md.disasm(code[skew:], start + skew))
        if any(i.address == va for i in insns):
            return insns
    return list(md.disasm(code, start))

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    for s in args:
        for sva in pe.find_string(s.encode()):
            refs = pe.xrefs_rip(sva)
            print(f"\n=== '{s}' @ exe+0x{sva-pe.imagebase:X}  code xrefs: {[hex(r-pe.imagebase) for r in refs]}")
            for r in refs[:6]:
                print(f"--- xref exe+0x{r-pe.imagebase:X}")
                for i in disasm_around(r):
                    mark = '>>' if i.address <= r < i.address + i.size else '  '
                    print(f"{mark} exe+0x{i.address-pe.imagebase:07X}  {annotate(i)}")

if __name__ == '__main__':
    main()
