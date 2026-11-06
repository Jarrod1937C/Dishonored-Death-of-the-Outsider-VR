"""Turn the CRASH lines of a DOTOVR.log into function names and source lines.

CrashLog writes every frame as module+RVA (e.g. DOTOVR.dll+0x164452). Given the PDB of the SAME build (scripts\
package.ps1 keeps one per release in local\releases\<version>-<commit>\), this resolves them with Windows' own dbghelp:

    python tools/symbolize.py <DOTOVR.log> [folder with DOTOVR.dll + .pdb, default build/bin]

Only DOTOVR.dll frames are resolved; the game's and Windows' are left as they are.
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

log = sys.argv[1]
bindir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "build", "bin")
dll = os.path.abspath(os.path.join(bindir, "DOTOVR.dll"))

dbghelp = ctypes.WinDLL("dbghelp.dll")
proc = ctypes.c_void_p(0xD1501)          # any unique handle value works for an offline session
dbghelp.SymSetOptions(0x2 | 0x10)         # SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
if not dbghelp.SymInitializeW(proc, ctypes.c_wchar_p(os.path.dirname(dll)), False):
    sys.exit("SymInitialize failed")
BASE = 0x10000000
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
if not dbghelp.SymLoadModuleExW(proc, None, ctypes.c_wchar_p(dll), None, ctypes.c_uint64(BASE), 0x4000000, None, 0):
    sys.exit("SymLoadModuleEx failed for " + dll)


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_wchar * 1024)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_uint64)]


def resolve(rva):
    addr = ctypes.c_uint64(BASE + rva)
    si = SYMBOL_INFOW(); si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW) - 1024 * 2 + 2; si.MaxNameLen = 1024
    disp = ctypes.c_uint64(0)
    name = "?"
    if dbghelp.SymFromAddrW(proc, addr, ctypes.byref(disp), ctypes.byref(si)):
        name = f"{si.Name}+0x{disp.value:X}"
    ln = IMAGEHLP_LINEW64(); ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINEW64)
    d32 = wt.DWORD(0)
    if dbghelp.SymGetLineFromAddrW64(proc, addr, ctypes.byref(d32), ctypes.byref(ln)):
        name += f"  ({os.path.basename(ln.FileName)}:{ln.LineNumber})"
    return name


pat = re.compile(r"DOTOVR\.dll\+0x([0-9A-Fa-f]+)")
for line in open(log, encoding="utf-8", errors="replace"):
    if "CRASH" not in line:
        continue
    print(line.rstrip())
    for m in pat.finditer(line):
        print(f"        {m.group(0):28} {resolve(int(m.group(1), 16))}")
