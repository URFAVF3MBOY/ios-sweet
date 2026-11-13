#!/usr/bin/env python3
# kcsig.py KERNELCACHE BUNDLE MACOS_SIG.json [NAME...]: name functions of a stripped kext in an old merged prelinked
# kernelcache by matching them with the same kext's named functions from a KDK (kdk_funcsig.py output).
# Fingerprint = kernel exports called (in order), C strings referenced; functions start at call targets, vtable
# entries and constructors. Pass 1 matches unique fingerprints; pass 2 spreads along the call graph (the i-th local
# call of a matched pair is the same function on both sides when both call lists have the same length).
# Prints "address name" for the requested names (all matches if none given).
import json, re, struct, sys
from collections import defaultdict
sys.path.insert(0, __import__('os').path.dirname(__file__))
import capstone
import kc_imports as k
import kc_symcheck as sc


def norm(name):
    return name.lstrip('_')


def main():
    kcpath, bundle, sigpath = sys.argv[1:4]
    want = sys.argv[4:]
    mac = json.load(open(sigpath))
    d, segs, st = k.load(kcpath)
    ksym = sc.symtab(d)
    byaddr = {}
    for n, v in ksym.items():
        byaddr.setdefault(v, n)
    info = k.prelink_info(d, segs)
    ks = k.kext_segments(d, segs, info[bundle][0])
    text = [(a, sz) for sn, vm, vsz, sects in ks for secname, a, sz in sects if secname == '__text'][0]
    stubsec = [(a, sz) for sn, vm, vsz, sects in ks for secname, a, sz in sects if secname in ('__stubs', '__auth_stubs')]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def stub_target(t):
        f = k.v2f(segs, t)
        i0, i1 = struct.unpack_from('<II', d, f)
        if (i0 & 0x9f000000) != 0x90000000 or (i1 & 0xffc00000) != 0xf9400000:
            return None
        v = (((i0 >> 5) & 0x7ffff) << 2) | ((i0 >> 29) & 3)
        if v & (1 << 20):
            v -= 1 << 21
        slot = (t & ~0xfff) + (v << 12) + ((i1 >> 10) & 0xfff) * 8
        return struct.unpack_from('<Q', d, k.v2f(segs, slot))[0]
    stubs = {}
    for a, sz in stubsec:
        for o in range(0, sz, 12):
            t = stub_target(a + o)
            if t is not None:
                stubs[a + o] = t
    kext_ranges = [(vm, vm + vsz) for sn, vm, vsz, sects in ks if vsz]

    def mine(a):
        return any(x <= a < y for x, y in kext_ranges)
    fo = k.v2f(segs, text[0])
    code = d[fo:fo + text[1]]
    insns = list(md.disasm(code, text[0]))
    starts = {text[0]}
    for ins in insns:
        if ins.mnemonic == 'bl':
            t = int(ins.op_str[1:], 0) & 0xffffffffffffffff
            if text[0] <= t < text[0] + text[1]:
                starts.add(t)
    # functions only called from other kexts have no caller here: a new function also starts right after a
    # return / unconditional branch (skipping alignment padding)
    for i, ins in enumerate(insns[:-1]):
        if ins.mnemonic in ('ret', 'b', 'br', 'retab', 'braa', 'brab') and not ins.mnemonic.startswith('b.'):
            j = i + 1
            while j < len(insns) and insns[j].mnemonic in ('nop', 'udf', '.byte'):
                j += 1
            if j < len(insns):
                nx = insns[j]
                if (nx.mnemonic == 'stp' and nx.op_str.endswith('!') and '[sp' in nx.op_str) or                         (nx.mnemonic == 'sub' and nx.op_str.startswith('sp, sp')) or nx.mnemonic == 'pacibsp':
                    starts.add(nx.address)
    for sn, vm, vsz, sects in ks:
        for secname, a, sz in sects:
            if sn.startswith('__DATA'):
                f2 = k.v2f(segs, a)
                for o in range(0, sz - 7, 8):
                    v = struct.unpack_from('<Q', d, f2 + o)[0]
                    if text[0] <= v < text[0] + text[1]:
                        starts.add(v)
    starts = sorted(starts)
    cstr = [(a, a + sz) for sn, vm, vsz, sects in ks for secname, a, sz in sects if secname in ('__cstring', '__os_log')]

    def string_at(a):
        for lo, hi in cstr:
            if lo <= a < hi:
                f3 = k.v2f(segs, a)
                return d[f3:f3 + 120].split(b'\0')[0].decode('latin1')
    idx = {ins.address: i for i, ins in enumerate(insns)}
    ios = {}
    for si, fa in enumerate(starts):
        end = starts[si + 1] if si + 1 < len(starts) else text[0] + text[1]
        calls, local, strs, regs, n, allc = [], [], [], {}, 0, []
        i = idx.get(fa)
        while i is not None and i < len(insns) and insns[i].address < end:
            ins = insns[i]
            n += 1
            m = ins.mnemonic
            if m in ('bl', 'b') and ins.op_str.startswith('#'):
                t = int(ins.op_str[1:], 0) & 0xffffffffffffffff
                if t in stubs:
                    tt = stubs[t]
                    if tt in byaddr:
                        calls.append(norm(byaddr[tt]))
                        allc.append(norm(byaddr[tt]))
                    else:
                        local.append(tt)
                        allc.append(tt)
                elif t in byaddr:
                    calls.append(norm(byaddr[t]))
                    allc.append(norm(byaddr[t]))
                elif m == 'bl' or not (fa <= t < end):
                    if text[0] <= t < text[0] + text[1] and not (fa <= t < end):
                        local.append(t)
                        allc.append(t)
            elif m == 'adrp':
                r, imm = ins.op_str.split(', #')
                regs[r] = int(imm, 0) & 0xffffffffffffffff
            elif m == 'add' and '#' in ins.op_str:
                ops = [x.strip() for x in ins.op_str.split(',')]
                if len(ops) == 3 and ops[1] in regs:
                    s = string_at(regs[ops[1]] + int(ops[2][1:], 0))
                    if s:
                        strs.append(s)
            i += 1
        ios[fa] = {'n': n, 'calls': calls, 'local': local, 'strs': strs, 'all': allc}

    # macOS side: split external (kernel) calls from local ones
    macnames = set(mac)
    kernel_names = set(ksym)
    macsig = {}
    for name, f in mac.items():
        ext = [norm(c) for c in f['calls'] if c in kernel_names]
        loc = [c for c in f['calls'] if c not in kernel_names]
        macsig[name] = {'calls': ext, 'local': loc, 'strs': f['strs'], 'n': f['n'],
                        'all': [norm(c) if c in kernel_names else c for c in f['calls']]}

    def key(f):
        return (tuple(f['calls']), tuple(f['strs']))
    mk, ik = defaultdict(list), defaultdict(list)
    for n_, f in macsig.items():
        if f['calls'] or f['strs']:
            mk[key(f)].append(n_)
    for a, f in ios.items():
        if f['calls'] or f['strs']:
            ik[key(f)].append(a)
    match = {}
    for kk, names in mk.items():
        if len(names) == 1 and len(ik.get(kk, [])) == 1:
            match[names[0]] = ik[kk][0]
    foreign = {}
    changed = True
    while changed:
        changed = False
        for name, a in list(match.items()):
            ml, il = macsig[name]['all'], ios[a]['all']
            if len(ml) == len(il) and all((type(x) is str and type(y) is str and x == y) or type(y) is int
                                          for x, y in zip(ml, il)):
                for x, y in zip(ml, il):
                    if type(y) is not int:
                        continue
                    if y in ios and x in macsig:
                        if x not in match and y not in match.values():
                            match[x] = y
                            changed = True
                    elif x not in macsig:
                        foreign.setdefault(x, set()).add(y)
    print('# %d of %d named macOS functions matched (%d iOS functions found)' % (len(match), len(mac), len(ios)))
    names = want or sorted(match)
    for n_ in names:
        if n_ in match:
            print('%s %s' % (hex(match[n_]), n_))
        elif n_ in foreign:
            print('%s %s  (called from this kext)' % (' '.join(hex(x) for x in sorted(foreign[n_])), n_))
        else:
            print('- %s' % n_)


if __name__ == '__main__':
    main()
