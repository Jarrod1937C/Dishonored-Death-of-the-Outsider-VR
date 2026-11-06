import numpy as np, struct, sys, collections
from q import al, _ra, _rb, _za, _zb
offa = np.concatenate([[0], np.cumsum(_za['len'].astype(np.int64))]); offb = np.concatenate([[0], np.cumsum(_zb['len'].astype(np.int64))])
na, nb = _za['norm'].tobytes(), _zb['norm'].tobytes()
def run(val, width=4, show=6, cap=300):
    pat = struct.pack('<I', val); votes = collections.Counter(); ex = []; nhit = 0; start = 0
    while nhit < cap:
        k = na.find(pat, start)
        if k < 0: break
        start = k + 1; nhit += 1
        i = np.searchsorted(offa, k, 'right') - 1; r = int(_ra[i]); pos = k - offa[i]
        y = al(r)
        if y is None: continue
        j = np.searchsorted(_rb, y); ba = na[offa[i]:offa[i + 1]]; bb = nb[offb[j]:offb[j + 1]]
        pos += len(bb) - len(ba)
        if pos < 0 or pos + 4 > len(bb): continue
        votes[struct.unpack_from('<I', bb, pos)[0]] += 1
        if len(ex) < show: ex.append((r, y))
    return votes, nhit, ex
if __name__ == '__main__':
    for a in sys.argv[1:]:
        v = int(a, 16); votes, n, ex = run(v)
        print(f'{v:#x}: hits {n}, aligned votes', [(hex(x), c) for x, c in votes.most_common(4)], 'eg', [(hex(x), hex(y)) for x, y in ex[:3]])
