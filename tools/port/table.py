import re, sys, os, collections
from q import mapr, pa
import offmap
root = os.path.expanduser('~') + '/mnt/Dishonored-Death-of-the-Outsider-VR/src/Src/'
for fn in sys.argv[1:]:
    seen = collections.OrderedDict()
    for n, line in enumerate(open(root + fn, errors='replace'), 1):
        for m in re.finditer(r'\b0x([0-9A-Fa-f]{5,7})\b', line):
            tok = m.group(0); v = int(m.group(1), 16); s = pa.sec_of_rva(v)
            if not s or s.name not in ('.text', '.rdata', '.data'): continue
            if tok not in seen: seen[tok] = [n, 0, line[max(0, m.start() - 28):m.end() + 12].strip()]
            seen[tok][1] += 1
    print('==', fn)
    for tok, (n, c, ctx) in seen.items():
        v = int(tok, 16); sec, nv, why = mapr(v)
        a = (f'{nv:#X}' if nv is not None else '?') + ' ' + re.sub(r'(func start|insn in|mid-insn|code ref by lea/rip,|data votes|sim|func) ?', '', why)[:38]
        votes, hits, _ = offmap.run(v, show=0)
        o = ','.join(f'{x:#X}:{k}' for x, k in votes.most_common(2)) + f'/{hits}'
        print(f'{tok:>10} L{n:<4} x{c} | A {a:<46} | O {o:<26} | {ctx[:70]}')
