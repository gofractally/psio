#!/usr/bin/env python3
import re, sys, os
def slug(h):
    h = h.strip().lower()
    h = re.sub(r'[^\w\- ]', '', h, flags=re.U)   # drop punctuation (keeps letters, digits, _, -, space)
    return h.replace(' ', '-')
def anchors(path):
    out, seen, fence = set(), {}, False
    for line in open(path):
        if line.startswith('```'): fence = not fence
        if fence: continue
        m = re.match(r'^(#{1,6})\s+(.*?)\s*$', line)
        if m:
            s = slug(m.group(2))
            n = seen.get(s, 0); seen[s] = n + 1
            out.add(s if n == 0 else '%s-%d' % (s, n))
    return out
bad = 0
for path in sys.argv[1:]:
    text = open(path).read()
    text_nofence = re.sub(r'```.*?```', '', text, flags=re.S)
    for m in re.finditer(r'\]\(([^)\s]+)\)', text_nofence):
        target = m.group(1)
        if target.startswith('http'): continue
        f, _, a = target.partition('#')
        fp = os.path.normpath(os.path.join(os.path.dirname(path), f)) if f else path
        if not os.path.exists(fp):
            print('MISSING FILE', path, target); bad += 1; continue
        if a and fp.endswith('.md') and a not in anchors(fp):
            print('MISSING ANCHOR', path, target); bad += 1
print('bad links:', bad)
