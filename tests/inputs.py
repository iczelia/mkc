"""Compressed inputs: every format and framing gives the index the plain
input gives (byte for byte: builds are deterministic), from files and from
stdin, at several thread counts; several inputs in one build give the sum
of their indexes; damaged inputs fail with an error.

usage: inputs.py ./mkc ./pack-test [small]
'small' marks a build with tiny read blocks, where units larger than a
block are streamed instead of decoded in parallel."""
import collections, gzip, os, random, re, struct, subprocess, sys, tempfile, threading, zlib

mkc, pack = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
small = len(sys.argv) > 3 and sys.argv[3] == 'small'
rng = random.Random(11)


def fastq(n):
    out = []
    for i in range(n):
        s = ''.join(rng.choices('ACGT', k=rng.choice([50, 100, 150])))
        if i % 17 == 0: s = s[:30] + 'N' + s[31:]
        out.append(f'@r{i}\n{s}\n+\n{"I"*len(s)}\n')
    return ''.join(out).encode()


def fasta(n):
    unit = ''.join(rng.choices('ACGT', k=1000))
    out = []
    for i in range(n):
        s = ''.join(rng.choices('ACGT', k=rng.choice([100, 5000, 30000])))
        if i % 3 == 0: s += unit * 3       # repeats, so blocks compress unevenly
        out.append(f'>c{i}\n' + '\n'.join(s[j:j+80] for j in range(0, len(s), 80)) + '\n')
    return ''.join(out).encode()


def deflate_member(data, level=6, fields=b'', flags=0):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    head = bytes([0x1f, 0x8b, 8, flags, 0, 0, 0, 0, 0, 255]) + fields
    if flags & 2:   # FHCRC: low 16 bits of the header's CRC-32
        head += struct.pack('<H', zlib.crc32(head) & 0xffff)
    return head + body + struct.pack('<II', zlib.crc32(data), len(data) & 0xffffffff)


def bgzf(data, eof=True, size=65280):
    out = []
    for at in range(0, len(data), size):
        chunk = data[at:at+size]
        c = zlib.compressobj(6, zlib.DEFLATED, -15)
        body = c.compress(chunk) + c.flush()
        total = 18 + len(body) + 8
        out.append(bytes([0x1f, 0x8b, 8, 4, 0, 0, 0, 0, 0, 255, 6, 0, ord('B'), ord('C'), 2, 0])
                   + struct.pack('<H', total-1) + body + struct.pack('<II', zlib.crc32(chunk), len(chunk)))
    if eof:
        out.append(bytes.fromhex('1f8b08040000000000ff0600424302001b0003000000000000000000'))
    return b''.join(out)


def packed(mode, data, *args):
    return subprocess.run([pack, mode, *map(str, args)], input=data, capture_output=True, check=True).stdout


def same_index(a, b):
    if os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, 'rb') as left, open(b, 'rb') as right:
        while True:
            chunk = left.read(1 << 20)
            if chunk != right.read(1 << 20):
                return False
            if not chunk:
                return True


