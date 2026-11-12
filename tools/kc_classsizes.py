#!/usr/bin/env python3
# kc_classsizes.py KERNEL [OTHER_KERNEL] [--kext BUNDLE ...]: C++ class instance sizes as registered with
# OSMetaClass::OSMetaClass(this, "Name", super, size) -- every BL to that constructor, the size taken from the
# last write to w3 before it, the name from the adrp/add that set x1. With two kernels: the classes both have and
# where the sizes differ (kexts lay out their own fields right after their superclass, so these must match).
# --kext: also list the classes defined in these kexts of the first file (a merged prelinked kernelcache).
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
import capstone
import kc_imports as k
import kc_symcheck as sc

CTOR = '__ZN11OSMetaClassC2EPKcPKS_j'


def cstr(d, segs, a):
    f = k.v2f(segs, a)
    return d[f:f + 200].split(b'\0')[0].decode('latin1') if f is not None else None


def scan(d, segs, ctor, ranges):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = False
    out = {}
    for lo, hi in ranges:
        f = k.v2f(segs, lo)
        code = d[f:f + (hi - lo)]
        for off in range(0, len(code) - 3, 4):
            ins = struct.unpack_from('<I', code, off)[0]
            if (ins & 0xfc000000) != 0x94000000:
                continue
            imm = ins & 0x3ffffff
            if imm & 0x2000000:
                imm -= 1 << 26
            if lo + off + imm * 4 not in ctor:
                continue
            start = max(0, off - 4 * 24)
            regs = {}
            for i in md.disasm(code[start:off], lo + start):
                ops = i.op_str.replace(' ', '').split(',')
                if i.mnemonic in ('mov', 'movz', 'orr') and ops[0] == 'w3' and ops[-1].startswith('#'):
                    regs['size'] = int(ops[-1][1:], 0)
                elif i.mnemonic == 'adrp' and ops[0] == 'x1':
                    regs['page'] = int(ops[1][1:], 0)
                elif i.mnemonic == 'add' and ops[0] == 'x1' and ops[1] == 'x1' and 'page' in regs:
                    regs['name'] = regs['page'] + int(ops[2][1:], 0)
                elif i.mnemonic == 'adr' and ops[0] == 'x1':
                    regs['name'] = int(ops[1][1:], 0)
            if 'name' in regs and 'size' in regs:
                n = cstr(d, segs, regs['name'])
                if n:
                    out[n] = regs['size']
    return out


def load(path, kexts=()):
    d, segs, st = k.load(path)
    tab = sc.symtab(d)
    ctor = {tab[CTOR]} if CTOR in tab else set()
    ctor |= {v for n, v in tab.items() if n == '__ZN11OSMetaClassC1EPKcPKS_j'}
    text = [(s[1], s[1] + s[4] if s[4] else s[1] + s[2]) for s in segs if s[0] in ('__TEXT_EXEC',)]
    sizes = scan(d, segs, ctor, text)
    ksizes = {}
    if kexts:
        info = k.prelink_info(d, segs)
        for b in kexts:
            ks = k.kext_segments(d, segs, info[b][0])
            # kext code calls the ctor through its stubs: accept any stub that jumps to it
            stubs = set(ctor)
            for sn, vm, vsz, sects in ks:
                for secname, saddr, ssize in sects:
                    if secname in ('__stubs', '__auth_stubs'):
                        for o in range(0, ssize, 12):
                            tgt = None
                            fo = k.v2f(segs, saddr + o)
                            i0, i1 = struct.unpack_from('<II', d, fo)
                            if (i0 & 0x9f000000) == 0x90000000 and (i1 & 0xffc00000) == 0xf9400000:
                                immlo, immhi = (i0 >> 29) & 3, (i0 >> 5) & 0x7ffff
                                v = (immhi << 2) | immlo
                                if v & (1 << 20):
                                    v -= 1 << 21
                                slot = ((saddr + o) & ~0xfff) + (v << 12) + ((i1 >> 10) & 0xfff) * 8
                                tgt = struct.unpack_from('<Q', d, k.v2f(segs, slot))[0]
                            if tgt in ctor:
                                stubs.add(saddr + o)
            rng = [(vm, vm + vsz) for sn, vm, vsz, sects in ks if sn == '__TEXT_EXEC' and vsz]
            ksizes[b] = scan(d, segs, stubs, rng)
    return sizes, ksizes


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    kexts = sys.argv[sys.argv.index('--kext') + 1:] if '--kext' in sys.argv else []
    a = [x for x in a if x not in kexts]
    A, KA = load(a[0], kexts)
    print("%s: %d classes" % (a[0], len(A)))
    if len(a) > 1:
        B, _ = load(a[1])
        print("%s: %d classes" % (a[1], len(B)))
        both = sorted(set(A) & set(B))
        diff = [(n, A[n], B[n]) for n in both if A[n] != B[n]]
        print("in both: %d; size differs: %d" % (len(both), len(diff)))
        for n, x, y in diff:
            print("  %-40s %6d %6d  (%+d)" % (n, x, y, y - x))
    for b, m in KA.items():
        print("== %s: %d classes" % (b, len(m)))
        for n, s in sorted(m.items()):
            print("  %-40s %6d" % (n, s))


if __name__ == '__main__':
    main()
