#!/usr/bin/env python3
# kdk_funcsig.py KEXT_BINARY OUT.json: a fingerprint of every named function of a KDK kext (arm64e, chained
# fixups): the external symbols it calls (through __auth_stubs -> bound GOT slots), the local functions it calls
# (by name), the C strings it references, its instruction count. Used to find the same functions in a stripped
# kext of the same version (kcsig.py does the other side). Output: names and fingerprints only.
import json, re, struct, sys
import capstone


def main():
    path, out = sys.argv[1], sys.argv[2]
    d = open(path, 'rb').read()
    ncmds = struct.unpack_from('<I', d, 16)[0]
    o = 32
    segs, sects, fix, symtab = [], [], None, None
    for _ in range(ncmds):
        c, cs = struct.unpack_from('<II', d, o)
        if c == 0x19:
            name = d[o + 8:o + 24].split(b'\0')[0].decode()
            vm, vsz, fo, fs = struct.unpack_from('<QQQQ', d, o + 24)
            segs.append((name, vm, vsz, fo, fs))
            for i in range(struct.unpack_from('<I', d, o + 64)[0]):
                so = o + 72 + 80 * i
                sn = d[so:so + 16].split(b'\0')[0].decode()
                a, sz, off = struct.unpack_from('<QQI', d, so + 32)
                sects.append((name, sn, a, sz, off))
        elif c == 0x80000034:
            fix = struct.unpack_from('<II', d, o + 8)
        elif c == 0x2:
            symtab = struct.unpack_from('<IIII', d, o + 8)
        o += cs

    def v2f(a):
        for n, vm, vsz, fo, fs in segs:
            if vm <= a < vm + fs:
                return fo + a - vm
    syms = {}
    so, ns, stro, _ = symtab
    for i in range(ns):
        strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', d, so + 16 * i)
        n = d[stro + strx:stro + strx + 400].split(b'\0')[0].decode('latin1')
        if (typ & 0x0e) == 0x0e and val and n and not (typ & 0xe0):
            syms.setdefault(val, n)
    # binds: GOT slot vmaddr -> import name
    fo, fsz = fix
    ver, starts_off, imports_off, symbols_off, imports_count, imports_format, sym_fmt = struct.unpack_from('<7I', d, fo)
    imports = []
    for i in range(imports_count):
        if imports_format == 1:
            v = struct.unpack_from('<I', d, fo + imports_off + 4 * i)[0]
            no = v >> 9
        elif imports_format == 2:
            v = struct.unpack_from('<I', d, fo + imports_off + 8 * i)[0]
            no = v >> 9
        else:
            v = struct.unpack_from('<Q', d, fo + imports_off + 16 * i)[0]
            no = v >> 32
        imports.append(d[fo + symbols_off + no:fo + symbols_off + no + 400].split(b'\0')[0].decode('latin1'))
    binds = {}
    sio = fo + starts_off
    base = min(s[1] for s in segs)
    for si in range(struct.unpack_from('<I', d, sio)[0]):
        off = struct.unpack_from('<I', d, sio + 4 + 4 * si)[0]
        if not off:
            continue
        s = sio + off
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', d, s)
        stride = 4 if fmt in (7, 8) else 8
        seg_vm = base + seg_offset
        sfo = v2f(seg_vm)
        for p in range(page_count):
            start = struct.unpack_from('<H', d, s + 22 + 2 * p)[0]
            if start == 0xffff:
                continue
            loc = p * page_size + start
            while True:
                v = struct.unpack_from('<Q', d, sfo + loc)[0]
                if (v >> 62) & 1:
                    ordn = v & (0xffffff if fmt == 12 else 0xffff)
                    if ordn < len(imports):
                        binds[seg_vm + loc] = imports[ordn]
                nxt = (v >> 51) & 0x7ff
                if not nxt:
                    break
                loc += nxt * stride
    # stubs: stub address -> import name
    stubs = {}
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for sg, sn, a, sz, off in sects:
        if sn in ('__auth_stubs', '__stubs'):
            ins = list(md.disasm(d[off:off + sz], a))
            for i in range(len(ins) - 2):
                if ins[i].mnemonic == 'adrp' and ins[i + 1].mnemonic.startswith('ldr') or \
                        ins[i].mnemonic == 'adrp' and ins[i + 1].mnemonic == 'add':
                    page = int(ins[i].op_str.split('#')[-1], 0)
                    m = re.search(r'#(0x[0-9a-f]+|\d+)\]', ins[i + 1].op_str) or re.search(r'#(0x[0-9a-f]+|\d+)$', ins[i + 1].op_str)
                    if m:
                        slot = page + int(m.group(1), 0)
                        if slot in binds:
                            stubs[ins[i].address] = binds[slot]
    cstr = [(a, a + sz, off) for sg, sn, a, sz, off in sects if sn in ('__cstring', '__os_log')]

    def string_at(a):
        for lo, hi, off in cstr:
            if lo <= a < hi:
                return d[off + a - lo:off + a - lo + 120].split(b'\0')[0].decode('latin1')
    text = [(a, sz, off) for sg, sn, a, sz, off in sects if sn == '__text'][0]
    funcs = sorted(a for a in syms if text[0] <= a < text[0] + text[1])
    res = {}
    for i, fa in enumerate(funcs):
        end = funcs[i + 1] if i + 1 < len(funcs) else text[0] + text[1]
        code = d[text[2] + fa - text[0]:text[2] + end - text[0]]
        calls, strs, n, regs = [], [], 0, {}
        for ins in md.disasm(code, fa):
            n += 1
            m = ins.mnemonic
            if m in ('bl', 'b') and ins.op_str.startswith('#'):
                t = int(ins.op_str[1:], 0)
                if t in stubs:
                    calls.append(stubs[t])
                elif t in syms and not (fa <= t < end):
                    calls.append(syms[t])
            elif m == 'adrp':
                r, imm = ins.op_str.split(', #')
                regs[r] = int(imm, 0)
            elif m == 'add' and '#' in ins.op_str:
                ops = [x.strip() for x in ins.op_str.split(',')]
                if len(ops) == 3 and ops[1] in regs:
                    s = string_at(regs[ops[1]] + int(ops[2][1:], 0))
                    if s:
                        strs.append(s)
        res[syms[fa]] = {'n': n, 'calls': calls, 'strs': strs}
    json.dump(res, open(out, 'w'), indent=0)


if __name__ == '__main__':
    main()
