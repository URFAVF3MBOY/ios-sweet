#!/usr/bin/env python3
"""Fast PAC lowering plan for the dyld shared cache (no code proof).

Writes a work directory in the same layout as prove-zeta-dyld-cache.py (summary.json + one .npz of edits per
image), so rewrite-zeta-dyld-cache.py applies it, re-signs every file and refills the corecrypto FIPS HMACs.

Instead of proving every word is code, it lowers every PAC-shaped word inside each image's instruction sections
(S_ATTR_PURE_INSTRUCTIONS / S_ATTR_SOME_INSTRUCTIONS). arm64e keeps jump tables and constants in other sections,
so a data word that looks like a PAC instruction inside __text is unlikely, but not proven impossible: the
proven pass (prove-zeta-dyld-cache.py) is the check to run later.

Stub islands (executable bytes outside every image's sections) only get their `ldr x16, [x17]; braa x16, x17`
pairs lowered, as zeta does.

  fast-dyld-cache.py --cache .../dyld_shared_cache_arm64e --work DIR
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys

import numpy as np

import zeta_dyld_cache as dc
import zeta_userland_pauth as pauth

LDR_X16_X17 = 0xf9400230


def candidates(words):
    """Indices of words that may be non-hint PAC instructions (pauth.lower decides)."""
    w = words
    mask = (w & 0xffff0000) == 0xdac10000                       # PAC*/AUT*/XPAC* (data-processing, 1 source)
    mask |= (w & 0xffe0fc00) == 0x9ac03000                      # PACGA
    mask |= (w & 0xfe9ff800) == 0xd61f0800                      # BRAA/BRAB/BLRAA/BLRAB (+Z forms)
    mask |= (w & 0xfe9ff800) == 0xd71f0800
    mask |= (w == 0xd65f0bff) | (w == 0xd65f0fff)               # RETAA/RETAB
    mask |= (w & 0xff200400) == 0xf8200400                      # LDRAA/LDRAB
    return np.nonzero(mask)[0]


def lower_range(data, offset, size, base_address, counts, refused):
    """[(address, file offset, original, ordinary, 0)] for the PAC words in data[offset:offset+size]."""
    size -= size % 4
    words = np.frombuffer(data, dtype='<u4', count=size // 4, offset=offset)
    rows = []
    for i in candidates(words):
        word = int(words[i])
        try:
            found = pauth.lower(word)
        except ValueError:
            refused[hex(word)] += 1
            continue
        if found is None:
            continue
        name, ordinary = found
        counts[name] += 1
        rows.append((base_address + 4 * int(i), offset + 4 * int(i), word, ordinary, 0))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    args = parser.parse_args(argv)
    args.work.mkdir(parents=True, exist_ok=False)
    cache = dc.Cache(args.cache)
    counts, refused = Counter(), Counter()
    summary = dict(schema='zeta-dyld-cache-proof-v1', cache=str(args.cache), mode='fast-unproven', images={})
    covered = []
    for n, (address, path) in enumerate(cache.images):
        image = dc.Image(cache, address, path)
        for s in image.sections:
            if s['size']:
                covered.append((s['addr'], s['addr'] + s['size']))
        sections = image.instruction_sections()
        rows, file_index = [], None
        for s in sections:
            file_index = s['file']
            rows += lower_range(cache.files[s['file']].data, s['offset'], s['size'], s['addr'], counts, refused)
        name = f'image{n:05d}'
        edits = np.array(rows, dtype=np.uint64).reshape(-1, 5)
        if len(edits):
            np.savez_compressed(args.work / f'{name}.npz', edits=edits)
        summary['images'][path] = dict(file=file_index, edit_count=len(edits), result_file=f'{name}.npz')
        if n % 500 == 0:
            print(f'{n}/{len(cache.images)} images, {sum(counts.values())} edits', file=sys.stderr, flush=True)
    # Stub islands: executable bytes no image section covers.
    covered.sort()
    islands = []
    for start, size, index, fileoff in cache.executable_regions():
        cursor, end = start, start + size
        gaps = []
        for b, e in covered:
            if e <= cursor or b >= end:
                continue
            if b > cursor:
                gaps.append((cursor, b))
            cursor = max(cursor, e)
        if cursor < end:
            gaps.append((cursor, end))
        for b, e in gaps:
            b, e = (b + 3) & ~3, e & ~3
            if e - b < 8:
                continue
            off = fileoff + (b - start)
            words = np.frombuffer(cache.files[index].data, dtype='<u4', count=(e - b) // 4, offset=off)
            rows = []
            for i in np.nonzero(words[1:] != 0)[0] + 1:
                word = int(words[i])
                if int(words[i - 1]) != LDR_X16_X17:
                    continue
                try:
                    found = pauth.lower(word)
                except ValueError:
                    continue
                if found and found[0] in ('braa', 'brab', 'braaz', 'brabz'):
                    counts['island_' + found[0]] += 1
                    rows.append((b + 4 * int(i), off + 4 * int(i), word, found[1], 0))
            if rows:
                name = f'island{len(islands):05d}'
                np.savez_compressed(args.work / f'{name}.npz', edits=np.array(rows, dtype=np.uint64))
                islands.append(dict(file=index, address=hex(b), size=e - b, edit_count=len(rows), result_file=f'{name}.npz'))
    summary['islands'] = islands
    summary['totals'] = dict(edits=sum(counts.values()), refused=sum(refused.values()))
    summary['instruction_counts'] = dict(counts)
    summary['refused_words'] = dict(refused.most_common(50))
    (args.work / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n')
    print(json.dumps(dict(summary['totals'], islands=len(islands)), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
