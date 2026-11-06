import pickle, numpy as np, time
t = time.time(); M = pickle.load(open('maps3.pkl', 'rb')); print('load', time.time() - t, flush=True)
xa = []; xb = []; fs = []; fr = []; fg = []
for f, (r, g, ap) in M['align'].items():
    fs.append(f); fr.append(r); fg.append(g)
    if ap:
        a, b = zip(*ap); xa.append(np.array(a, np.uint32)); xb.append(np.array(b, np.uint32))
XA = np.concatenate(xa); XB = np.concatenate(xb); o = np.argsort(XA, kind='stable'); XA, XB = XA[o], XB[o]
fs = np.array(fs, np.int64); o = np.argsort(fs)
dk = np.array(list(M['dmap'].keys()), np.int64); dv = np.array([v[0] for v in M['dmap'].values()], np.int64)
dn = np.array([v[1] for v in M['dmap'].values()], np.int32); dc = np.array([v[2] for v in M['dmap'].values()], np.int32); od = np.argsort(dk)
fmk = np.array(list(M['fmapB'].keys()), np.int64); fmv = np.array(list(M['fmapB'].values()), np.int64); of = np.argsort(fmk)
np.savez('al.npz', XA=XA, XB=XB, AF=fs[o], AR=np.array(fr)[o], AG=np.array(fg)[o], DK=dk[od], DV=dv[od], DN=dn[od], DC=dc[od],
         FMK=fmk[of], FMV=fmv[of], FA=M['FA'], EA=M['EA'], FB=M['FB'], EB=M['EB'])
print('done', len(XA), time.time() - t)
