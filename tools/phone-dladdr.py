#!/var/jb/usr/bin/python3
"""Name user addresses on the phone: one hex address per line on stdin -> address, image, offset in
the image, nearest exported symbol + distance. Shared-cache addresses are the same in every process
of a boot (one slide), so this process can name another's (the kernel profiler's PROF pcs for
backboardd). Addresses in a process's own dylibs outside the cache (libredwood-gl, libOSMesa) are
not mapped here: those print "?" and are named on the PC from the file (PROFREG gives the base).
  python3 phone-dladdr.py < pcs.txt
"""
import ctypes, sys


class DlInfo(ctypes.Structure):
    _fields_ = [('dli_fname', ctypes.c_char_p), ('dli_fbase', ctypes.c_void_p),
                ('dli_sname', ctypes.c_char_p), ('dli_saddr', ctypes.c_void_p)]


lib = ctypes.CDLL(None)
lib.dladdr.argtypes = [ctypes.c_void_p, ctypes.POINTER(DlInfo)]
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    a = int(line, 16)
    info = DlInfo()
    if lib.dladdr(ctypes.c_void_p(a), ctypes.byref(info)) == 0 or not info.dli_fname:
        print(f'{a:#x} ? ? ?')
        continue
    image = info.dli_fname.decode(errors='replace').split('/')[-1]
    off = a - (info.dli_fbase or 0)
    name = info.dli_sname.decode(errors='replace') if info.dli_sname else '?'
    dist = a - (info.dli_saddr or a)
    print(f'{a:#x} {image} +{off:#x} {name}+{dist:#x}')
