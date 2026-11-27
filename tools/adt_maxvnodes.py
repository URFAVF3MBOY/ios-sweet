#!/usr/bin/env python3
# adt_maxvnodes.py IN.adt OUT.adt [--n 65536]: sets /defaults kern.maxvnodes (u32). The SWEET config caps the
# vnode table at CONFIG_VNODES (1024) unless the ADT names a size (bsd_startupearly reads it through
# PE_get_default, not clipped). iOS 26/27 open far more files than iOS 16 did: ios-guacamole's iOS 26 boot filled
# the table ("vnode: table is full, 1024 desired") once the daemons ran.
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt

KEY = 'kern.maxvnodes'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--n', type=int, default=65536)
    a = ap.parse_args()
    buf = open(a.src, 'rb').read()
    root, end = adt.parse(buf)
    assert adt.serialize(root) == buf[:end], 'ADT does not round-trip'
    defaults = next(c for c in root.children if c.name == 'defaults')
    value = struct.pack('<I', a.n)
    if defaults.prop(KEY) is None:
        defaults.props.append((KEY, 0, value))
    else:
        defaults.props = [(k, f, value if k == KEY else v) for k, f, v in defaults.props]
    open(a.dst, 'wb').write(adt.serialize(root))
    print('%s: /defaults %s = %d' % (a.dst, KEY, a.n))


if __name__ == '__main__':
    main()
