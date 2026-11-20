"""Python bindings: build through mkc.build, then check every API against a
brute-force counter. usage: python tests/python.py (with mkc importable)."""
import array, collections, os, random, sys, tempfile, threading
import mkc

comp = str.maketrans('ACGT', 'TGCA')
canon = lambda s: min(s, s.translate(comp)[::-1])
rng = random.Random(7)
seqs = [''.join(rng.choice('ACGT') for _ in range(rng.randrange(0, 2000))) for _ in range(40)]
seqs.append('ACGTN' * 50 + 'A' * 400)
with tempfile.TemporaryDirectory() as tmp:
    fa = os.path.join(tmp, 'x.fa')
    with open(fa, 'w') as f:
        for i, s in enumerate(seqs):
            f.write(f'>{i}\n{s}\n')
    for k in (5, 13, 20, 31, 32):
        want = collections.Counter()
        for s in seqs:
            for part in s.replace('N', ' ').split():
                for i in range(len(part) - k + 1):
                    want[canon(part[i:i + k])] += 1
        with mkc.build([fa], os.path.join(tmp, f'k{k}.mkc'), k=k, threads=3, tmpdir=tmp) as idx:
            assert idx.k == k and len(idx) == len(want) and idx.total == sum(want.values())
            assert dict(idx.items()) == dict(want) and dict(iter(idx)) == dict(want)
            probe = list(want)[:300] + [''.join(rng.choice('acgt') for _ in range(k)) for _ in range(300)]
            for q in probe:
                assert idx[q] == idx.count(q.encode()) == want[canon(q.upper())]
                assert (q in idx) == (want[canon(q.upper())] > 0)
            keys = array.array('Q', [mkc.encode(q) for q in probe])
            assert list(idx.count_keys(keys)) == [want[canon(q.upper())] for q in probe]
            out = array.array('Q', bytes(8 * len(keys)))
            assert idx.count_keys(keys, out) is out and list(out) == [want[canon(q.upper())] for q in probe]
            try:
                import numpy as np
                arr = np.array(keys, dtype=np.uint64)
                assert np.frombuffer(idx.count_keys(arr), dtype=np.uint64).tolist() == list(out)
                res = np.zeros(len(arr), dtype=np.uint64)
                idx.count_keys(arr, out=res)
                assert res.tolist() == list(out)
            except ImportError:
                pass
            s = seqs[-1] + seqs[0]
            got = list(idx.count_sequence(s))
            exp = [want[canon(s[i:i + k])] if 'N' not in s[i:i + k] else 0 for i in range(len(s) - k + 1)]
            assert got == exp
            assert mkc.decode(mkc.encode('ACGT' * 8), 32) == 'ACGT' * 8
            for bad in ('A' * (k + 1), 'N' * k):
                try:
                    idx[bad]
                except ValueError:
                    pass
                else:
                    raise AssertionError(bad)
            errors = []
            def worker():
                try:
                    for _ in range(20):
                        idx.count_keys(keys)
                except Exception as e:
                    errors.append(e)
            ts = [threading.Thread(target=worker) for _ in range(4)]
            [t.start() for t in ts]; [t.join() for t in ts]
            assert not errors
        assert idx.closed
        try:
            len(idx)
        except ValueError:
            pass
        else:
            raise AssertionError('closed index still usable')
        with mkc.Index(os.path.join(tmp, f'k{k}.mkc')) as again, mkc.Index(os.path.join(tmp, f'k{k}.mkc'), mapped=True) as mapped:
            assert list(again.count_keys(keys)) == list(mapped.count_keys(keys)) == list(out)
            print(f'python k={k} ok ({again.layout})')
    try:
        mkc.Index(fa)
    except ValueError:
        pass
    else:
        raise AssertionError('FASTA accepted as an index')
print('python: all ok')
