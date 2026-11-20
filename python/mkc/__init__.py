"""Exact k-mer counting: build indexes with mkc and query them from Python.

    import mkc
    mkc.build(["genome.fa.gz"], "genome.mkc", k=31, threads=16)
    with mkc.Index("genome.mkc") as idx:
        idx["ACGTACGTACGTACGTACGTACGTACGTACG"]     # count, either orientation
        idx.count_keys(numpy_uint64_array)           # batch, GIL released
        idx.count_sequence(read)                     # every k-mer window
        for kmer, count in idx: ...
"""
import os
import shutil
import subprocess

from ._mkc import Index, decode, encode

__all__ = ["Index", "build", "encode", "decode", "binary"]


def binary():
    """Path of the mkc command-line tool: $MKC_BINARY, the copy installed
    with this package, or mkc on PATH."""
    env = os.environ.get("MKC_BINARY")
    if env:
        return env
    bundled = os.path.join(os.path.dirname(__file__), "bin", "mkc")
    if os.access(bundled, os.X_OK):
        return bundled
    found = shutil.which("mkc")
    if found:
        return found
    raise FileNotFoundError("mkc binary not found; set MKC_BINARY or reinstall the package")


def build(inputs, output, k=31, *, threads=None, memory_mib=None, minimizer=None,
          tmpdir=None, sync=True, quiet=True):
    """Count canonical k-mers of FASTA/FASTQ files (plain or gzip) into
    output and return an open Index. Raises CalledProcessError on failure,
    with the tool's message in .stderr."""
    if isinstance(inputs, (str, bytes, os.PathLike)):
        inputs = [inputs]
    cmd = [binary(), "build", "-k", str(k)]
    if threads is not None:
        cmd += ["-t", str(threads)]
    if memory_mib is not None:
        cmd += ["-M", str(memory_mib)]
    if minimizer is not None:
        cmd += ["-m", str(minimizer)]
    if tmpdir is not None:
        cmd += ["-T", os.fspath(tmpdir)]
    if not sync:
        cmd.append("--no-sync")
    cmd += ["-o", os.fspath(output), "--", *map(os.fspath, inputs)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE if quiet else None, text=True)
    return Index(output)
