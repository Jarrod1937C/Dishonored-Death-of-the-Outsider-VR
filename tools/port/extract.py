import sys, re, subprocess, struct, zlib, numpy as np
num_re = re.compile(r'0x[0-9a-f]+|\b\d+\b')
exe, out, start, stop = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
base = 0x140000000
p = subprocess.Popen(['objdump','-d','-M','intel','--insn-width=16','-j','.text',f'--start-address={base+start:#x}',f'--stop-address={base+stop:#x}',exe], stdout=subprocess.PIPE, text=True, bufsize=1<<20)
line_re = re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
tgt_re = re.compile(r'# 0x([0-9a-f]+)')
shp=[]; rvas=[]; lens=[]; rs=[]; rd=[]; rk=[]; norm=bytearray()
for ln in p.stdout:
    m = line_re.match(ln)
    if not m: continue
    va = int(m.group(1), 16); hx = m.group(2); txt = m.group(3)
    bs = bytes.fromhex(hx.replace(' ', '')); n = len(bs); b = bytearray(bs); tgt = None; kind = 0
    if 'rip' in txt:
        t = tgt_re.search(txt)
        if t: tgt = int(t.group(1), 16); kind = 1
    elif n >= 5 and (bs[0] in (0xE8, 0xE9) or (bs[0] == 0x0F and 0x80 <= bs[1] <= 0x8F)):
        sp = txt.split()
        if len(sp) >= 2 and sp[1].startswith('0x'): tgt = int(sp[1], 16); kind = 2 if bs[0] == 0xE8 else 3
    if tgt is not None:
        i = bs.rfind(struct.pack('<I', (tgt - va - n) & 0xFFFFFFFF))
        if i >= 0: b[i:i+4] = b'\xAA\xAA\xAA\xAA'
        rs.append(va-base); rd.append(tgt-base); rk.append(kind)
    sh = num_re.sub('#', txt.split('#')[0].strip())
    shp.append(zlib.crc32(sh.encode()))
    rvas.append(va-base); lens.append(n); norm += b
np.savez(out, rva=np.array(rvas,np.uint32), len=np.array(lens,np.uint8), norm=np.frombuffer(bytes(norm),np.uint8),
         rsrc=np.array(rs,np.uint32), rdst=np.array(rd,np.int64), rkind=np.array(rk,np.uint8), shp=np.array(shp,np.uint32))
print(out, len(rvas), len(rs))
