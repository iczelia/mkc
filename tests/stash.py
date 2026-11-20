#!/usr/bin/env python3
"""Key sets aimed at the fixed hash: far more keys in one group than it has
slots, so that most go to the stash; the index must stay exact.
usage: stash.py ./mkc ./mkc-no-reseed ./adversarial-test"""
import json, os, subprocess, sys, tempfile
base, stashing, gen = sys.argv[1], sys.argv[2], sys.argv[3]
failed = 0
with tempfile.TemporaryDirectory() as tmp:
    for k, n, p, r in ((31, 200000, 30, 100000), (21, 60000, 20, 0), (25, 150000, 26, 20000)):
        fa, want, idx = (os.path.join(tmp, x) for x in ('in.fa', 'want.tsv', 'out.mkc'))
        with open(fa, 'wb') as f:
            subprocess.run([gen, str(k), str(n), str(p), str(r), want], stdout=f, check=True)
        expected = sorted(open(want).read().split('\n')[:-1])
        # 1. The stash path: reseeding disabled, most keys of the group must be stashed.
        # 2. The default: the builder notices, builds again with the next hash seed, and the stash is empty.
        for label, mkc in (('stash', stashing), ('reseeded', base)):
            digests = set()
            for threads in (1, 4):
                subprocess.run([mkc, 'build', '-k', str(k), '-t', str(threads), '-o', idx, fa], check=True, stderr=subprocess.DEVNULL)
                info = json.loads(subprocess.run([mkc, 'info', idx], capture_output=True, text=True, check=True).stdout)
                got = sorted(subprocess.run([mkc, 'dump', idx], capture_output=True, text=True, check=True).stdout.split('\n')[:-1])
                digests.add(open(idx, 'rb').read())
                sample = [line.split('\t') for line in expected[::max(1, len(expected)//500)]]
                answers = subprocess.run([mkc, 'query', idx, *[s[0] for s in sample]], capture_output=True, text=True, check=True).stdout.split('\n')[:-1]
                if got != expected: failed += 1; print(f'FAIL {label} k={k} t={threads}: dump differs')
                elif label == 'stash' and not info['stash']: failed += 1; print(f'FAIL {label} k={k}: nothing was stashed')
                elif label == 'reseeded' and info['stash'] > info['distinct']//64: failed += 1; print(f'FAIL {label} k={k}: {info["stash"]} keys still stashed')
                elif [a.split('\t')[-1] for a in answers] != [s[1] for s in sample]: failed += 1; print(f'FAIL {label} k={k} t={threads}: query differs')
            if len(digests) != 1: failed += 1; print(f'FAIL {label} k={k}: index depends on the thread count')
            print(f'{label} k={k} ok ({info["layout"]}, {info.get("stash", 0)} of {info["distinct"]} keys stashed)', flush=True)
print('stash: all ok' if not failed else f'stash: {failed} FAILED')
sys.exit(1 if failed else 0)
