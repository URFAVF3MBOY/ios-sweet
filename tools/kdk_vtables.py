#!/usr/bin/env python3
# kdk_vtables.py KEXT_BINARY [CLASS...]: vtable slots of C++ classes in a KDK kext (MH_KEXT_BUNDLE with
# LC_DYLD_CHAINED_FIXUPS, arm64e): for each __ZTV<len><Class> (all of them if no class is given) print
# "Class slot symbol", the slot counted from the first virtual (vtable + 16). A slot's pointer is either a bind (an
# imported symbol) or a rebase (a local function: named from the symbol table). Text out only; meant to run where
# the KDK is (CI), so the binaries stay there.
import re, struct, sys


def main():
    path = sys.argv[1]
    want = set(sys.argv[2:])
    d = open(path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', d, 0)
    o = 32
    segs, fix, symtab = [], None, None
    for _ in range(ncmds):
        c, cs = struct.unpack_from('<II', d, o)
        if c == 0x19:
            name = d[o + 8:o + 24].split(b'\0')[0].decode()
            vm, vsz, fo, fs = struct.unpack_from('<QQQQ', d, o + 24)
            segs.append((name, vm, vsz, fo, fs))
        elif c == 0x80000034:
            fix = struct.unpack_from('<II', d, o + 8)
        elif c == 0x2:
            symtab = struct.unpack_from('<IIII', d, o + 8)
        o += cs
    base = min(s[1] for s in segs if s[0] != '__PAGEZERO')

    syms, byaddr = {}, {}
    so, ns, stro, _ = symtab
    for i in range(ns):
        strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', d, so + 16 * i)
        n = d[stro + strx:stro + strx + 400].split(b'\0')[0].decode('latin1')
        if (typ & 0x0e) == 0x0e and val:
            syms[n] = val
            byaddr.setdefault(val, n)

    # chained fixups: header, imports, starts
    fo, fsz = fix
    ver, starts_off, imports_off, symbols_off, imports_count, imports_format, sym_fmt = struct.unpack_from('<7I', d, fo)
    imports = []
    for i in range(imports_count):
        if imports_format == 1:
            v = struct.unpack_from('<I', d, fo + imports_off + 4 * i)[0]
            name_off = v >> 9
        elif imports_format == 2:
            v, add = struct.unpack_from('<Ii', d, fo + imports_off + 8 * i)
            name_off = v >> 9
        else:
            v, add = struct.unpack_from('<Qq', d, fo + imports_off + 16 * i)
            name_off = v >> 32
        imports.append(d[fo + symbols_off + name_off:fo + symbols_off + name_off + 400].split(b'\0')[0].decode('latin1'))
    sio = fo + starts_off
    seg_count = struct.unpack_from('<I', d, sio)[0]
    ptrs = {}                                   # vmaddr -> ('bind', name) / ('rebase', target vmaddr)
    for si in range(seg_count):
        off = struct.unpack_from('<I', d, sio + 4 + 4 * si)[0]
        if not off:
            continue
        s = sio + off
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', d, s)
        stride = 4 if fmt in (7, 8) else 8
        seg_vm = base + seg_offset
        seg_fo = [x[3] for x in segs if x[1] == seg_vm]
        seg_fo = seg_fo[0] if seg_fo else seg_offset
        for p in range(page_count):
            start = struct.unpack_from('<H', d, s + 22 + 2 * p)[0]
            if start == 0xffff:
                continue
            loc = p * page_size + start
            while True:
                v = struct.unpack_from('<Q', d, seg_fo + loc)[0]
                auth, bind = v >> 63, (v >> 62) & 1
                nxt = (v >> 51) & 0x7ff
                if bind:
                    ordinal = v & (0xffffff if fmt == 12 else 0xffff)
                    ptrs[seg_vm + loc] = ('bind', imports[ordinal] if ordinal < len(imports) else '?%d' % ordinal)
                else:
                    target = v & (0xffffffff if auth else 0x7ffffffffff)
                    if fmt in (7, 8) or auth or target < base:
                        target += base if target < base or fmt in (7, 8) else 0
                    ptrs[seg_vm + loc] = ('rebase', target)
                if not nxt:
                    break
                loc += nxt * stride

    vts = sorted((v, n) for n, v in syms.items() if n.startswith('__ZTV'))
    for i, (va, n) in enumerate(vts):
        m = re.match(r'__ZTV(\d+)(.*)', n)
        cls = m.group(2)[:int(m.group(1))] if m else n
        if want and cls not in want:
            continue
        end = vts[i + 1][0] if i + 1 < len(vts) else va + 0x2000
        slot, a = 0, va + 16
        while a < end:
            p = ptrs.get(a)
            if p is None:
                break
            name = p[1] if p[0] == 'bind' else byaddr.get(p[1], '0x%x' % p[1])
            print('%s %d %s' % (cls, slot, name))
            slot += 1
            a += 8


if __name__ == '__main__':
    main()