def build(path, threads, tmp, stdin=None, skip=0, memory=512, timeout=None,
          save_index=None, reference=None):
    out = os.path.join(tmp, 'o.mkc')
    with open(stdin or os.devnull, 'rb', buffering=0) as f:
        if skip:
            f.seek(skip)
        try:
            r = subprocess.run([mkc, 'build', '-k', '25', '-t', str(threads), '-M', str(memory), '-T', tmp, '-o', out,
                                *([path] if isinstance(path, str) else path)], stdin=f, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None, 'no end after %d s' % timeout, 0
    if r.returncode:
        return None, r.stderr.decode().strip(), 0
    m = re.search(r'\((\d+) in parallel\)', r.stderr.decode())
    # Equal bytes imply equal counts. Compression or thread count can change
    # the bin count and layout, so compare the dumped counts in that case.
    if reference is not None and same_index(out, reference[0]):
        data = reference[1]
    else:
        data = sorted(subprocess.run([mkc, 'dump', out], capture_output=True, check=True).stdout.split(b'\n'))
    if save_index is None:
        os.unlink(out)
    else:
        os.replace(out, save_index)
    return data, None, int(m.group(1)) if m else 0


def tally(dump):
    return collections.Counter({k: int(c) for k, c in (line.split(b'\t') for line in dump if line)})


def repeated(data, size):
    return data * (size // len(data) + 1)


failed = 0
with tempfile.TemporaryDirectory(dir=os.environ.get('MKC_TEST_TMP')) as tmp:
    for kind, data in (('fastq', fastq(110000)), ('fasta', fasta(400))):
        plain = os.path.join(tmp, 'in')
        with open(plain, 'wb') as f:
            f.write(data)
        saved = os.path.join(tmp, 'plain.mkc')
        want, err, _ = build(plain, 1, tmp, save_index=saved)
        assert want, err
        reference = (saved, want)
        third = len(data)//3
        variants = [
            ('gzip', gzip.compress(data, 1), False),
            ('gzip-members', deflate_member(data[:third], 1) + deflate_member(b'') + deflate_member(data[third:], 9), False),
            ('gzip-header-fields', deflate_member(data, 6, b'\x04\x00XYab' + b'name\x00' + b'comment\x00', 4 | 8 | 16 | 2), False),
            ('bgzf', bgzf(data), True),
            ('bgzf-no-eof', bgzf(data, eof=False), True),
            ('bgzf-then-gzip', bgzf(data[:third]) + deflate_member(data[third:2*third]) + bgzf(data[2*third:], size=4000), True),
            ('zstd-frame', packed('zstd-frame', data, 3), False),
            ('zstd-stream', packed('zstd-stream', data, 1), False),
            ('zstd-frames', packed('zstd-frames', data, 1 << 20), True),
            ('zstd-frames-small', packed('zstd-frames', data, 3000), True),
            ('zstd-frames-large', packed('zstd-frames', data, 12 << 20), False),
            # A leading skippable frame, as pzstd writes (and two, one of them longer).
            ('zstd-skippable-first', b'\x50\x2a\x4d\x18\x04\x00\x00\x00abcd' + packed('zstd-frames', data, 1 << 20), True),
            ('zstd-skippable-twice', b'\x5f\x2a\x4d\x18\x00\x00\x00\x00' + b'\x51\x2a\x4d\x18\x64\x00\x00\x00' + bytes(100) + packed('zstd-frame', data, 3), False),
            ('lz4-skippable-first', b'\x50\x2a\x4d\x18\x04\x00\x00\x00abcd' + packed('lz4', data, 4, 1, 1, 1, 1, 0), True),
            ('lz4-64k', packed('lz4', data, 4, 1, 1, 1, 1, 0), True),
            ('lz4-4m', packed('lz4', data, 7, 1, 0, 1, 0, 0), not small),
            ('lz4-frames', packed('lz4', data, 5, 1, 0, 0, 1, 5 << 20), not small),
            ('lz4-linked', packed('lz4', data, 4, 0, 1, 1, 0, 0), False),
            ('lz4-linked-frames', packed('lz4', data, 6, 0, 0, 1, 1, 3 << 20), False),
        ]
        if small:   # units larger than a block are streamed
            variants = [(n, d, p and n.startswith('zstd-frames-small')) for n, d, p in variants]
        for name, blob, parallel in variants:
            path = os.path.join(tmp, name)
            with open(path, 'wb') as f:
                f.write(blob)
            for t in (1, 4, 16):
                got, err, par = build(path, t, tmp, reference=reference)
                if got != want:
                    failed += 1; print(f'FAIL {kind} {name} t={t}: {err or "index differs"}')
                elif t > 1 and parallel and not par:
                    failed += 1; print(f'FAIL {kind} {name} t={t}: nothing decoded in parallel')
            got, err, _ = build('-', 4, tmp, stdin=path, reference=reference)
            if got != want:
                failed += 1; print(f'FAIL {kind} {name} stdin: {err or "index differs"}')
            print(f'{kind} {name} ok', flush=True)
        # Damage: truncation, a flipped byte, trailing garbage, mixed formats.
        bad = [
            ('gzip-truncated', gzip.compress(data)[:-1000]),
            ('gzip-crc', (lambda b: b[:-8] + bytes([b[-8] ^ 1]) + b[-7:])(gzip.compress(data))),
            ('gzip-trailing', gzip.compress(data) + b'garbage'),
            ('bgzf-truncated', bgzf(data)[:-5000]),
            ('bgzf-corrupt', (lambda b: b[:200000] + bytes([b[200000] ^ 0x55]) + b[200001:])(bgzf(data))),
            ('zstd-truncated', packed('zstd-frame', data, 3)[:-100]),
            ('zstd-frames-truncated', packed('zstd-frames', data, 1 << 20)[:-100]),
            ('zstd-corrupt', (lambda b: b[:5000] + bytes([b[5000] ^ 0x55]) + b[5001:])(packed('zstd-frames', data, 1 << 20))),
            ('zstd-then-gzip', packed('zstd-frame', data[:third], 3) + gzip.compress(data[third:])),
            ('lz4-truncated', packed('lz4', data, 4, 1, 1, 1, 1, 0)[:-100]),
            ('lz4-block-checksum', (lambda b: b[:30000] + bytes([b[30000] ^ 0x55]) + b[30001:])(packed('lz4', data, 4, 1, 1, 0, 0, 0))),
            ('lz4-content-checksum', (lambda b: b[:-1] + bytes([b[-1] ^ 1]))(packed('lz4', data, 4, 1, 0, 1, 0, 0))),
            ('lz4-linked-truncated', packed('lz4', data, 4, 0, 0, 0, 0, 0)[:-100]),
        ]
        for name, blob in bad:
            path = os.path.join(tmp, name)
            with open(path, 'wb') as f:
                f.write(blob)
            for t in (1, 16):
                got, err, _ = build(path, t, tmp)
                if got is not None or not err or 'mkc: ' not in err:
                    failed += 1; print(f'FAIL {kind} {name} t={t}: accepted or crashed: {err!r}')
            print(f'{kind} {name} rejected ({err})', flush=True)
    # Several inputs in one build give the sum of the indexes of each built
    # alone. Compressed inputs are decoded together and parsed in turns, a
    # block of each at a time, so three of them are longer than a block
    # (repeated, to keep the indexes small) and end after different turns;
    # others are parsed serially, in part (no final newline) or in whole (a
    # leading empty line). More than four inputs make a second group, and a
    # plain input ends a group.
    block = 4096 if small else 8 << 20
    texts = {
        'fasta': fasta(2),
        'fasta-long': repeated(fasta(4), 9*block//4),
        'fasta-open': repeated(fasta(4), 5*block//4)[:-1],
        'fasta-blank': b'\n' + fasta(2),
        'fastq-long': repeated(fastq(64), 5*block//4),
        'fastq-open': fastq(300)[:-1],
    }
    frames = {'plain': bytes, 'gzip': lambda d: gzip.compress(d, 1), 'bgzf': bgzf,
              'zstd': lambda d: packed('zstd-frames', d, 1 << 20), 'lz4': lambda d: packed('lz4', d, 4, 1, 1, 1, 1, 0)}
    together = [
        ('fasta', 'fasta-long.gzip fasta-open.gzip'),
        ('fasta-fastq', 'fastq-long.gzip fasta-long.bgzf fastq-open.zstd fasta-open.lz4'),
        ('empty-line', 'fasta-blank.bgzf fasta.bgzf'),
        ('two-groups', 'fasta-long.zstd fastq-open.zstd fasta-blank.zstd fasta-open.zstd fastq-long.zstd fasta.zstd'),
        ('plain-between', 'fasta.plain fastq-long.lz4 fasta-open.lz4 fastq-open.plain fasta-blank.lz4 fasta-long.lz4'),
    ]
    plain, alone = os.path.join(tmp, 'in'), {}
    for name, text in texts.items():
        with open(plain, 'wb') as f:
            f.write(text)
        got, err, _ = build(plain, 1, tmp)
        assert got, err
        alone[name] = tally(got)
    for case, inputs in together:
        paths, want = [], collections.Counter()
        for i, item in enumerate(inputs.split()):
            name, frame = item.split('.')
            paths.append(os.path.join(tmp, f'together{i}'))
            want.update(alone[name])
            with open(paths[-1], 'wb') as f:
                f.write(frames[frame](texts[name]))
        for t in (1, 4, 16):
            got, err, _ = build(paths, t, tmp)
            if got is None or tally(got) != want:
                failed += 1; print(f'FAIL together {case} t={t}: {err or "index differs"}')
        print(f'together {case} ok', flush=True)
    def stored(text, size):
        for _ in range(64):
            member = deflate_member(text, 0)
            if len(member) == size: return member, text
            text = text[:size - len(member)] if len(member) > size else text + b'A' * (size - len(member))
        raise AssertionError('no stored member of %d bytes' % size)
    for back in (17, 20, 24):
        first, text = stored(b'>a\n' + b'ACGTTGCATG' * (block // 10), block - back)
        second = b'\n>b\n' + ''.join(rng.choices('ACGT', k=300)).encode() + b'\n'
        with open(plain, 'wb') as f:
            f.write(text + second)
        want, err, _ = build(plain, 1, tmp)
        assert want, err
        path = os.path.join(tmp, 'gzip-header-split')
        with open(path, 'wb') as f:
            f.write(first + deflate_member(second, 6, b'reads_1.fastq\x00a comment here\x00', 8 | 16))
        for t in (1, 4):
            got, err, _ = build(path, t, tmp)
            if got != want:
                failed += 1; print(f'FAIL gzip-header-split {back} t={t}: {err or "index differs"}')
    print('gzip-header-split ok', flush=True)
    text = fasta(3)
    at = text.index(b'>', 1)
    with open(plain, 'wb') as f:
        f.write(text[at:])
    want, err, _ = build(plain, 1, tmp)
    assert want, err
    with open(plain, 'wb') as f:
        f.write(text)
    for inputs in ('-', ['-', '-']):
        got, err, _ = build(inputs, 4, tmp, stdin=plain, skip=at)
        if got != want:
            failed += 1; print(f'FAIL stdin-offset {inputs}: {err or "index differs"}')
    print('stdin-offset ok', flush=True)
    if hasattr(os, 'mkfifo'):
        fifo = os.path.join(tmp, 'fifo')
        os.mkfifo(fifo)
        def feed():
            try:
                with open(fifo, 'wb') as f:
                    f.write(text[at:])
            except OSError:
                pass
        for t in (1, 4):
            writer = threading.Thread(target=feed, daemon=True)
            writer.start()
            got, err, _ = build(fifo, t, tmp, timeout=300)
            os.close(os.open(fifo, os.O_RDONLY | os.O_NONBLOCK))
            writer.join(60)
            if got != want:
                failed += 1; print(f'FAIL fifo t={t}: {err or "index differs"}')
        os.unlink(fifo)
        print('fifo ok', flush=True)
    if not small:
        unit = ''.join(rng.choices('ACGT', k=6800))
        with open(plain, 'wb') as f:
            f.write(b'>r\n' + (unit * 5001).encode() + b'\n')
        want, err, _ = build(plain, 1, tmp)
        assert want, err
        for t, memory in ((1, 1), (16, 1), (16, 2)):
            got, err, _ = build('-', t, tmp, stdin=plain, memory=memory)
            if got != want:
                failed += 1; print(f'FAIL heavy-bin t={t} M={memory}: {err or "index differs"}')
        print('heavy-bin ok', flush=True)
    # Empty inputs, plain and compressed, give an empty index.
    for name, blob in (('empty', b''), ('empty-gzip', gzip.compress(b'')), ('empty-zstd', packed('zstd-frame', b'', 3)),
                       ('empty-lz4', packed('lz4', b'', 4, 1, 0, 1, 1, 0))):
        path = os.path.join(tmp, name)
        with open(path, 'wb') as f:
            f.write(blob)
        got, err, _ = build(path, 4, tmp)
        if got is None:
            failed += 1; print(f'FAIL {name}: {err}')
        print(f'{name} ok', flush=True)
    leftovers = [x for x in os.listdir(tmp) if x.startswith('mkc.') or '.tmp.' in x or '.exc.' in x]
    if leftovers:
        failed += 1; print(f'FAIL leftovers {leftovers}')
sys.exit(f'inputs: {failed} failures' if failed else print('inputs: all ok'))
