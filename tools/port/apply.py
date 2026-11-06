# apply.py <file> '{"0xTOKEN": "0xNEW", ...}'   keys: exact token (case-sensitive) or value; output keeps the token's case
import sys, json, re, os
root = os.path.expanduser('~') + '/mnt/Dishonored-Death-of-the-Outsider-VR/src/Src/'
p = root + sys.argv[1]; mp = json.loads(sys.argv[2]) if len(sys.argv) > 2 else json.load(sys.stdin)
exact = {k: v for k, v in mp.items()}; byval = {int(k, 16): v for k, v in mp.items()}
data = open(p, 'rb').read().decode('utf-8'); n = [0]
def rep(m):
    t = m.group(0); new = exact.get(t) or byval.get(int(t, 16))
    if new is None: return t
    h = new[2:]; h = h.lower() if any(c in 'abcdef' for c in t[2:]) else h.upper()
    n[0] += 1; return '0x' + h
data2 = re.sub(r'\b0x[0-9A-Fa-f]{5,7}\b', rep, data)
open(p, 'wb').write(data2.encode('utf-8')); print('replaced', n[0], 'in', sys.argv[1])
