#!/usr/bin/env python3
# kcbuild.py OURKERNEL KERNELCACHE OUT BUNDLE...: a prelinked kernel in the old merged format (the format the iOS 16
# iPhone X kernelcache uses and xnu's KLDBootstrap::readPrelinkedExtensions boots): our kernel + the given kexts
# from Apple's kernelcache, relinked at their final addresses (see kextlink.py for the method).
#
# Layout, as xnu's arm_vm_init expects it (comment block at the top of arm_vm_init.c):
#   __PRELINK_TEXT, __PLK_TEXT_EXEC, __PLK_DATA_CONST  below our __TEXT (between virt_base and __TEXT: the shim
#                                                      copies segments to phys_base + vmaddr - virt_base)
#   __PRELINK_DATA, __PRELINK_INFO                     where our empty ones are (after __BOOTDATA); __LINKINFO and
#                                                      __LINKEDIT move up behind them
# Each kext's own mach header (start of its __TEXT) gets its segment/section addresses rewritten. __PRELINK_INFO
# holds the XML plist: _PrelinkInfoDictionary = the com.apple.kpi.* kernel resources (codeless, version =
# osrelease) + the kexts' Info.plist entries with _PrelinkExecutableLoadAddr/SourceAddr/Size, _PrelinkKmodInfo,
# _PrelinkBundlePath. kern.kernel_slide is 0 on this boot, so no KASLR offsets are needed.
# Output files hold Apple code: keep them out of git (research/ is ignored).
import base64, re, struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
import capstone
import kc_imports as k
import kc_symcheck as sc

PAGE = 0x4000
# 0217: IOUSBDeviceFamily 2.0.0 in the iPhone X iOS 16.5 kernelcache (research/awdl/kc/kernelcache.arm64): the
# addresses SweetUSBDevice.cpp calls (found by tools/kcsig.py against the macOS 13.5 KDK, and through the iPhone's own
# AppleSynopsysOTGDevice imports; NOTES.md 2026-11-13). Written relinked into sweet_usbdev_abi, in this order.
ABI_EMPTY, ABI_FILLED = 0x59544d45495841, 0x44454c4c49465841
ABI_ORDER = ['vtable', 'meta', 'ctor', 'ready_for_host', 'complete_io', 'cr_input_setup', 'cr_process_setup',
             'cr_input_data', 'cr_reset', 'cr_stall', 'if_alt_count', 'if_pipe_count', 'if_pipe', 'pipe_dir']
IOS165_USBDEV = {
    'vtable': 0xfffffff006e10b80, 'meta': 0xfffffff007a95708, 'ctor': 0xfffffff005cf0598,
    'ready_for_host': 0xfffffff005cf25d8, 'complete_io': 0xfffffff005cf269c,
    'cr_input_setup': 0xfffffff005cea854, 'cr_process_setup': 0xfffffff005ceafb4,
    'cr_input_data': 0xfffffff005ceb4e4, 'cr_reset': 0xfffffff005cea3d0, 'cr_stall': 0xfffffff005cec060,
    'if_alt_count': 0xfffffff005ce49a4, 'if_pipe_count': 0xfffffff005ce49f0, 'if_pipe': 0xfffffff005ce4a40,
    'pipe_dir': 0xfffffff005ce89cc,
}
KPI = ['com.apple.kpi.bsd', 'com.apple.kpi.dsep', 'com.apple.kpi.iokit', 'com.apple.kpi.libkern',
       'com.apple.kpi.mach', 'com.apple.kpi.private', 'com.apple.kpi.unsupported']
KIND = {'__TEXT': '__PRELINK_TEXT', '__DATA_CONST': '__PLK_DATA_CONST', '__TEXT_EXEC': '__PLK_TEXT_EXEC',
        '__DATA': '__PRELINK_DATA'}


def seg_cmds(buf):
    ncmds = struct.unpack_from('<I', buf, 16)[0]
    o, out = 32, {}
    for _ in range(ncmds):
        c, cs = struct.unpack_from('<II', buf, o)
        if c == 0x19:
            out[buf[o + 8:o + 24].split(b'\0')[0].decode()] = o
        o += cs
    return out


