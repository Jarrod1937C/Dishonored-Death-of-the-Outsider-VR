import re, os, glob, collections
from q import mapr, pa
home = os.path.expanduser('~'); root = home + '/mnt/Dishonored-Death-of-the-Outsider-VR/src/Src/'
occ = collections.defaultdict(list)
for f in sorted(glob.glob(root + '*.cpp') + glob.glob(root + '*.h') + glob.glob(root + '*.inc')):
    for n, line in enumerate(open(f, errors='replace'), 1):
        for m in re.finditer(r'\b0x([0-9A-Fa-f]{5,7})\b', line):
            v = int(m.group(1), 16)
            occ[v].append((os.path.basename(f), n))
res = {}
for v in sorted(occ):
    s = pa.sec_of_rva(v)
    if not s or s.name not in ('.text','.rdata','.data','.tls'): continue
    res[v] = mapr(v)
import pickle; pickle.dump(dict(occ=dict(occ), res=res), open('modrvas.pkl','wb'))
c = collections.Counter()
for v, (s, nv, why) in res.items(): c[(s, nv is not None)] += 1
print(len(res), c)
