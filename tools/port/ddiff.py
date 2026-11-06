# ddiff.py <d2 func hex> : aligned instructions whose bytes differ (struct offsets / constants that moved)
import sys, subprocess, numpy as np, re
from q import *
def disl(which, s, e):
    exe = pa if which == 'a' else pb
    path = (os.path.expanduser('~') + ('/mnt/DH2/Dishonored2.exe' if which == 'a' else '/mnt/Dishonored_DeathOfTheOutsider/Dishonored_DO.exe'))
    out = subprocess.run(['objdump', '-d', '-M', 'intel', '--insn-width=12', f'--start-address={0x140000000+s:#x}', f'--stop-address={0x140000000+e:#x}', path], capture_output=True, text=True).stdout
    d = {}
    for ln in out.splitlines():
        m = re.match(r'^\s+14([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$', ln)
        if m: d[int(m.group(1), 16)] = re.sub(r'\s+', ' ', m.group(3)).split('#')[0].strip()
    return d
for a in sys.argv[1:]:
    f = int(a, 16); g = lk(L['FMK'], L['FMV'], f); i = np.searchsorted(FA, f); e = int(EA[i])
    j = np.searchsorted(L['FB'], g); eb = int(L['EB'][j])
    da, db = disl('a', f, e), disl('b', g, eb)
    print(f'== {f:#x} -> {g:#x} ({e-f:#x} / {eb-g:#x} bytes)')
    k0 = np.searchsorted(XA, f); k1 = np.searchsorted(XA, e); n = 0
    for x, y in zip(XA[k0:k1].tolist(), XB[k0:k1].tolist()):
        ta, tb = da.get(x, ''), db.get(y, '')
        ta2 = re.sub(r'0x14[0-9a-f]{7}', 'ADDR', ta); tb2 = re.sub(r'0x14[0-9a-f]{7}', 'ADDR', tb)
        st = lambda x: re.sub(r'\[(rbp|rsp)[+-]0x[0-9a-f]+\]', '[STK]', re.sub(r'(rsp|rbp),0x[0-9a-f]+', r'\1,N', x))
        if st(ta2) == st(tb2): continue
        if ta2 != tb2 and not ('rip' in ta and re.sub(r'rip[+-]0x[0-9a-f]+', '', ta) == re.sub(r'rip[+-]0x[0-9a-f]+', '', tb)): print(f'  {x:#x}: {ta:<50} | {y:#x}: {tb}'); n += 1
    print('  aligned', k1 - k0, 'changed', n)
