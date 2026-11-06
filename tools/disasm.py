"""Disassemble a range of Dishonored_DO.exe: python dis.py <rva-hex> [len-hex=0x200]"""
import sys
from xref import pe, md, annotate

def dis(rva, n=0x200):
    va = pe.imagebase + rva
    code = pe.read(va, n)
    for i in md.disasm(code, va):
        extra = ''
        if i.mnemonic in ('call', 'jmp') and i.op_str.startswith('0x'):
            extra = f'    ; -> exe+0x{int(i.op_str, 16) - pe.imagebase:X}'
        print(f"exe+0x{i.address - pe.imagebase:07X}  {annotate(i)}{extra}")

if __name__ == '__main__':
    dis(int(sys.argv[1], 16), int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x200)