def set_seg(buf, o, vm, vsz, fo, fs, sect_size=None):
    struct.pack_into('<QQQQ', buf, o + 24, vm, vsz, fo, fs)
    nsect = struct.unpack_from('<I', buf, o + 64)[0]
    for i in range(nsect):
        so = o + 72 + 80 * i
        struct.pack_into('<QQI', buf, so + 32, vm, vsz if sect_size is None else sect_size, fo)


def plist_xml(obj):
    def enc(v):
        if isinstance(v, bool):
            return '<true/>' if v else '<false/>'
        if isinstance(v, int):
            return '<integer size="64">0x%x</integer>' % (v & 0xffffffffffffffff)
        if isinstance(v, dict):
            return '<dict>' + ''.join('<key>%s</key>%s' % (esc(kk), enc(vv)) for kk, vv in v.items()) + '</dict>'
        if isinstance(v, list):
            return '<array>' + ''.join(enc(x) for x in v) + '</array>'
        return '<string>%s</string>' % esc(str(v))

    def esc(s):
        return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">' + enc(obj) + '</plist>\n')


def main():
    ourpath, kcpath, out = sys.argv[1:4]
    bundles = sys.argv[4:]
    kern = bytearray(open(ourpath, 'rb').read())
    cmds = seg_cmds(kern)

    def seg(name):
        vm, vsz, fo, fs = struct.unpack_from('<QQQQ', kern, cmds[name] + 24)
        return vm, vsz, fo, fs
    text_vm = seg('__TEXT')[0]
    virt_base = text_vm & ~(0x2000000 - 1)
    after_boot = seg('__BOOTDATA')[0] + seg('__BOOTDATA')[1]
    ours = sc.symtab(bytes(kern))
    osrelease = '22.6.0'

    d, segs, st = k.load(kcpath)
    kcsym = sc.symtab(d)
    byaddr = {}
    for n, v in kcsym.items():
        byaddr.setdefault(v, n)
    plist = {kd['CFBundleIdentifier']: kd for kd in k.prelink_plist(d, segs)['_PrelinkInfoDictionary']}

    # ---- placement: kind by kind, page offsets kept ----
    pieces = {kd: [] for kd in KIND.values()}
    for b in bundles:
        for sn, vm, vsz, sects in k.kext_segments(d, segs, plist[b]['_PrelinkExecutableLoadAddr']):
            if vsz and sn in KIND:
                pieces[KIND[sn]].append((b, sn, vm, vsz, sects))
    lens = {}
    rel = {}
    for kind, lst in pieces.items():
        cur = 0
        for b, sn, vm, vsz, sects in lst:
            off = (cur & ~0xfff) + (vm & 0xfff)
            if off < cur:
                off += 0x1000
            rel[(b, sn)] = off
            cur = off + vsz
        lens[kind] = (cur + PAGE - 1) & ~(PAGE - 1)
    # order as Apple's kernelcache: arm_vm_init maps the gap from the end of __PLK_DATA_CONST up to __TEXT RW/NX,
    # so __PLK_DATA_CONST must come last (with it before __PLK_TEXT_EXEC the kext code was non-executable)
    below = ['__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST']
    need = sum(lens[x] for x in below)
    base = {}
    cur = (text_vm - need - 0x100000) & ~(0x100000 - 1)        # 1 MB aligned start, ends below __TEXT
    assert cur >= virt_base, 'no room below __TEXT'
    for x in below:
        base[x] = cur
        cur += lens[x]
    assert cur <= text_vm
    base['__PRELINK_DATA'] = after_boot
    newva = {}
    for (b, sn), off in rel.items():
        newva[(b, sn)] = base[KIND[sn]] + off

    places = []
    for kind, lst in pieces.items():
        for b, sn, vm, vsz, sects in lst:
            places.append((b, sn, vm, vsz, sects, newva[(b, sn)]))

    def where(a):
        for b, sn, vm, vsz, sects, nv in places:
            if vm <= a < vm + vsz:
                return nv + (a - vm)
        return None
    blobs = {kind: bytearray(lens[kind]) for kind in KIND.values()}

    def put(addr, data):
        for kind, bv in base.items():
            if bv <= addr < bv + lens[kind]:
                blobs[kind][addr - bv:addr - bv + len(data)] = data
                return
        raise SystemExit('put outside %#x' % addr)

    def get(addr, n):
        for kind, bv in base.items():
            if bv <= addr < bv + lens[kind]:
                return bytes(blobs[kind][addr - bv:addr - bv + n])
        raise SystemExit('get outside %#x' % addr)
    for b, sn, vm, vsz, sects, nv in places:
        f = k.v2f(segs, vm)
        put(nv, d[f:f + vsz])

    # ---- code: ADRP ----
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    st_ = {'adrp': 0, 'unpaired': 0, 'rebase': 0, 'bind': 0, 'unknown': [], 'missing': []}
    for b, sn, vm, vsz, sects, nv in places:
        if sn != '__TEXT_EXEC':
            continue
        for secname, saddr, ssize in sects:
            if secname not in ('__text', '__stubs', '__auth_stubs'):
                continue
            new_s = nv + (saddr - vm)
            insns = list(md.disasm(get(new_s, ssize), saddr))
            for idx, ins in enumerate(insns):
                if ins.mnemonic != 'adrp':
                    continue
                reg = ins.op_str.split(',')[0].strip()
                page = ins.operands[1].imm & 0xffffffffffffffff
                target = None
                for j in insns[idx + 1:idx + 12]:
                    ops = j.op_str.replace(' ', '')
                    if j.mnemonic == 'add' and ops.split(',')[1] == reg and '#' in ops:
                        target = page + int(ops.split('#')[-1], 0)
                        break
                    if ('[%s,#' % reg) in ops or ('[%s]' % reg) in ops:
                        lo = int(ops.split('[%s,#' % reg)[1].split(']')[0].rstrip('!'), 0) if ('[%s,#' % reg) in ops else 0
                        target = page + lo
                        break
                    if ops.split(',')[0] == reg and j.mnemonic not in ('str', 'stp', 'cmp', 'tst'):
                        break
                if target is None:
                    hit = [p for p in places if p[2] < page + 0x1000 and page < p[2] + p[3]]
                    if len(hit) == 1:
                        target = max(page, hit[0][2])
                    else:
                        st_['unpaired'] += 1
                        continue
                nt = where(target)
                if nt is None:
                    st_['unpaired'] += 1
                    continue
                pc = new_s + (ins.address - saddr)
                delta = ((nt & ~0xfff) - (pc & ~0xfff)) >> 12
                assert -(1 << 20) <= delta < (1 << 20), hex(ins.address)
                w = struct.unpack_from('<I', get(pc, 4))[0]
                v = delta & 0x1fffff
                w = (w & 0x9f00001f) | ((v & 3) << 29) | ((v >> 2) << 5)
                put(pc, struct.pack('<I', w))
                st_['adrp'] += 1

    # ---- data pointers ----
    for b, sn, vm, vsz, sects, nv in places:
        if sn == '__TEXT_EXEC':
            continue
        for secname, saddr, ssize in sects:
            if sn == '__TEXT' and secname != '__const':
                continue
            na = nv + (saddr - vm)
            # every 4-byte offset: packed structs (kmod_info is #pragma pack(4): address/start/stop at +0x9c/+0xb4/
            # +0xbc) hold pointers that are not 8-byte aligned. Only exact hits (our kexts, named exports) count.
            o = 0
            while o <= ssize - 8:
                v = struct.unpack_from('<Q', get(na + o, 8))[0]
                o += 4
                if v >> 32 != 0xfffffff0:
                    continue
                if v >> 32 != 0xfffffff0:
                    continue
                at = na + o - 4
                t = where(v)
                if t is not None:
                    put(at, struct.pack('<Q', t))
                    st_['rebase'] += 1
                    o += 4
                elif v in byaddr and byaddr[v] in ours:
                    put(at, struct.pack('<Q', ours[byaddr[v]]))
                    st_['bind'] += 1
                    o += 4
                elif v in byaddr:
                    st_['missing'].append(byaddr[v])
                elif not (at & 7):
                    st_['unknown'].append(hex(v))

    # ---- each kext's mach header: segment and section addresses ----
    for b in bundles:
        hdr = newva[(b, '__TEXT')]
        h = bytearray(get(hdr, 0x4000 if True else 0))
        assert struct.unpack_from('<I', h, 0)[0] == 0xfeedfacf, b
        ncmds = struct.unpack_from('<I', h, 16)[0]
        o = 32
        for _ in range(ncmds):
            c, cs = struct.unpack_from('<II', h, o)
            if c == 0x19:
                vm, vsz, fo, fs = struct.unpack_from('<QQQQ', h, o + 24)
                if vsz:
                    nvm = where(vm)
                    if nvm is not None:
                        struct.pack_into('<Q', h, o + 24, nvm)
                    nsect = struct.unpack_from('<I', h, o + 64)[0]
                    for i in range(nsect):
                        so = o + 72 + 80 * i
                        a = struct.unpack_from('<Q', h, so + 32)[0]
                        na = where(a) if a else None
                        if na is not None:
                            struct.pack_into('<Q', h, so + 32, na)
            o += cs
        put(hdr, bytes(h[:32 + struct.unpack_from('<I', h, 20)[0]]))

    # ---- kalloc_type_view_parse walks __PRELINK_TEXT,__text: a kext header right at the section start, the next
    # one at align64(previous __TEXT filesize). Page offsets are kept for ADRP+ADD, so instead each kext's
    # __TEXT size (in its own header) is set to the distance to the next kext (zero padding, read-only text).
    texts = sorted((newva[(b, '__TEXT')], b) for b in bundles)
    prelink_sect = (texts[0][0], 0)
    for i, (va, b) in enumerate(texts):
        h = bytearray(get(va, 32 + struct.unpack_from('<I', get(va, 32), 20)[0]))
        ncmds = struct.unpack_from('<I', h, 16)[0]
        o = 32
        for _ in range(ncmds):
            c, cs = struct.unpack_from('<II', h, o)
            if c == 0x19 and h[o + 8:o + 24].split(b'\0')[0] == b'__TEXT':
                fs = struct.unpack_from('<Q', h, o + 48)[0]
                if i + 1 < len(texts):
                    dist = texts[i + 1][0] - va
                    assert dist % 64 == 0 and dist >= fs, (b, hex(dist), hex(fs))
                    fs = dist
                else:
                    fs = (fs + 63) & ~63
                struct.pack_into('<Q', h, o + 32, fs)        # vmsize
                struct.pack_into('<Q', h, o + 48, fs)        # filesize
                end = va + fs
            o += cs
        put(va, bytes(h))
    prelink_sect = (texts[0][0], end - texts[0][0])

    # ---- __PRELINK_INFO ----
    infos = []
    for kpi in KPI:
        e = dict(plist[kpi])
        for kk in list(e):
            if kk.startswith('_Prelink'):
                del e[kk]
        e['CFBundleVersion'] = osrelease
        e['OSKernelResource'] = True
        infos.append(e)
    for b in bundles:
        e = dict(plist[b])
        la = newva[(b, '__TEXT')]
        e['_PrelinkExecutableLoadAddr'] = la
        e['_PrelinkExecutableSourceAddr'] = la
        if e.get('_PrelinkKmodInfo'):
            e['_PrelinkKmodInfo'] = where(e['_PrelinkKmodInfo'])
        infos.append(e)
    xml = plist_xml({'_PrelinkInfoDictionary': infos}).encode() + b'\0'
    info_len = (len(xml) + PAGE - 1) & ~(PAGE - 1)
    base['__PRELINK_INFO'] = base['__PRELINK_DATA'] + lens['__PRELINK_DATA']
    shift_to = base['__PRELINK_INFO'] + info_len

    # ---- new kernel file: data appended, load commands updated ----
    fileend = (len(kern) + PAGE - 1) & ~(PAGE - 1)
    kern += bytes(fileend - len(kern))
    for kind in ['__PRELINK_TEXT', '__PLK_DATA_CONST', '__PLK_TEXT_EXEC', '__PRELINK_DATA']:
        fo = len(kern)
        kern += blobs[kind]
        set_seg(kern, cmds[kind], base[kind], lens[kind], fo, lens[kind])
        if kind == '__PRELINK_TEXT':                 # the section starts at the first kext header (see above)
            so = cmds[kind] + 72
            struct.pack_into('<QQI', kern, so + 32, prelink_sect[0], prelink_sect[1], fo + prelink_sect[0] - base[kind])
    fo = len(kern)
    kern += xml + bytes(info_len - len(xml))
    set_seg(kern, cmds['__PRELINK_INFO'], base['__PRELINK_INFO'], info_len, fo, info_len, sect_size=len(xml))
    # unused prelink segments sit at the end of the new data, size 0
    for kind in ['__PLK_LLVM_COV', '__PLK_LINKEDIT']:
        set_seg(kern, cmds[kind], shift_to, 0, fo + info_len, 0)
    old_li = seg('__LINKINFO')[0]
    delta = shift_to - old_li
    for kind in ['__LINKINFO', '__LINKEDIT', '__CTF']:
        if kind in cmds:
            vm, vsz, fo2, fs2 = struct.unpack_from('<QQQQ', kern, cmds[kind] + 24)
            struct.pack_into('<Q', kern, cmds[kind] + 24, vm + delta)
            nsect = struct.unpack_from('<I', kern, cmds[kind] + 64)[0]
            for i in range(nsect):
                so = cmds[kind] + 72 + 80 * i
                a = struct.unpack_from('<Q', kern, so + 32)[0]
                struct.pack_into('<Q', kern, so + 32, a + delta)
    # ---- 0217: the backend's table of IOUSBDeviceFamily addresses (sweet_usbdev_abi in our __DATA) ----
    abi_note = 'no sweet_usbdev_abi in this kernel'
    if '_sweet_usbdev_abi' in ours and 'com.apple.iokit.IOUSBDeviceFamily' in bundles:
        av = ours['_sweet_usbdev_abi']
        cmds2 = seg_cmds(kern)
        fo_abi = None
        for nm, o2 in cmds2.items():
            vm2, vs2, fo2, fs2 = struct.unpack_from('<QQQQ', kern, o2 + 24)
            if vm2 <= av < vm2 + fs2:
                fo_abi = fo2 + (av - vm2)
        if fo_abi is not None and struct.unpack_from('<Q', kern, fo_abi)[0] == ABI_EMPTY:
            vals = [ABI_FILLED] + [where(IOS165_USBDEV[n]) for n in ABI_ORDER]
            if None in vals:
                raise SystemExit('sweet_usbdev_abi: an address is outside the relinked kexts')
            struct.pack_into('<%dQ' % len(vals), kern, fo_abi, *vals)
            abi_note = 'sweet_usbdev_abi filled (%d addresses)' % (len(vals) - 1)
        else:
            abi_note = 'sweet_usbdev_abi not in the expected state: left alone'
    open(out, 'wb').write(kern)
    print(abi_note)

    print('kexts: %s' % ', '.join(bundles))
    for kind in ['__PRELINK_TEXT', '__PLK_DATA_CONST', '__PLK_TEXT_EXEC', '__PRELINK_DATA', '__PRELINK_INFO']:
        print('  %-17s %#x +%#x' % (kind, base[kind], lens.get(kind, info_len)))
    print('  __LINKINFO/__LINKEDIT moved by %#x' % delta)
    print('ADRP %d (unpaired %d), rebase %d, bind %d, missing %d, unknown %d' % (
        st_['adrp'], st_['unpaired'], st_['rebase'], st_['bind'], len(st_['missing']), len(st_['unknown'])))
    for b in bundles:
        print('  %s at %#x, kmod_info %#x' % (b, newva[(b, '__TEXT')], where(plist[b].get('_PrelinkKmodInfo') or 0) or 0))
    print('plist %d bytes, %d entries; wrote %s (%d bytes)' % (len(xml), len(infos), out, len(kern)))


if __name__ == '__main__':
    main()
