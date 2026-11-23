#!/usr/bin/env python3
"""Expands @@SEC name@@, @@RUN name@@ and @@FROM text@@TO text@@ directives in
*.md.in into fenced cpp blocks taken from the examples test, and verifies
that every ```cpp block of the result is a run of consecutive lines of it."""
import os, re, sys, textwrap
# Usage, from anywhere:  python3 docs/annotations-src/expand.py
# (no arguments: regenerates docs/annotations.md, tutorial-annotations.md and
# annotations-for-agents.md from the *.md.in files beside this script).
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TEST = os.path.join(ROOT, 'cpp', 'tests', 'doc_annotations_examples.cpp')
lines = open(TEST).read().split('\n')

def dedent(block):
    while block and not block[0].strip(): block = block[1:]
    while block and not block[-1].strip(): block = block[:-1]
    return textwrap.dedent('\n'.join(block))

def sec(name):
    start = [i for i, l in enumerate(lines) if l.startswith('// ── ' + name + ':') or l.startswith('// ── ' + name + ' ')]
    assert len(start) == 1, (name, start)
    i = start[0] + 1
    j = i
    while j < len(lines) and not lines[j].startswith('// ── ') and not lines[j].startswith('}  // namespace') and not lines[j].startswith('// ══'):
        j += 1
    return dedent(lines[i:j])

def run(name):
    a = [i for i, l in enumerate(lines) if l.strip() == '// ── ' + name + ' ──']
    b = [i for i, l in enumerate(lines) if l.strip() == '// ── /' + name + ' ──']
    assert len(a) == 1 and len(b) == 1, (name, a, b)
    return dedent(lines[a[0] + 1:b[0]])

def fromto(f, t):
    a = [i for i, l in enumerate(lines) if l.strip() == f.strip()]
    assert len(a) >= 1, ('from', f)
    i = a[0]
    b = [j for j in range(i, len(lines)) if lines[j].strip() == t.strip()]
    assert b, ('to', t)
    return dedent(lines[i:b[0] + 1])

def expand(text):
    def rep(m):
        kind = m.group(1)
        if kind == 'SEC': body = sec(m.group(2))
        elif kind == 'RUN': body = run(m.group(2))
        return '```cpp\n' + body + '\n```'
    text = re.sub(r'^@@(SEC|RUN) ([^@]+)@@$', rep, text, flags=re.M)
    text = re.sub(r'^@@FROM (.+?)@@TO (.+?)@@$', lambda m: '```cpp\n' + fromto(m.group(1), m.group(2)) + '\n```', text, flags=re.M)
    assert '@@' not in text, [l for l in text.split('\n') if '@@' in l][:5]
    return text

def check(text, name):
    stripped = [l.strip() for l in lines]
    n = 0
    for m in re.finditer(r'```cpp\n(.*?)\n```', text, flags=re.S):
        block = [l.strip() for l in m.group(1).split('\n')]
        ok = any(stripped[i:i + len(block)] == block for i in range(len(stripped) - len(block) + 1))
        n += 1
        if not ok:
            print('NOT IN TEST (%s): %s ...' % (name, m.group(1).split('\n')[0]))
    return n

if __name__ == '__main__':
    pairs = list(zip(sys.argv[1::2], sys.argv[2::2]))
    if not pairs:
        pairs = [(os.path.join(HERE, n + '.md.in'), os.path.join(ROOT, 'docs', n + '.md'))
                 for n in ('annotations', 'tutorial-annotations', 'annotations-for-agents')]
    for src, dst in pairs:
        out = expand(open(src).read())
        open(dst, 'w').write(out)
        print(dst, 'cpp blocks:', check(out, dst))
