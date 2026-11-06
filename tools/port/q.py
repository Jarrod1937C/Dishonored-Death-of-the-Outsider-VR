import numpy as np, os, sys
from pe import PE
home = os.path.expanduser('~')
pa = PE(os.environ.get('D2_EXE', home + '/mnt/DH2/Dishonored2.exe')); pb = PE(os.environ.get('DOTO_EXE', home + '/mnt/Dishonored_DeathOfTheOutsider/Dishonored_DO.exe'))
L = np.load('al.npz'); XA, XB = L['XA'], L['XB']; FA, EA = L['FA'], L['EA']
_za = np.load('d2.npz'); _zb = np.load('doto.npz'); _ra = _za['rva']; _rb = _zb['rva']; _la = _za['len']; _lb = _zb['len']
def lk(keys, vals, k):
    i = np.searchsorted(keys, k)
    return (int(vals[i]) if not isinstance(vals, tuple) else tuple(int(v[i]) for v in vals)) if i < len(keys) and keys[i] == k else None
def al(r): return lk(XA, XB, r)
def func_of(r):
    i = np.searchsorted(FA, r, 'right') - 1
    return int(FA[i]) if i >= 0 and FA[i] <= r < EA[i] else None
def sim(f):
    v = lk(L['AF'], L['AR'], f); return v if v is not None else 0.0
def sim(f):
    i = np.searchsorted(L['AF'], f); return float(L['AR'][i]) if i < len(L['AF']) and L['AF'][i] == f else 0.0
def dmap(r): return lk(L['DK'], (L['DV'], L['DN'], L['DC']), r)
def mapr(r):
    s = pa.sec_of_rva(r); s = s.name if s else None
    if s == '.text':
        f = func_of(r); g = lk(L['FMK'], L['FMV'], f) if f is not None else None
        if g is None:
            d = dmap(r)
            if d: return s, d[0], f'code ref by lea/rip, votes {d[1]}/{d[2]}'
            return s, None, 'not in a function' if f is None else f'func {f:#x}+{r-f:#x} unmatched'
        ratio = sim(f)
        if r == f: return s, g, f'func start {f:#x}->{g:#x} sim {ratio:.2f}'
        x = al(r)
        if x is not None: return s, x, f'insn in {f:#x}+{r-f:#x} -> {g:#x}+{x-g:#x} sim {ratio:.2f}'
        i = np.searchsorted(_ra, r, 'right') - 1; st = int(_ra[i]); y = al(st)
        if y is not None and st != r:
            j = np.searchsorted(_rb, y)
            if _la[i] == _lb[j]: return s, y + (r - st), f'mid-insn {st:#x}+{r-st} in {f:#x} -> {g:#x} sim {ratio:.2f}'
        return s, None, f'func {f:#x}->{g:#x} sim {ratio:.2f}, insn +{r-f:#x} not aligned (changed)'
    if s in ('.rdata', '.data', '.tls', '_RDATA'):
        d = dmap(r)
        if d: return s, d[0], f'data votes {d[1]}/{d[2]}'
        return s, None, 'data not referenced by aligned code'
    return s, None, 'not in image'
if __name__ == '__main__':
    for a in sys.argv[1:]:
        r = int(a, 16); s, v, why = mapr(r); print(f'{r:#x}', s, f'{v:#x}' if v is not None else '-', why)
_offa = np.concatenate([[0], np.cumsum(_la.astype(np.int64))]); _offb = np.concatenate([[0], np.cumsum(_lb.astype(np.int64))])
_na = _za['norm']; _nb = _zb['norm']
def body(which, f):
    F, E, ra, off, nm = (FA, EA, _ra, _offa, _na) if which == 'a' else (L['FB'], L['EB'], _rb, _offb, _nb)
    i = np.searchsorted(F, f); e = int(E[i]); i0 = np.searchsorted(ra, f); i1 = np.searchsorted(ra, e)
    return nm[off[i0]:off[i1]].tobytes()
def ident(f):
    g = lk(L['FMK'], L['FMV'], f)
    return g is not None and body('a', f) == body('b', g)
