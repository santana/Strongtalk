#!/usr/bin/env python3
"""
Equivalence gate for the generated interpreter/primitives dumps.

The raw dumps are NOT byte-deterministic across runs: absolute addresses
embedded in the code vary with the code traffic-buffer base, external-symbol
positions (E8/relocs into the dylib), and out-of-buffer heap structures
(AArch64 absolute literals). Relocation, however, shifts EVERY field belonging
to a given address class by ONE constant amount (the class's base delta between
the two runs).

The gate groups the differing runs of contiguous bytes (over the deterministic
core = min(lenA,lenB) - tail_margin) by the value delta of the 8/4/2-byte
window starting at the run, and checks:
  * |lenA - lenB| is a multiple of the 20-byte absolute-stub size (or zero) and
    no larger than the tail margin,
  * every differing run is <= 8 bytes long and falls into one of a SMALL number
    of distinct delta buckets (pure relocation yields <= 3: buffer base, dylib
    slide, heap slide).

A pure relocation passes with every run bucketed; a reordered/added/changed
instruction sequence produces many distinct or oversized runs and fails.

Usage: cmp_dump.py [--tail-margin N] fileA fileB
"""
import sys
from collections import Counter

STUB = 20
MAX_BUCKETS = 3


def le(d, o, n):
    return int.from_bytes(d[o:o + n], 'little')


def runs_of(diffset):
    runs = []
    for i in sorted(diffset):
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    return runs


def span_win(lo, hi):
    span = hi - lo + 1
    if span <= 2:
        return 2
    if span <= 4:
        return 4
    if span <= 8:
        return 8
    return None


def match(a, b, margin):
    if len(a) == len(b):
        delta = 0
    else:
        delta = max(len(a), len(b)) - min(len(a), len(b))
        if delta % STUB != 0:
            sys.exit(f'FAIL: lengths {len(a)} vs {len(b)} not a multiple of the {STUB}-byte stub size')
        if delta > margin:
            sys.exit(f'FAIL: length delta {delta} exceeds tail margin {margin}')
    core = min(len(a), len(b)) - margin
    if core <= 0:
        sys.exit('FAIL: files too short for the tail margin')

    diffset = {i for i in range(core) if a[i] != b[i]}
    if not diffset:
        print(f'OK: deterministic cores identical ({core} bytes); length delta {delta}')
        return

    buckets = Counter()
    oversized = []
    for lo, hi in runs_of(diffset):
        w = span_win(lo, hi)
        if w is None:
            oversized.append((lo, hi))
            continue
        if lo + w > len(b):
            oversized.append((lo, hi))
            continue
        d = (le(b, lo, w) - le(a, lo, w)) & ((1 << (8 * w)) - 1)
        buckets[d] += 1

    if oversized:
        sys.exit(f'FAIL: {len(oversized)} differing runs longer than 8 bytes (sizes {sorted(set(h - l + 1 for l, h in oversized))}); length/structure change')
    if len(buckets) > MAX_BUCKETS:
        sys.exit(f'FAIL: {len(buckets)} distinct address-delta buckets (max {MAX_BUCKETS}); more than relocation can explain')
    desc = ', '.join(f'{k & 0xFFFFFFFF:#x} x{c}' for k, c in buckets.most_common())
    print(f'OK: cores equivalent ({core} bytes); {len(buckets)} address classes {{{desc}}}; length delta {delta}')


def main():
    margin = 512
    args = sys.argv[1:]
    if args and args[0] == '--tail-margin':
        margin = int(args[1])
        args = args[2:]
    if len(args) != 2:
        sys.exit('usage: cmp_dump.py [--tail-margin N] fileA fileB')
    a = open(args[0], 'rb').read()
    b = open(args[1], 'rb').read()
    match(a, b, margin)


if __name__ == '__main__':
    main()