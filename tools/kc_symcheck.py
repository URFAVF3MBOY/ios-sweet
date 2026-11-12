#!/usr/bin/env python3
# kc_symcheck.py KERNELCACHE OURKERNEL BUNDLE...: every kernel export the given prelinked kexts reach (calls via their
# stubs/GOT and 8-byte data pointers, resolved through the kernelcache's export symtab) -- present in our kernel's
# symbol table? Prints the missing ones. Step toward relinking Apple's USB device kexts against our kernel.
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
import kc_imports as k


def symtab(d):
    ncmds = struct.unpack_from('<I', d, 16)[0]
    o = 32
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', d, o)
        if cmd == 0x2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', d, o + 8)
            out = {}
            for i in range(nsyms):
                strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
                if val and (typ & 0x0e) == 0x0e or (typ & 0x0e) == 0x02 and val:
                    out[d[stroff + strx:stroff + strx + 300].split(b'\0')[0].decode('latin1')] = val
            return out
        o += cs
    return {}


def main():
    d, segs, st = k.load(sys.argv[1])
    kc = symtab(d)
    byaddr = {v: n for n, v in kc.items()}
    ours = symtab(open(sys.argv[2], 'rb').read())
    info = k.prelink_info(d, segs)
    used = {}
    for bid in sys.argv[3:]:
        for sname, vm, vsz, sects in k.kext_segments(d, segs, info[bid][0]):
            for secname, saddr, ssize in sects:
                if sname.startswith('__TEXT') and secname not in ('__got',):
                    continue
                f = k.v2f(segs, saddr)
                if f is None:
                    continue
                for off in range(0, ssize - 7, 8):
                    v = struct.unpack_from('<Q', d, f + off)[0]
                    if v in byaddr:
                        used.setdefault(byaddr[v], set()).add(bid.split('.')[-1])
    missing = sorted(n for n in used if n not in ours)
    print("kernel symbols used: %d; in our kernel: %d; MISSING: %d" % (len(used), len(used) - len(missing), len(missing)))
    for n in missing:
        print("  %s  (%s)" % (n, ','.join(sorted(used[n]))))


if __name__ == '__main__':
    main()
