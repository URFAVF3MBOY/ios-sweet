#!/usr/bin/env python3
# On the phone: a 352x288 420v CVPixelBuffer with IOSurface; prints CoreVideo and IOSurface plane counts and bytes-per-row.
import ctypes
cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
cv = ctypes.CDLL('/System/Library/Frameworks/CoreVideo.framework/CoreVideo')
ios = ctypes.CDLL('/System/Library/Frameworks/IOSurface.framework/IOSurface')
V = ctypes.c_void_p
cf.CFDictionaryCreate.restype = V
cf.CFDictionaryCreate.argtypes = [V, ctypes.POINTER(V), ctypes.POINTER(V), ctypes.c_long, V, V]
kKeyCB = V.in_dll(cf, 'kCFTypeDictionaryKeyCallBacks'); kValCB = V.in_dll(cf, 'kCFTypeDictionaryValueCallBacks')
empty = cf.CFDictionaryCreate(None, None, None, 0, ctypes.addressof(kKeyCB), ctypes.addressof(kValCB))
key = V.in_dll(cv, 'kCVPixelBufferIOSurfacePropertiesKey')
keys = (V * 1)(key.value); vals = (V * 1)(empty)
attrs = cf.CFDictionaryCreate(None, keys, vals, 1, ctypes.addressof(kKeyCB), ctypes.addressof(kValCB))
pb = V()
cv.CVPixelBufferCreate.argtypes = [V, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_uint32, V, ctypes.POINTER(V)]
r = cv.CVPixelBufferCreate(None, 352, 288, 0x34323076, attrs, ctypes.byref(pb))   # '420v'
print('CVPixelBufferCreate', r)
for f in ['CVPixelBufferGetPlaneCount', 'CVPixelBufferGetBytesPerRowOfPlane', 'CVPixelBufferGetIOSurface']:
    getattr(cv, f).restype = ctypes.c_size_t if 'Surface' not in f else V
cv.CVPixelBufferGetBytesPerRowOfPlane.argtypes = [V, ctypes.c_size_t]
print('CV planes', cv.CVPixelBufferGetPlaneCount(pb), 'bpr', [cv.CVPixelBufferGetBytesPerRowOfPlane(pb, i) for i in range(2)])
s = cv.CVPixelBufferGetIOSurface(pb); print('surface', hex(s or 0))
if s:
    for f in ['IOSurfaceGetPlaneCount', 'IOSurfaceGetBytesPerRowOfPlane', 'IOSurfaceGetWidthOfPlane', 'IOSurfaceGetHeightOfPlane', 'IOSurfaceGetBytesPerRow', 'IOSurfaceGetID', 'IOSurfaceGetPixelFormat']:
        getattr(ios, f).restype = ctypes.c_size_t
    for f in ['IOSurfaceGetBytesPerRowOfPlane', 'IOSurfaceGetWidthOfPlane', 'IOSurfaceGetHeightOfPlane']:
        getattr(ios, f).argtypes = [V, ctypes.c_size_t]
    print('IOSurface id', ios.IOSurfaceGetID(V(s)), 'fmt', hex(ios.IOSurfaceGetPixelFormat(V(s))), 'planes', ios.IOSurfaceGetPlaneCount(V(s)), 'bpr', ios.IOSurfaceGetBytesPerRow(V(s)))
    for i in range(2):
        print('  plane', i, 'w', ios.IOSurfaceGetWidthOfPlane(V(s), i), 'h', ios.IOSurfaceGetHeightOfPlane(V(s), i), 'bpr', ios.IOSurfaceGetBytesPerRowOfPlane(V(s), i))
