#!/usr/bin/env python3
# kextlink.py KERNELCACHE OURKERNEL OUT.bin BUNDLE...: relink kexts out of an old-style merged prelinked arm64
# kernelcache (iOS 16 iPhone X) into one blob for our kernel.
#
# Layout: every segment of every kext (TEXT_EXEC first, then TEXT, DATA_CONST, DATA), each placed at a blob offset
# with the same offset inside a 4 KB page as in the kernelcache, so the low 12 bits of ADRP+ADD/LDR pairs stay
# right and only the ADRP page has to change.
# Code: each ADRP in the kexts' __text/__stubs is paired with the ADD/LDR/STR that uses its register; when the
# target lies in another place after relinking the ADRP immediate is rewritten. BL/B/ADR/LDR-literal that leave
# their segment are reported (not expected: calls go through __stubs + __got).
# Data: every 8-byte word of DATA/DATA_CONST (and TEXT __const) that holds a kernelcache address becomes
#   REBASE  (points into one of these kexts: value = blob offset), or
#   BIND    (a named kernel export: value = our kernel's unslid address of the same name);
# anything else that looks like a kernelcache address is reported and left alone.
#
# OUT.bin: "SWKX" v1 header, segment table, relocation table, the bytes. OUT.json: per kext the segment offsets,
# __mod_init_func / __mod_term_func entries (blob offsets), kmod_info, bundle id/version and IOKitPersonalities
# (plain JSON: the kernel side turns it into an OSDictionary for gIOCatalogue).
import json, struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
import capstone
import kc_imports as k
import kc_symcheck as sc

ORDER = ['__TEXT_EXEC', '__TEXT', '__DATA_CONST', '__DATA']
REBASE, BIND = 1, 2


