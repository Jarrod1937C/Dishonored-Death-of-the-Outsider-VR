# find.py d2|doto "48 8B 0D ?? ?? ?? ?? 48 8B 11"  -> rvas in .text
import sys, re, os
from pe import PE
home = os.path.expanduser('~')
pe = PE(home + ('/mnt/DH2/Dishonored2.exe' if sys.argv[1] == 'd2' else '/mnt/Dishonored_DeathOfTheOutsider/Dishonored_DO.exe'))
pat = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in sys.argv[2].split())
tx = [s for s in pe.sections if s.name == '.text'][0]
d = pe.data[tx.raw:tx.raw + tx.rsize]
hits = [m.start() + tx.va for m in re.finditer(pat, d, re.S)]
print(len(hits), ' '.join(f'{h:#x}' for h in hits[:40]))
