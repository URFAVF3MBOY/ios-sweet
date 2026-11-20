#!/usr/bin/env python3
"""bluetool_stub.py: answers bluetoothd where iOS's BlueTool would (OnePlus 7T, WCN3990).

bluetoothd asks the Mach service com.apple.BlueTool to run its boot script before it powers the controller
on, and waits for the reply: an XPC dictionary whose int64 "result" is 0 for success, with "firmware"
(string) and "manufacturer" (int64). Apple's BlueTool only knows Broadcom chips and only reads override
scripts (/etc/bluetool/<ProductHash hex>.<phase>.script) when MobileGestalt's ReleaseType is Vendor or
Internal, so on this phone it times out. The kernel (sweet_bt.c, sysctl kern.sweetbt=1) has already loaded
the Qualcomm firmware; this sends nothing to the chip and answers success, naming crbtfw11 and Qualcomm (29).

After HackiOS's userland/src/bluetool_stub.c (hakios-sm8150), in Python with ctypes because the phone has no
compiler: the two event handlers are hand-made global blocks.

Run by launchd as the job com.apple.BlueTool (MachServices com.apple.BlueTool), see tools/bluetool-stub.plist.
Every request is logged to stderr in full.
"""
import ctypes, os, sys

xpc = ctypes.CDLL('/usr/lib/system/libxpc.dylib')
dispatch = ctypes.CDLL('/usr/lib/system/libdispatch.dylib')
libc = ctypes.CDLL(None)
blocks_rt = ctypes.CDLL('/usr/lib/system/libsystem_blocks.dylib')

XPC_CONNECTION_MACH_SERVICE_LISTENER = 1
vp = ctypes.c_void_p

xpc.xpc_connection_create_mach_service.restype = vp
xpc.xpc_connection_create_mach_service.argtypes = [ctypes.c_char_p, vp, ctypes.c_uint64]
xpc.xpc_connection_set_event_handler.argtypes = [vp, vp]
xpc.xpc_connection_resume.argtypes = [vp]
xpc.xpc_get_type.restype = vp
xpc.xpc_get_type.argtypes = [vp]
xpc.xpc_copy_description.restype = vp
xpc.xpc_copy_description.argtypes = [vp]
xpc.xpc_dictionary_create_reply.restype = vp
xpc.xpc_dictionary_create_reply.argtypes = [vp]
xpc.xpc_dictionary_set_int64.argtypes = [vp, ctypes.c_char_p, ctypes.c_int64]
xpc.xpc_dictionary_set_string.argtypes = [vp, ctypes.c_char_p, ctypes.c_char_p]
xpc.xpc_connection_send_message.argtypes = [vp, vp]
xpc.xpc_connection_get_pid.restype = ctypes.c_int
xpc.xpc_connection_get_pid.argtypes = [vp]
xpc.xpc_retain.restype = vp
xpc.xpc_retain.argtypes = [vp]
libc.free.argtypes = [vp]

TYPE_CONNECTION = ctypes.addressof(ctypes.c_char.in_dll(xpc, '_xpc_type_connection'))
TYPE_DICTIONARY = ctypes.addressof(ctypes.c_char.in_dll(xpc, '_xpc_type_dictionary'))
GLOBAL_BLOCK_ISA = ctypes.addressof(ctypes.c_char.in_dll(blocks_rt, '_NSConcreteGlobalBlock'))


VERBOSE = bool(os.environ.get('BLUETOOL_STUB_LOG'))     # quiet unless set (sweet-media.sh starts it at every boot)


def log(*a, always=False):
    if VERBOSE or always:
        print('bluetool_stub:', *a, file=sys.stderr, flush=True)


def describe(obj):
    p = xpc.xpc_copy_description(obj)
    if not p:
        return '?'
    s = ctypes.string_at(p).decode(errors='replace')
    libc.free(p)
    return s


# A block literal: isa, flags, reserved, invoke, descriptor. Global blocks are never copied or freed.
class BlockDescriptor(ctypes.Structure):
    _fields_ = [('reserved', ctypes.c_ulong), ('size', ctypes.c_ulong)]


class Block(ctypes.Structure):
    _fields_ = [('isa', vp), ('flags', ctypes.c_int), ('reserved', ctypes.c_int), ('invoke', vp),
                ('descriptor', ctypes.POINTER(BlockDescriptor))]


INVOKE = ctypes.CFUNCTYPE(None, vp, vp)          # void (^)(xpc_object_t): invoke(block, object)
_keep = []


def make_block(fn):
    cb = INVOKE(lambda _blk, obj: fn(obj))
    desc = BlockDescriptor(0, ctypes.sizeof(Block))
    blk = Block(GLOBAL_BLOCK_ISA, 1 << 28, 0, ctypes.cast(cb, vp), ctypes.pointer(desc))   # BLOCK_IS_GLOBAL
    _keep.extend([cb, desc, blk])
    return ctypes.addressof(blk)


peers = {}


def on_message(peer, msg):
    if xpc.xpc_get_type(msg) != TYPE_DICTIONARY:
        log('connection event:', describe(msg))
        return
    log('request:', describe(msg))
    reply = xpc.xpc_dictionary_create_reply(msg)
    if not reply:
        log('the request wants no reply')
        return
    xpc.xpc_dictionary_set_int64(reply, b'result', 0)
    xpc.xpc_dictionary_set_string(reply, b'firmware', b'crbtfw11')
    xpc.xpc_dictionary_set_int64(reply, b'manufacturer', 29)
    xpc.xpc_connection_send_message(peer, reply)
    log('answered result 0, firmware crbtfw11, manufacturer 29')


def on_listener_event(obj):
    if xpc.xpc_get_type(obj) != TYPE_CONNECTION:
        log('listener event:', describe(obj))
        return
    peer = xpc.xpc_retain(obj)
    log('a client connected, pid', xpc.xpc_connection_get_pid(peer))
    xpc.xpc_connection_set_event_handler(peer, make_block(lambda m, p=peer: on_message(p, m)))
    xpc.xpc_connection_resume(peer)
    peers[peer] = True


def main():
    listener = xpc.xpc_connection_create_mach_service(b'com.apple.BlueTool', None, XPC_CONNECTION_MACH_SERVICE_LISTENER)
    if not listener:
        log('cannot listen on com.apple.BlueTool', always=True)
        sys.exit(1)
    xpc.xpc_connection_set_event_handler(listener, make_block(on_listener_event))
    xpc.xpc_connection_resume(listener)
    log('listening on com.apple.BlueTool')
    dispatch.dispatch_main()


if __name__ == '__main__':
    main()
