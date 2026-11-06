import numpy as np, hashlib, pickle, os, time, collections, difflib, bisect
from pe import PE
t0 = time.time(); home = os.path.expanduser('~')
EXES = {'d2': os.environ.get('D2_EXE', home + '/mnt/DH2/Dishonored2.exe'), 'doto': os.environ.get('DOTO_EXE', home + '/mnt/Dishonored_DeathOfTheOutsider/Dishonored_DO.exe')}
class X: pass
def load(tag):
    x = X(); z = np.load(tag + '.npz'); pe = PE(EXES[tag]); x.pe = pe
    x.rva = z['rva']; x.off = np.concatenate([[0], np.cumsum(z['len'].astype(np.int64))]); x.norm = z['norm'].tobytes()
    x.shp = z['shp']; x.rs, x.rd, x.rk = z['rsrc'], z['rdst'], z['rkind']
    s = [q for q in pe.sections if q.name == '.pdata'][0]
    raw = np.frombuffer(pe.data[s.raw:s.raw + s.vsize - s.vsize % 12], dtype=np.uint32).reshape(-1, 3)
    raw = raw[raw[:, 0] != 0]
    pd = {}
    for b, e, u in raw.tolist(): pd.setdefault(b, e)
    calls = set(x.rd[x.rk == 2].tolist())
    tx = [q for q in pe.sections if q.name == '.text'][0]
    starts = sorted(set(pd) | {c for c in calls if tx.va <= c < tx.va + tx.vsize})
    starts = np.array(starts, np.int64)
    i0 = np.searchsorted(x.rva, starts); ok = i0 < len(x.rva); ok[ok] &= x.rva[i0[ok]] == starts[ok]
    starts = starts[ok]
    nxt = np.append(starts[1:], tx.va + tx.vsize)
    ends = np.array([min(pd.get(int(b), 1 << 40), int(n)) for b, n in zip(starts, nxt)], np.int64)
    x.B = starts; x.E = ends; x.I0 = np.searchsorted(x.rva, starts); x.I1 = np.searchsorted(x.rva, ends)
    x.R0 = np.searchsorted(x.rs, starts); x.R1 = np.searchsorted(x.rs, ends)
    x.H = [hashlib.md5(x.norm[x.off[a]:x.off[b]]).digest() for a, b in zip(x.I0.tolist(), x.I1.tolist())]
    x.idx = {int(b): i for i, b in enumerate(starts)}
    print(tag, 'funcs', len(starts), time.time() - t0, flush=True)
    return x
A = load('d2'); B = load('doto')
import sys
STATE = 'state.pkl'
pairs = {}
ha = collections.defaultdict(list); hb = collections.defaultdict(list)
for i, h in enumerate(A.H): ha[h].append(i)
for i, h in enumerate(B.H): hb[h].append(i)
for h, l in ha.items():
    if len(l) == 1 and len(hb.get(h, ())) == 1: pairs[l[0]] = hb[h][0]
print('exact unique', len(pairs), flush=True)
gapdone = set()
def shp(x, i): return x.shp[x.I0[i]:x.I1[i]].tolist()
def refs(x, i):
    a, b = int(x.R0[i]), int(x.R1[i]); return dict(zip(x.rs[a:b].tolist(), zip(x.rd[a:b].tolist(), x.rk[a:b].tolist())))
align = {}; fv = collections.defaultdict(collections.Counter); dv = collections.defaultdict(collections.Counter)
done = set()
if os.path.exists(STATE):
    st = pickle.load(open(STATE, 'rb')); pairs = st['pairs']; align = st['align']; done = st['done']; gapdone = st['gapdone']
    fv.update(st['fv']); dv.update(st['dv']); print('resumed', len(pairs), flush=True)
def save():
    pickle.dump(dict(pairs=pairs, align=align, done=done, gapdone=gapdone, fv=dict(fv), dv=dict(dv)), open(STATE + '.tmp', 'wb')); os.replace(STATE + '.tmp', STATE)
cnt_cache = {}
def counter(x, tag, i):
    k = (tag, i)
    if k not in cnt_cache: cnt_cache[k] = collections.Counter(shp(x, i))
    return cnt_cache[k]
