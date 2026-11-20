"""FASTQ parsing: the parallel block parser against the serial one (-t 1)
and a brute-force counter, on inputs that exercise every way out of it.

usage: fastq.py ./mkc
Best run with a build using tiny read blocks (-DMKC_READ_BLOCK=4096), so
records are cut at block ends, some across several blocks."""
import collections, os, random, re, subprocess, sys, tempfile

mkc = os.path.abspath(sys.argv[1])
comp = str.maketrans('ACGT', 'TGCA')
K = 21


def reference(seqs):
    c = collections.Counter()
    for s in seqs:
        for part in ''.join(ch if ch in 'ACGT' else ' ' for ch in s.upper()).split():
            for i in range(len(part)-K+1):
                w = part[i:i+K]; r = w.translate(comp)[::-1]
                c[min(w, r)] += 1
    return c


parallel = 0   # input bytes the last build parsed in parallel


def build(path, threads, tmp):
    global parallel
    out = os.path.join(tmp, 'o.mkc')
    r = subprocess.run([mkc, 'build', '-k', str(K), '-t', str(threads), '-T', tmp, '-o', out, path], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()
    m = re.search(r'parsed in parallel: (\d+) bytes', r.stderr)
    parallel = int(m.group(1)) if m else 0
    d = subprocess.run([mkc, 'dump', out], capture_output=True, text=True, check=True).stdout
    os.unlink(out)
    return collections.Counter({l.split('\t')[0]: int(l.split('\t')[1]) for l in d.splitlines()}), None


rng = random.Random(7)
qual = ''.join(chr(c) for c in range(33, 127))
def read(n, lower=False, ns=False):
    s = ''.join(rng.choice('ACGT') for _ in range(n))
    if ns and n > 10: s = s[:n//2] + 'NN' + s[n//2+2:]
    return s.lower() if lower else s
def quality(n):
    # Quality lines that start with '@' or '+' are what resync must not trust.
    q = ''.join(rng.choice(qual) for _ in range(n))
    return (rng.choice('@+') + q[1:]) if n and rng.random() < 0.3 else q


def write(path, recs, eol='\n', wrap=None, last_eol=True):
    with open(path, 'w', newline='') as f:
        for i, (s, q) in enumerate(recs):
            e = eol if isinstance(eol, str) else eol(i)
            if wrap and len(s) > wrap:
                body = e.join(s[j:j+wrap] for j in range(0, len(s), wrap))
                qbody = e.join(q[j:j+wrap] for j in range(0, len(q), wrap))
            else:
                body, qbody = s, q
            tail = e if (last_eol or i+1 < len(recs)) else ''
            f.write(f'@r{i} x{e}{body}{e}+{e}{qbody}{tail}')


def reads(count, long_every=0):
    out = []
    for i in range(count):
        n = rng.choice([0, 1, 20, 21, 100, 150, 151, 300])
        if long_every and i % long_every == 0:
            n = rng.choice([5000, 9000, 20000])   # longer than several blocks
        s = read(n, lower=i % 13 == 0, ns=i % 9 == 0)
        out.append((s, quality(n)))
    return out


cases = []
base = reads(3000)
cases.append(('plain', base, {}))
cases.append(('long-reads', reads(1500, long_every=50), {}))
cases.append(('crlf', base, {'eol': '\r\n'}))
cases.append(('mixed-eol', base, {'eol': lambda i: '\r\n' if i % 3 == 0 else '\n'}))
cases.append(('no-final-newline', base, {'last_eol': False}))
cases.append(('multi-line', base, {'wrap': 60}))
late = reads(2000)
late[1700] = (read(400), quality(400))
cases.append(('multi-line-late', late, {'wrap': 350}))   # only one record wraps
cases.append(('tiny', [(read(30), quality(30))], {}))
cases.append(('empty-reads', [('', '')]*50 + reads(200), {}))

bad = []
r = reads(1500)
bad.append(('bad-quality-byte', r[:1000] + [(r[1000][0], ' '*len(r[1000][0]))] + r[1001:], {}))
r = reads(1500); s = read(200)
bad.append(('short-quality', r[:900] + [(s, quality(150))] + r[900:], {}))
r = reads(1500); s = read(200)
bad.append(('long-quality', r[:900] + [(s, quality(250))] + r[900:], {}))

failed = 0
with tempfile.TemporaryDirectory(dir=os.environ.get('MKC_TEST_TMP')) as tmp:
    path = os.path.join(tmp, 'in.fq')
    for name, recs, opts in cases:
        write(path, recs, **opts)
        want = reference(s for s, _ in recs)
        for t in (1, 2, 5, 16):
            got, err = build(path, t, tmp)
            if got != want:
                failed += 1; print(f'FAIL {name} t={t}: {err or f"{len(got)} vs {len(want)} keys"}')
            # Records cut by blocks (and reads longer than one) are completed
            # serially; well-formed short records must stay on the parallel path.
            if t > 1 and name in ('plain', 'crlf', 'mixed-eol', 'no-final-newline', 'empty-reads') and parallel < 0.8*os.path.getsize(path):
                failed += 1; print(f'FAIL {name} t={t}: only {parallel} of {os.path.getsize(path)} bytes parsed in parallel')
        print(f'{name} ok', flush=True)
    for name, recs, opts in bad:
        write(path, recs, **opts)
        _, serial = build(path, 1, tmp)
        assert serial, name
        for t in (2, 5, 16):
            _, err = build(path, t, tmp)
            if err != serial:
                failed += 1; print(f'FAIL {name} t={t}: {err!r} vs serial {serial!r}')
        print(f'{name} ok ({serial})', flush=True)
    # A blank line inside, and text that is not FASTQ after valid records.
    for name, text in (('blank-line', '@a\nACGTACGTACGTACGTACGTACGT\n+\nIIIIIIIIIIIIIIIIIIIIIIII\n\n@b\nACGT\n+\nIIII\n'),
                       ('missing-plus', '@a\nACGTACGTACGTACGTACGTACGT\nIIIIIIIIIIIIIIIIIIIIIIII\n'),
                       ('truncated', '@a\nACGTACGTACGTACGTACGTACGT\n+\n')):
        with open(path, 'w') as f:
            f.write(''.join(f'@p{i}\n{s}\n+\n{q}\n' for i, (s, q) in enumerate(reads(800))) + text)
        _, serial = build(path, 1, tmp)
        assert serial, name
        for t in (2, 16):
            _, err = build(path, t, tmp)
            if err != serial:
                failed += 1; print(f'FAIL {name} t={t}: {err!r} vs serial {serial!r}')
        print(f'{name} ok ({serial})', flush=True)
sys.exit(f'fastq: {failed} failures' if failed else print('fastq: all ok'))