def main():
    kcpath, ourpath, out = sys.argv[1:4]
    bundles = sys.argv[4:]
    d, segs, st = k.load(kcpath)
    kcsym = sc.symtab(d)
    byaddr = {}
    for n, v in kcsym.items():
        byaddr.setdefault(v, n)
    ours = sc.symtab(open(ourpath, 'rb').read())
    plist = {kd['CFBundleIdentifier']: kd for kd in k.prelink_plist(d, segs)['_PrelinkInfoDictionary']}

    # ---- layout ----
    pieces = []          # (bundle, segname, old_vm, size, sects)
    for b in bundles:
        for sn, vm, vsz, sects in k.kext_segments(d, segs, plist[b]['_PrelinkExecutableLoadAddr']):
            if vsz and sn in ORDER:
                pieces.append((b, sn, vm, vsz, sects))
    pieces.sort(key=lambda p: (ORDER.index(p[1]), bundles.index(p[0])))
    place, cur = [], 0
    for b, sn, vm, vsz, sects in pieces:
        if ORDER.index(sn) and place and place[-1][1] != sn:
            cur = (cur + 0x3fff) & ~0x3fff                      # new kind of segment: own 16 KB page
        off = (cur & ~0xfff) + (vm & 0xfff)
        if off < cur:
            off += 0x1000
        place.append((b, sn, vm, vsz, sects, off))
        cur = off + vsz
    total = (cur + 0x3fff) & ~0x3fff
    blob = bytearray(total)
    for b, sn, vm, vsz, sects, off in place:
        f = k.v2f(segs, vm)
        blob[off:off + vsz] = d[f:f + vsz]

    def where(a):
        for b, sn, vm, vsz, sects, off in place:
            if vm <= a < vm + vsz:
                return off + (a - vm), (b, sn)
        return None, None

    relocs, report = [], {'adrp_fixed': 0, 'adrp_same': 0, 'adrp_unpaired': [], 'leaving_branch': [],
                          'unknown_ptr': [], 'rebase': 0, 'bind': 0, 'bind_missing': []}

    # ---- code ----
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    for b, sn, vm, vsz, sects, off in place:
        if sn != '__TEXT_EXEC':
            continue
        for secname, saddr, ssize in sects:
            if secname not in ('__text', '__stubs', '__auth_stubs'):
                continue
            base_off = off + (saddr - vm)
            insns = list(md.disasm(bytes(blob[base_off:base_off + ssize]), saddr))
            for idx, ins in enumerate(insns):
                m = ins.mnemonic
                if m in ('bl', 'b') or (m.startswith('b.')) or m in ('cbz', 'cbnz', 'tbz', 'tbnz', 'adr') or \
                        (m.startswith('ldr') and '#0x' in ins.op_str and '[' not in ins.op_str):
                    tgt = (ins.operands[-1].imm & 0xffffffffffffffff) if ins.operands and ins.operands[-1].type == capstone.arm64.ARM64_OP_IMM else None
                    if tgt is not None and m != 'adr' or m == 'adr':
                        if tgt is not None and not (vm <= tgt < vm + vsz):
                            report['leaving_branch'].append('%s %#x %s %s' % (b, ins.address, m, ins.op_str))
                    continue
                if m != 'adrp':
                    continue
                reg = ins.op_str.split(',')[0].strip()
                page = ins.operands[1].imm & 0xffffffffffffffff
                target = None
                for j in insns[idx + 1: idx + 12]:
                    ops = j.op_str.replace(' ', '')
                    if j.mnemonic == 'add' and ops.split(',')[1] == reg and '#' in ops:
                        target = page + int(ops.split('#')[-1], 0)
                        break
                    if ('[%s,#' % reg) in ops or ('[%s]' % reg) in ops:
                        lo = 0
                        if '#' in ops.split('[%s' % reg)[1]:
                            lo = int(ops.split('[%s,#' % reg)[1].split(']')[0].rstrip('!'), 0)
                        target = page + lo
                        break
                    if ops.split(',')[0] == reg and j.mnemonic not in ('str', 'stp', 'cmp', 'tst'):
                        break                                   # register overwritten before use
                if target is None:
                    # use far away or not recognised: fine if the page touches exactly one relinked segment
                    hit = [(pvm, pvsz) for pb, psn, pvm, pvsz, ps, po in place
                           if pvm < page + 0x1000 and page < pvm + pvsz]
                    if len(hit) == 1:
                        target = max(page, hit[0][0])
                        report.setdefault('adrp_by_page', []).append('%s %#x' % (b, ins.address))
                    else:
                        report['adrp_unpaired'].append('%s %#x (page %#x, %d segments)' % (b, ins.address, page,
                                                                                           len(hit)))
                        continue
                new_t, _ = where(target)
                if new_t is None:
                    report['adrp_unpaired'].append('%s %#x -> %#x outside' % (b, ins.address, target))
                    continue
                pc_new = base_off + (ins.address - saddr)
                delta_pages = ((new_t & ~0xfff) - (pc_new & ~0xfff)) >> 12
                old_delta = ((page) - (ins.address & ~0xfff)) >> 12
                if delta_pages == old_delta:
                    report['adrp_same'] += 1
                    continue
                if not -(1 << 20) <= delta_pages < (1 << 20):
                    raise SystemExit('ADRP out of range at %#x' % ins.address)
                w = struct.unpack_from('<I', blob, pc_new)[0]
                v = delta_pages & 0x1fffff
                w = (w & 0x9f00001f) | ((v & 3) << 29) | ((v >> 2) << 5)
                struct.pack_into('<I', blob, pc_new, w)
                report['adrp_fixed'] += 1

    # ---- data ----
    for b, sn, vm, vsz, sects, off in place:
        if sn == '__TEXT_EXEC':
            continue
        for secname, saddr, ssize in sects:
            if sn == '__TEXT' and secname != '__const':
                continue
            so = off + (saddr - vm)
            for o in range(0, ssize - 7, 8):
                if (so + o) & 7:
                    continue
                v = struct.unpack_from('<Q', blob, so + o)[0]
                if v >> 32 != 0xfffffff0:
                    continue
                nt, _ = where(v)
                if nt is not None:
                    relocs.append((so + o, REBASE, nt))
                    struct.pack_into('<Q', blob, so + o, nt)
                    report['rebase'] += 1
                elif v in byaddr:
                    name = byaddr[v]
                    if name not in ours:
                        report['bind_missing'].append(name)
                        continue
                    relocs.append((so + o, BIND, ours[name]))
                    struct.pack_into('<Q', blob, so + o, ours[name])
                    report['bind'] += 1
                else:
                    report['unknown_ptr'].append('%s %s,%s +%#x = %#x' % (b, sn, secname, saddr + o - vm, v))

    # ---- manifest ----
    man = {'size': total, 'kexts': []}
    for b in bundles:
        kd = plist[b]
        e = {'bundle': b, 'version': kd.get('CFBundleVersion'), 'libraries': kd.get('OSBundleLibraries'),
             'personalities': kd.get('IOKitPersonalities') or {}, 'segments': {}, 'mod_init': [], 'mod_term': []}
        for pb, sn, vm, vsz, sects, off in place:
            if pb != b:
                continue
            e['segments'][sn] = [off, vsz]
            for secname, saddr, ssize in sects:
                so = off + (saddr - vm)
                if secname in ('__mod_init_func', '__mod_term_func'):
                    key = 'mod_init' if 'init' in secname else 'mod_term'
                    e[key] += [struct.unpack_from('<Q', blob, so + i)[0] for i in range(0, ssize, 8)]
        ki = [n for n, v in kcsym.items() if False]
        man['kexts'].append(e)

    hdr = struct.pack('<4sIIII', b'SWKX', 1, total, len(relocs), len(place))
    segtab = b''.join(struct.pack('<IIQ16s', off, vsz, vm, sn.encode()) for b, sn, vm, vsz, sects, off in place)
    reltab = b''.join(struct.pack('<IIQ', o, kind, val) for o, kind, val in relocs)
    open(out, 'wb').write(hdr + segtab + reltab + bytes(blob))
    json.dump(man, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1, default=str)

    print('blob %#x bytes, %d segments' % (total, len(place)))
    for b, sn, vm, vsz, sects, off in place:
        print('  %-40s %-13s old %#x size %#7x -> blob %#8x' % (b, sn, vm, vsz, off))
    print('ADRP rewritten %d, unchanged %d, unpaired %d' % (report['adrp_fixed'], report['adrp_same'],
                                                            len(report['adrp_unpaired'])))
    print('pointers: rebase %d, bind %d, bind missing %d, unknown %d' % (report['rebase'], report['bind'],
          len(report['bind_missing']), len(report['unknown_ptr'])))
    print('branches leaving their segment: %d' % len(report['leaving_branch']))
    for key in ('adrp_unpaired', 'leaving_branch', 'unknown_ptr', 'bind_missing'):
        for x in report[key][:8]:
            print('  %s: %s' % (key, x))
    for e in man['kexts']:
        print('  %s: mod_init %s, mod_term %s, personalities %d' % (e['bundle'], [hex(x) for x in e['mod_init']],
              [hex(x) for x in e['mod_term']], len(e['personalities'])))


if __name__ == '__main__':
    main()
