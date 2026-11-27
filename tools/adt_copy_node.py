#!/usr/bin/env python3
# adt_copy_node.py FROM.adt NODE IN.adt OUT.adt --as NEWNAME: copies the root child NODE of FROM.adt (an Apple
# device tree, e.g. research/ios27/ipsw-small/d73ap.adt) into IN.adt as root child NEWNAME, without its
# AAPL,phandle and function-* properties (they point at nodes this ADT does not have).
# Used for /sweet-backlight = the iPhone 14 Pro's /backlight: kernel 0033 copies its properties onto
# AppleARMBacklight, because iOS 27's CoreBrightness reads tables from there (bright-dot-mitigation, aml-table,
# EDR*, rtplc-*) and backboardd crashes on the missing ones (boot 28n: SIGSEGV, NULL float array).
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt


def load(path):
    buf = open(path, 'rb').read()
    root, end = adt.parse(buf)
    assert adt.serialize(root) == buf[:end], path + ': ADT does not round-trip'
    return root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('node')
    ap.add_argument('dst_in')
    ap.add_argument('dst_out')
    ap.add_argument('--as', dest='newname', required=True)
    a = ap.parse_args()
    src = next(c for c in load(a.src).children if c.name == a.node)
    root = load(a.dst_in)
    assert not any(c.name == a.newname for c in root.children), 'ADT already has /' + a.newname
    node = adt.Node()
    node.props.append(('name', 0, a.newname.encode() + b'\0'))
    for k, f, v in src.props:
        if k in ('name', 'AAPL,phandle') or k.startswith('function-'):
            continue
        node.props.append((k, f, v))
    root.children.append(node)
    open(a.dst_out, 'wb').write(adt.serialize(root))
    print('%s: /%s = %s/%s, %d properties' % (a.dst_out, a.newname, os.path.basename(a.src), a.node,
                                              len(node.props) - 1))


if __name__ == '__main__':
    main()