def harvest():
    n = 0
    for i, j in list(pairs.items()):
        if i in done: continue
        done.add(i)
        sa, sb = shp(A, i), shp(B, j)
        ra = A.rva[A.I0[i]:A.I1[i]].tolist(); rb = B.rva[B.I0[j]:B.I1[j]].tolist()
        if A.H[i] == B.H[j]: ap = list(zip(ra, rb)); ratio = 1.0
        else:
            if len(sa) > 8000 or len(sb) > 8000: continue
            sm = difflib.SequenceMatcher(None, sa, sb, autojunk=False); ap = []
            for tg, a0, a1, b0, b1 in sm.get_opcodes():
                if tg == 'equal': ap += list(zip(ra[a0:a1], rb[b0:b1]))
            ratio = 2 * len(ap) / max(1, len(sa) + len(sb))
        align[int(A.B[i])] = (ratio, int(B.B[j]), ap)
        RA, RB = refs(A, i), refs(B, j)
        for p, q in ap:
            if p in RA and q in RB and RA[p][1] == RB[q][1]:
                (dv if RA[p][1] == 1 else fv)[RA[p][0]][RB[q][0]] += 1
        n += 1
    return n
def adopt():
    added = 0; usedb = set(pairs.values())
    for x, c in fv.items():
        i = A.idx.get(x)
        if i is None or i in pairs: continue
        (y, n), *rest = c.most_common(2); j = B.idx.get(y)
        if j is not None and j not in usedb and (not rest or n >= 2 * rest[0][1]): pairs[i] = j; usedb.add(j); added += 1
    return added
def gapfill():
    # monotone anchors
    items = sorted(pairs.items()); bs = [j for i, j in items]
    # LIS
    tails = []; tidx = []; prev = [-1] * len(bs)
    for k, v in enumerate(bs):
        p = bisect.bisect_left(tails, v)
        if p == len(tails): tails.append(v); tidx.append(k)
        else: tails[p] = v; tidx[p] = k
        prev[k] = tidx[p - 1] if p > 0 else -1
    k = tidx[-1]; lis = []
    while k >= 0: lis.append(items[k]); k = prev[k]
    lis.reverse()
    usedb = set(pairs.values()); added = 0
    for (i1, j1), (i2, j2) in zip(lis, lis[1:]):
        ua = [i for i in range(i1 + 1, i2) if i not in pairs]; ub = [j for j in range(j1 + 1, j2) if j not in usedb]
        if (i1, i2, j1, j2) in gapdone: continue
        gapdone.add((i1, i2, j1, j2))
        if time.time() - t0 > DEADLINE: return added
        if not ua or not ub or len(ua) * len(ub) > 20000: continue
        CA = [counter(A, 'a', i) for i in ua]; CB = [counter(B, 'b', j) for j in ub]
        LA = [sum(c.values()) for c in CA]; LB = [sum(c.values()) for c in CB]
        def sim(p, q):
            la, lb = LA[p], LB[q]
            if min(la, lb) < 3 or max(la, lb) > 2 * min(la, lb): return 0.0
            r = 2 * sum((CA[p] & CB[q]).values()) / (la + lb)
            return r if r >= 0.6 else 0.0
        n, m = len(ua), len(ub)
        D = np.zeros((n + 1, m + 1)); S = {}
        for p in range(1, n + 1):
            for q in range(1, m + 1):
                s = sim(p - 1, q - 1); S[(p, q)] = s
                D[p, q] = max(D[p - 1, q], D[p, q - 1], D[p - 1, q - 1] + s)
        p, q = n, m
        while p > 0 and q > 0:
            if S[(p, q)] > 0 and D[p, q] == D[p - 1, q - 1] + S[(p, q)]:
                sa, sb = shp(A, ua[p - 1]), shp(B, ub[q - 1])
                if len(sa) * len(sb) < 4e6 and difflib.SequenceMatcher(None, sa, sb, autojunk=False).ratio() >= 0.5:
                    pairs[ua[p - 1]] = ub[q - 1]; usedb.add(ub[q - 1]); added += 1
                p -= 1; q -= 1
            elif D[p, q] == D[p - 1, q]: p -= 1
            else: q -= 1
    return added
DEADLINE = 150
for it in range(30):
    h = harvest(); a = adopt(); save()
    g = gapfill() if a < 50 else 0; save()
    print('iter', it, 'harvested', h, 'adopted', a, 'gapfilled', g, 'pairs', len(pairs), round(time.time() - t0), flush=True)
    if a == 0 and g == 0: break
    if time.time() - t0 > DEADLINE: print('time'); break
harvest()
dmap = {}
for x, c in dv.items():
    (y, n), *rest = c.most_common(2)
    if not rest or n >= 2 * rest[0][1]: dmap[x] = (y, n, len(c))
pickle.dump(dict(fmapB={int(A.B[i]): int(B.B[j]) for i, j in pairs.items()}, align=align, dmap=dmap,
                 FA=A.B, EA=A.E, FB=B.B, EB=B.E), open('maps3.pkl', 'wb'))
print('done pairs', len(pairs), 'dmap', len(dmap), round(time.time() - t0))
