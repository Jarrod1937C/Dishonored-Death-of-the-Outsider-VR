# edit.py <file>  (stdin: JSON list of [old, new] or [old, new, count]); \n in old/new = CRLF-aware
import sys, json
p = sys.argv[1]; data = open(p, 'rb').read().decode('utf-8')
crlf = '\r\n' in data
for e in json.load(sys.stdin):
    old, new = e[0], e[1]; cnt = e[2] if len(e) > 2 else 1
    if crlf and data.count(old.replace('\r\n', '\n').replace('\n', '\r\n')):
        old = old.replace('\r\n', '\n').replace('\n', '\r\n'); new = new.replace('\r\n', '\n').replace('\n', '\r\n')
    n = data.count(old)
    if n != cnt: sys.exit(f'{p}: expected {cnt} match(es), found {n}: {old[:80]!r}')
    data = data.replace(old, new)
open(p, 'wb').write(data.encode('utf-8')); print('ok', p)
