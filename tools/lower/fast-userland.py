#!/usr/bin/env python3
"""rewrite-zeta-userland.py without the code proof (same arguments, same output and report).

Every PAC-shaped word in the image's instruction sections that pauth.lower can lower is lowered, except words the
image's own data-in-code table marks as data. Unlowerable PAC words are left for the kernel to emulate. The
same pass on the 27.2 dyld cache gave exactly the proven pass's numbers (34,315,608 edits); the proven tool
stays the check for anything that looks wrong.
"""
from collections import Counter
import importlib.util
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
spec = importlib.util.spec_from_file_location('zeta_rewriter', Path(__file__).with_name('rewrite-zeta-userland.py'))
rw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rw)


def fast_lower(data, info):
    dice = set()
    for offset, length, kind in info['dice']:
        dice.update(range(offset, offset + length, 4))
    edits, kept = [], Counter()
    for section in rw.instruction_sections(info):
        for at in range(section['offset'], section['offset'] + section['size'] - 3, 4):
            word = struct.unpack_from('<I', data, at)[0]
            try:
                lowered = rw.pauth.lower(word)
            except ValueError:
                kept['unlowerable'] += 1
                continue
            if not lowered:
                continue
            if at in dice:
                kept['data-in-code'] += 1
                continue
            name, ordinary = lowered
            edits.append(dict(va=hex(section['addr'] + at - section['offset']), fileoff=at, mnemonic=name,
                              evidence='fast', original_word=hex(word), ordinary_word=hex(ordinary)))
    return edits, dict(mode='fast-unproven', kept=dict(kept), edits_by_evidence={'fast': len(edits)})


rw.lower = fast_lower
if __name__ == '__main__':
    sys.exit(rw.main())
