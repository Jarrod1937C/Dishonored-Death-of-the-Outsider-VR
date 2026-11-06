import numpy as np, collections, sys
from q import al
za = np.load('d2.npz'); zb = np.load('doto.npz')
rsb, rdb = zb['rsrc'], zb['rdst']
def run(t):
    src = za['rsrc'][(za['rdst'] == t) & (za['rkind'] >= 2)]
    c = collections.Counter(); n = 0
    for s in src.tolist():
        y = al(s)
        if y is None: continue
        i = np.searchsorted(rsb, y)
        if i < len(rsb) and rsb[i] == y: c[int(rdb[i])] += 1; n += 1
    return c, len(src)
if __name__ == '__main__':
    for a in sys.argv[1:]:
        c, n = run(int(a, 16)); print(a, f'callers {n}', [(hex(k), v) for k, v in c.most_common(3)])
