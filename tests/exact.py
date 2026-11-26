"""Exactness of mkc build/dump/query/info against a brute-force counter.

usage: [MKC_TEST_ITERATE=./iterate-test] exact.py ./mkc [k...]
Covers every k (1..32 by default), FASTA and FASTQ, N runs, lowercase,
CRLF, empty and short records, heavy repeats (counts above every inline
limit), forced oversized-bin chunking (-M), several thread counts, and
both index layouts."""
import collections, gzip, json, os, random, shutil, subprocess, sys, tempfile

mkc = os.path.abspath(sys.argv[1])
iterate = os.environ.get('MKC_TEST_ITERATE')   # tests/iterate.c, run on every index
ks = [int(k) for k in sys.argv[2:]] or list(range(1, 33))
comp = str.maketrans('ACGT', 'TGCA')


def canonical(s):
    r = s.translate(comp)[::-1]
    return min(s, r)


def reference(records, k):
    c = collections.Counter()
    for rec in records:
        for part in ''.join(ch if ch in 'ACGT' else ' ' for ch in rec.upper()).split():
            for i in range(len(part)-k+1):
                c[canonical(part[i:i+k])] += 1
    return c


def run(*args, **kw):
    r = subprocess.run([mkc, *args], capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'FAIL {" ".join(args)}\n{r.stderr}')
    return r


rng = random.Random(20261022)
unit = ''.join(rng.choice('ACGT') for _ in range(400))
records = []
for i in range(60):
    n = rng.choice([0, 1, 5, 31, 33, 64, 500, 3000])
    s = ''.join(rng.choice('ACGT') for _ in range(n))
    if i % 7 == 0:
        s = s[:n//2] + 'NNNN' + s[n//2:]
    if i % 11 == 0:
        s = s.lower()
    records.append(s)
records.append(unit * 700)            # one long repeat: counts above 65535
records.append('A' * 70000)           # homopolymer: one huge count
records.append('ACGT' * 300)
# Counts exactly at the dense layout's all-ones values (255 and 65535), which
# have no exception entry and must read back as themselves.
records += [''.join(rng.choice('ACGT') for _ in range(32))] * 255
records += [''.join(rng.choice('ACGT') for _ in range(14))] * 65535

with tempfile.TemporaryDirectory(dir=os.environ.get('MKC_TEST_TMP')) as tmp:
    fasta = os.path.join(tmp, 'in.fa')
    with open(fasta, 'w', newline='') as f:
        for i, s in enumerate(records):
            eol = '\r\n' if i % 5 == 0 else '\n'
            f.write(f'>r{i}{eol}')
            for j in range(0, len(s), 61):
                f.write(s[j:j+61] + eol)
    fastq = os.path.join(tmp, 'in.fq')
    reads = [s for s in records if 0 < len(s) < 4000]
    with open(fastq, 'w') as f:
        for i, s in enumerate(reads):
            f.write(f'@q{i}\n{s}\n+\n{"I"*len(s)}\n')
    for k in ks:
        want = reference(records, k)
        for variant, extra in (('t4', ['-t', '4']), ('t1-chunked', ['-t', '1', '-M', '1']), ('t3', ['-t', '3'])):
            out = os.path.join(tmp, f'k{k}-{variant}.mkc')
            run('build', '-k', str(k), *extra, '-T', tmp, '-o', out, fasta)
            info = json.loads(run('info', out).stdout)
            if iterate:   # the iterator split into ranges visits the same k-mers
                r = subprocess.run([iterate, out], capture_output=True, text=True)
                assert r.returncode == 0, (k, variant, 'iterate', r.stdout)
            got = collections.Counter()
            for line in run('dump', out).stdout.splitlines():
                key, count = line.split('\t')
                assert key not in got, (k, variant, 'duplicate', key)
                got[key] = int(count)
            if got != want:
                missing = [x for x in want if got[x] != want[x]][:5]
                sys.exit(f'FAIL k={k} {variant}: {len(got)} vs {len(want)} keys, e.g. {missing}')
            assert info['distinct'] == len(want) and info['occurrences'] == sum(want.values()), (k, info)
            probe = list(want)[:200] + [''.join(rng.choice('ACGT') for _ in range(k)) for _ in range(200)]
            res = run('query', out, '-', input='\n'.join(probe) + '\n').stdout.splitlines()
            for q, line in zip(probe, res):
                assert int(line.split('\t')[1]) == want[canonical(q)], (k, variant, q)
            os.unlink(out)
        # gzip input, and stdin (plain and gzip), read through other paths.
        if k in (13, 21, 31) or len(ks) < 8:
            gz = fasta + '.gz'
            if not os.path.exists(gz):
                with open(fasta, 'rb') as src, gzip.open(gz, 'wb') as dst:
                    shutil.copyfileobj(src, dst)
            for source, stdin in ((gz, None), ('-', fasta), ('-', gz)):
                out = os.path.join(tmp, f'k{k}-in.mkc')
                with open(stdin, 'rb') if stdin else open(os.devnull, 'rb') as f:
                    r = subprocess.run([mkc, 'build', '-k', str(k), '-t', '2', '-T', tmp, '-o', out, source], stdin=f, capture_output=True, text=True)
                if r.returncode: sys.exit(f'FAIL k={k} input {source} {stdin}: {r.stderr}')
                got = collections.Counter({l.split('\t')[0]: int(l.split('\t')[1]) for l in run('dump', out).stdout.splitlines()})
                assert got == want, (k, 'input', source, stdin)
                os.unlink(out)
        # FASTQ input gives the same counts as FASTA for the same sequences.
        out = os.path.join(tmp, f'k{k}-fq.mkc')
        run('build', '-k', str(k), '-t', '2', '-T', tmp, '-o', out, fastq)
        want_fq = reference(reads, k)
        got = collections.Counter({l.split('\t')[0]: int(l.split('\t')[1]) for l in run('dump', out).stdout.splitlines()})
        assert got == want_fq, (k, 'fastq')
        os.unlink(out)
        print(f'k={k} ok layout={info["layout"]} distinct={len(want)}', flush=True)
    leftovers = [x for x in os.listdir(tmp) if x.startswith('mkc.') or '.tmp.' in x or '.exc.' in x]
    assert not leftovers, leftovers
print('exact: all ok')
