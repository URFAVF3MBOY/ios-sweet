import ctypes, sys
libc = ctypes.CDLL(None)
pid = int(sys.argv[1]); pat = sys.argv[2] if len(sys.argv) > 2 else ""
buf = ctypes.create_string_buffer(4096)
addr = 0; seen = set()
for _ in range(200000):
    r = libc.proc_pidinfo(pid, 8, ctypes.c_uint64(addr), buf, 4096)  # PROC_PIDREGIONPATHINFO
    if r <= 0: break
    raw = buf.raw
    # proc_regioninfo: 96 bytes; pri_address at 64? fields: protection..(u32*?) ; use offsets
    import struct
    pri_address, pri_size = struct.unpack_from("<QQ", raw, 80)
    # vnode_info_path: vnode_info (152 bytes) then path[1024], after region info 96
    path = raw[96+152:96+152+1024].split(b"\0",1)[0].decode(errors="replace")
    if path and pat in path and path not in seen:
        seen.add(path); print(path)
    if pri_size == 0: break
    addr = pri_address + pri_size
