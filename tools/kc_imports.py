#!/usr/bin/env python3
# kc_imports.py KERNELCACHE BUNDLE-ID...: for kexts in an old-style merged prelinked arm64 kernelcache (iOS 16 iPhone X:
# research/awdl/kc/kernelcache.arm64), what their code and data reach outside themselves: BL targets and 8-byte data
# pointers that land in the kernel (named if the kernel's export symtab has them), in another kext (which), or nowhere.
# First step toward loading Apple's own kexts (IOUSBDeviceFamily, AppleUSBDeviceMux) into our source-built XNU.
import re, struct, sys
from collections import Counter, defaultdict


def load(path):
    d = open(path, 'rb').read()
    ncmds = struct.unpack_from('<I', d, 16)[0]
    o, segs, symtab = 32, [], None
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', d, o)
        if cmd == 0x19:
            name = d[o + 8:o + 24].split(b'\0')[0].decode()
            vm, vsz, foff, fsz = struct.unpack_from('<QQQQ', d, o + 24)
            segs.append((name, vm, vsz, foff, fsz))
        elif cmd == 0x2:
            symtab = struct.unpack_from('<IIII', d, o + 8)
        o += cs
    return d, segs, symtab


def v2f(segs, vm):
    for name, s, sz, foff, fsz in segs:
        if s <= vm < s + fsz:
            return foff + (vm - s)
    return None


def prelink_plist(d, segs):
    """__PRELINK_INFO as Python objects (Apple's XML with ID= / IDREF= sharing, which plistlib rejects)."""
    seg = [s for s in segs if s[0] == '__PRELINK_INFO'][0]
    xml = d[seg[3]:seg[3] + seg[4]].split(b'\0')[0].decode('latin1')
    tok = re.compile(r'<(/?)(dict|array|key|string|integer|data|true|false|date|real)([^>]*?)(/?)>')
    ids = {}
    pos = [0]

    def text_until(tag):
        e = xml.index('</%s>' % tag, pos[0])
        t = xml[pos[0]:e]
        pos[0] = e + len(tag) + 3
        return t

    def value():
        m = tok.search(xml, pos[0])
        pos[0] = m.end()
        closing, tag, attrs, selfclose = m.groups()
        idm = re.search(r'\bID="([^"]+)"', attrs)
        ref = re.search(r'\bIDREF="([^"]+)"', attrs)
        if ref:
            return ids[ref.group(1)]
        if tag == 'dict':
            v = {}
            if not selfclose:
                while True:
                    m2 = tok.search(xml, pos[0])
                    if m2.group(1) and m2.group(2) == 'dict':
                        pos[0] = m2.end()
                        break
                    pos[0] = m2.end()
                    key = text_until('key')
                    v[key] = value()
        elif tag == 'array':
            v = []
            if not selfclose:
                while True:
                    m2 = tok.search(xml, pos[0])
                    if m2.group(1) and m2.group(2) == 'array':
                        pos[0] = m2.end()
                        break
                    v.append(value())
        elif tag in ('true', 'false'):
            v = tag == 'true'
        elif selfclose:
            v = '' if tag != 'integer' else 0
        else:
            t = text_until(tag)
            v = int(t, 0) if tag == 'integer' else t
        if idm:
            ids[idm.group(1)] = v
        return v
    return value()


def prelink_info(d, segs):
    """bundle id -> (_PrelinkExecutableLoadAddr, _PrelinkExecutableSize), each top-level kext dict on its own."""
    out = {}
    for kd in prelink_plist(d, segs).get('_PrelinkInfoDictionary', []):
        if '_PrelinkExecutableLoadAddr' in kd:
            out[kd['CFBundleIdentifier']] = (kd['_PrelinkExecutableLoadAddr'], kd.get('_PrelinkExecutableSize', 0))
    return out


def kext_segments(d, segs, base):
    f = v2f(segs, base)
    ncmds = struct.unpack_from('<I', d, f + 16)[0]
    o, out = f + 32, []
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', d, o)
        if cmd == 0x19:
            name = d[o + 8:o + 24].split(b'\0')[0].decode()
            vm, vsz, foff, fsz = struct.unpack_from('<QQQQ', d, o + 24)
            nsect = struct.unpack_from('<I', d, o + 64)[0]
            sects = []
            for k in range(nsect):
                so = o + 72 + 80 * k
                sname = d[so:so + 16].split(b'\0')[0].decode()
                saddr, ssize = struct.unpack_from('<QQ', d, so + 32)
                sects.append((sname, saddr, ssize))
            out.append((name, vm, vsz, sects))
        o += cs
    return out


