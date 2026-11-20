"""FASTA parsing: the parallel block parser against the serial one (-t 1)
and a brute-force counter, on inputs with every kind of line structure.

usage: fasta.py ./mkc
Best run with a build using tiny read blocks (-DMKC_READ_BLOCK=4096), so
lines, headers and records are cut at block ends."""
import collections, os, random, re, subprocess, sys, tempfile

mkc = os.path.abspath(sys.argv[1])
comp = str.maketrans('ACGT', 'TGCA')
K = 21
rng = random.Random(11)


def reference(records):
    c = collections.Counter()
    for s in records:
        for part in ''.join(ch if ch in 'ACGT' else ' ' for ch in s.upper()).split():
            for i in range(len(part)-K+1):
                w = part[i:i+K]; r = w.translate(comp)[::-1]
                c[min(w, r)] += 1
    return c


def build(path, threads, tmp):
    out = os.path.join(tmp, 'o.mkc')
    r = subprocess.run([mkc, 'build', '-k', str(K), '-t', str(threads), '-T', tmp, '-o', out, path], capture_output=True, text=True)
    if r.returncode:
        return None, 0, r.stderr.strip()
    m = re.search(r'parsed in parallel: (\d+) bytes', r.stderr)
    d = subprocess.run([mkc, 'dump', out], capture_output=True, text=True, check=True).stdout
    os.unlink(out)
    return collections.Counter({l.split('\t')[0]: int(l.split('\t')[1]) for l in d.splitlines()}), int(m.group(1)) if m else 0, None


def seq(n, lower=False):
    s = ''.join(rng.choice('ACGTN' if rng.random() < 0.05 else 'ACGT') for _ in range(n))
    return s.lower() if lower else s


def write(path, recs, eol='\n', wrap=60, blank=False, last_eol=True, long_header=False):
    """Writes records and returns the sequences as the parser should see them."""
    parts, seqs = [], []
    for i, s in enumerate(recs):
        head = '>r%d %s' % (i, 'x'*5000 if long_header and i % 7 == 0 else 'description')
        e = eol if isinstance(eol, str) else eol(i)
        parts.append(head + e)
        if wrap:
            for j in range(0, len(s), wrap): parts.append(s[j:j+wrap] + e)
        elif s: parts.append(s + e)
        if blank: parts.append(e)
        seqs.append(s)
    text = ''.join(parts)
    if not last_eol: text = text.rstrip('\r\n')
    with open(path, 'w', newline='') as f: f.write(text)
    return seqs


cases = [
    ('plain', dict()),
    ('crlf', dict(eol='\r\n')),
    ('mixed-eol', dict(eol=lambda i: '\r\n' if i % 2 else '\n')),
    ('unwrapped', dict(wrap=None)),
    ('odd-wrap', dict(wrap=61)),
    ('blank-lines', dict(blank=True)),
    ('no-final-newline', dict(last_eol=False)),
    ('long-headers', dict(long_header=True)),
]
failed = 0
with tempfile.TemporaryDirectory(dir=os.environ.get('MKC_TEST_TMP')) as tmp:
    path = os.path.join(tmp, 'in.fa')
    for name, opts in cases:
        recs = [seq(rng.randint(0, 3000), lower=rng.random() < 0.1) for _ in range(400)]
        recs.insert(200, seq(30000))   # a sequence spanning several blocks
        seqs = write(path, recs, **opts)
        want = reference(seqs)
        for t in (1, 2, 5, 16):
            got, par, err = build(path, t, tmp)
            if got != want:
                failed += 1; print(f'FAIL {name} t={t}: {err or f"{len(got)} vs {len(want)} keys"}')
            if t > 1 and par < 0.5*os.path.getsize(path):
                failed += 1; print(f'FAIL {name} t={t}: only {par} of {os.path.getsize(path)} bytes parsed in parallel')
        print(f'{name} ok', flush=True)
print('fasta: ' + ('all ok' if not failed else f'{failed} failures'))
sys.exit(1 if failed else 0)
