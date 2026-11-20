#!/usr/bin/env python3
# adt_identity.py IN.adt OUT.adt: an existing ADT with the root identity properties an iPhone's device tree has
# and ours lacked (MobileGestalt answered SerialNumber "", no ModelNumber / RegionInfo; tools such as 3uTools then
# show "None"). The values are synthetic on purpose (not a real Apple serial): serial-number SWEET7T00001,
# mlb-serial-number SWEET7TMLB0000001, model-number MQA82 (the iPhone X retail model number, a product code),
# region-info LL/A. Existing properties of the same name are replaced.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt

PROPS = {
    'serial-number': b'SWEET7T00001\0',
    'mlb-serial-number': b'SWEET7TMLB0000001\0',
    'model-number': b'MQA82\0',
    'region-info': b'LL/A\0',
}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    buf = open(src, 'rb').read()
    root, end = adt.parse(buf)
    assert adt.serialize(root) == buf[:end], 'ADT does not round-trip'
    root.props = [p for p in root.props if p[0] not in PROPS]
    root.props += [(k, 0, v) for k, v in PROPS.items()]
    out = adt.serialize(root)
    open(dst, 'wb').write(out)
    print('%s: %d bytes (+%d), root: %s' % (dst, len(out), len(out) - end, ', '.join(PROPS)))


if __name__ == '__main__':
    main()