def main():
    d, segs, symtab = load(sys.argv[1])
    symoff, nsyms, stroff, _ = symtab
    sym = {}
    for i in range(nsyms):
        strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
        if val:
            sym[val] = d[stroff + strx:stroff + strx + 300].split(b'\0')[0].decode('latin1')
    info = prelink_info(d, segs)
    ranges = []
    for bid_, (base_, size_) in info.items():          # every segment of every kext (code/data live apart)
        for sn, vm, vsz, _ in kext_segments(d, segs, base_):
            if vsz:
                ranges.append((vm, vm + vsz, bid_))
    ranges.sort()
    text = [s for s in segs if s[0] == '__TEXT_EXEC'] or [s for s in segs if s[0] == '__TEXT']

    def owner(a):
        for s, e, k in ranges:
            if s <= a < e:
                return k
        return None

    def kernel_range(a):
        return any(s[1] <= a < s[1] + s[2] for s in segs if not s[0].startswith('__PRELINK'))

    for bid in sys.argv[2:]:
        base, size = info[bid]
        ks = kext_segments(d, segs, base)
        lo = min(s[1] for s in ks if s[2]); hi = max(s[1] + s[2] for s in ks if s[2])
        kext_ranges = [(s[1], s[1] + s[2]) for s in ks if s[2]]

        def mine(a):
            return any(x <= a < y for x, y in kext_ranges)

        def stub_target(t):
            f = v2f(segs, t)
            if f is None:
                return None
            i0, i1, i2 = struct.unpack_from('<III', d, f)
            if (i0 & 0x9f000000) != 0x90000000 or (i1 & 0xffc00000) != 0xf9400000 or (i2 & 0xfffffc1f) != 0xd61f0000:
                return None
            immlo, immhi = (i0 >> 29) & 3, (i0 >> 5) & 0x7ffff
            page = (t & ~0xfff) + (((immhi << 2) | immlo) << 12 if not (immhi >> 18) else ((((immhi << 2) | immlo) - (1 << 21)) << 12))
            slot = page + ((i1 >> 10) & 0xfff) * 8
            fs = v2f(segs, slot)
            return struct.unpack_from('<Q', d, fs)[0] if fs is not None else None
        calls, ptrs = Counter(), Counter()
        unnamed_calls, unnamed_ptrs, other = Counter(), Counter(), Counter()
        for sname, vm, vsz, sects in ks:
            for secname, saddr, ssize in sects:
                f = v2f(segs, saddr)
                if f is None:
                    continue
                if secname in ('__text', '__auth_stubs', '__stubs'):
                    for off in range(0, ssize, 4):
                        ins = struct.unpack_from('<I', d, f + off)[0]
                        if (ins & 0xfc000000) in (0x94000000, 0x14000000):   # BL / B
                            imm = ins & 0x3ffffff
                            if imm & 0x2000000:
                                imm -= 1 << 26
                            tgt = saddr + off + imm * 4
                            if mine(tgt):
                                st = stub_target(tgt)
                                if st is None:
                                    continue
                                tgt = st
                                if mine(tgt):
                                    continue
                            else:
                                st = stub_target(tgt)
                                if st is not None:
                                    tgt = st
                            ob = owner(tgt)
                            if ob:
                                other[ob] += 1
                            elif tgt in sym:
                                calls[sym[tgt]] += 1
                            elif kernel_range(tgt):
                                unnamed_calls[tgt] += 1
                else:
                    for off in range(0, ssize - 7, 8):
                        v = struct.unpack_from('<Q', d, f + off)[0]
                        if v >> 48 == 0xffff or (v >> 32) == 0xfffffff0:
                            a = v if v >> 48 == 0xffff else v
                            if mine(a):
                                continue
                            ob = owner(a)
                            if ob:
                                other[ob] += 1
                            elif a in sym:
                                ptrs[sym[a]] += 1
                            elif kernel_range(a):
                                unnamed_ptrs[a] += 1
        print('=== %s at %#x, code+data %#x..%#x' % (bid, base, lo, hi))
        print('  segments:', ', '.join('%s %#x' % (s[0], s[2]) for s in ks))
        print('  kernel exports called: %d distinct (%d sites); data pointers to exports: %d distinct' % (
            len(calls), sum(calls.values()), len(ptrs)))
        print('  UNNAMED kernel targets: %d call targets (%d sites), %d pointer targets' % (
            len(unnamed_calls), sum(unnamed_calls.values()), len(unnamed_ptrs)))
        print('  other kexts referenced:', dict(other.most_common(8)))
        print('  most used exports:', ', '.join('%s %d' % kv for kv in calls.most_common(12)))
        if unnamed_calls:
            print('  unnamed call targets (first 15):', ' '.join('%#x' % a for a, _ in unnamed_calls.most_common(15)))


if __name__ == '__main__':
    main()
