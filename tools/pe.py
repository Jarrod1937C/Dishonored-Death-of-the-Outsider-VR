"""Minimal PE64 helper for static RE of Dishonored_DO.exe (no external deps).
Usage: from pe import PE; pe = PE(path); pe.find_string(b"vieworg"); pe.xrefs_ptr(va)
"""
import struct, sys

class Section:
    def __init__(self, name, va, vsize, raw, rsize, chars):
        self.name, self.va, self.vsize, self.raw, self.rsize, self.chars = name, va, vsize, raw, rsize, chars
    def __repr__(self): return f"{self.name} va=0x{self.va:X} vsize=0x{self.vsize:X} raw=0x{self.raw:X}"

class PE:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        e_lfanew = struct.unpack_from('<I', d, 0x3C)[0]
        assert d[e_lfanew:e_lfanew+4] == b'PE\0\0'
        nsec = struct.unpack_from('<H', d, e_lfanew + 6)[0]
        optsize = struct.unpack_from('<H', d, e_lfanew + 20)[0]
        opt = e_lfanew + 24
        self.imagebase = struct.unpack_from('<Q', d, opt + 24)[0]
        self.entry = struct.unpack_from('<I', d, opt + 16)[0]
        self.sections = []
        p = opt + optsize
        for i in range(nsec):
            name = d[p:p+8].rstrip(b'\0').decode(errors='replace')
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, p + 8)
            chars = struct.unpack_from('<I', d, p + 36)[0]
            self.sections.append(Section(name, va, vsize, raw, rsize, chars))
            p += 40

    def sec_of_rva(self, rva):
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.rsize): return s
        return None
    def rva2off(self, rva):
        s = self.sec_of_rva(rva)
        if not s: return None
        off = rva - s.va + s.raw
        return off if off < s.raw + s.rsize else None
    def off2rva(self, off):
        for s in self.sections:
            if s.raw <= off < s.raw + s.rsize: return off - s.raw + s.va
        return None
    def va2off(self, va): return self.rva2off(va - self.imagebase)
    def off2va(self, off):
        r = self.off2rva(off); return None if r is None else r + self.imagebase
    def in_image(self, va):
        return self.imagebase <= va < self.imagebase + 0x10000000 and self.va2off(va) is not None

    def read(self, va, n):
        o = self.va2off(va); return self.data[o:o+n] if o is not None else b''
    def u64(self, va): return struct.unpack_from('<Q', self.data, self.va2off(va))[0]
    def u32(self, va): return struct.unpack_from('<I', self.data, self.va2off(va))[0]
    def i32(self, va): return struct.unpack_from('<i', self.data, self.va2off(va))[0]
    def f32(self, va): return struct.unpack_from('<f', self.data, self.va2off(va))[0]
    def cstr(self, va, maxlen=256):
        o = self.va2off(va)
        if o is None: return None
        e = self.data.find(b'\0', o, o + maxlen)
        return self.data[o:e if e >= 0 else o + maxlen]

    def find_string(self, s, exact=True):
        """VAs of NUL-terminated occurrences (preceded by NUL/start) of bytes s."""
        out, start = [], 0
        needle = s + b'\0'
        while True:
            i = self.data.find(needle, start)
            if i < 0: break
            if not exact or i == 0 or self.data[i-1] == 0:
                va = self.off2va(i)
                if va: out.append(va)
            start = i + 1
        return out

    def xrefs_ptr(self, va):
        """Data xrefs: 8-byte aligned qwords equal to va (absolute pointers)."""
        out, needle, start = [], struct.pack('<Q', va), 0
        while True:
            i = self.data.find(needle, start)
            if i < 0: break
            if i % 8 == 0:
                v = self.off2va(i)
                if v: out.append(v)
            start = i + 1
        return out

    def xrefs_rip(self, va, text_only=True):
        """Code xrefs via RIP-relative disp32: scan for 4-byte disp such that next_ip+disp == va.
        Brute force over .text: for each offset o, disp = int32 at o; target = va_of(o+4) + disp."""
        out = []
        for s in self.sections:
            if text_only and not (s.chars & 0x20000000): continue
            base = s.raw; end = s.raw + s.rsize
            d = self.data
            # vectorized-ish: iterate; ~40MB, python is slow but acceptable once (few seconds with memoryview tricks)
            import numpy as np
            arr = np.frombuffer(d, dtype=np.uint8, count=end - base, offset=base)
            # int32 at every byte offset
            i32 = (arr[0:-3].astype(np.uint32) | (arr[1:-2].astype(np.uint32) << 8) |
                   (arr[2:-1].astype(np.uint32) << 16) | (arr[3:].astype(np.uint32) << 24)).astype(np.int32)
            idx = np.arange(len(i32), dtype=np.int64)
            nextva = self.imagebase + s.va + idx + 4
            hit = np.nonzero(nextva + i32 == va)[0]
            out += [int(self.imagebase + s.va + h) for h in hit]
        return out

if __name__ == '__main__':
    pe = PE(sys.argv[1])
    print(f"imagebase=0x{pe.imagebase:X} entry=0x{pe.entry:X}")
    for s in pe.sections: print(' ', s, f"chars=0x{s.chars:X}")
