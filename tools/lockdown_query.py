#!/usr/bin/env python3
# Through usbmux to the first device's lockdownd (port 62078): QueryType and a few public GetValue keys. No pairing.
import socket, struct, plistlib
def mux(msg):
    s = socket.create_connection(('127.0.0.1', 27015), timeout=10)
    body = plistlib.dumps(msg)
    s.sendall(struct.pack('<IIII', 16 + len(body), 1, 8, 1) + body)
    hdr = s.recv(16); n = struct.unpack('<I', hdr[:4])[0]
    data = b''
    while len(data) < n - 16:
        data += s.recv(n - 16 - len(data))
    return s, plistlib.loads(data)
s, r = mux({'MessageType': 'ListDevices', 'ClientVersionString': 'kitty', 'ProgName': 'kitty'})
dev = r['DeviceList'][0]['DeviceID']
s, r = mux({'MessageType': 'Connect', 'DeviceID': dev, 'PortNumber': socket.htons(62078),
            'ClientVersionString': 'kitty', 'ProgName': 'kitty'})
print('connect result', r.get('Number'))
def ld(req):
    b = plistlib.dumps(req)
    s.sendall(struct.pack('>I', len(b)) + b)
    n = struct.unpack('>I', s.recv(4))[0]
    data = b''
    while len(data) < n:
        data += s.recv(n - len(data))
    return plistlib.loads(data)
print(ld({'Request': 'QueryType', 'Label': 'kitty'}))
for key in ['DeviceName', 'ProductType', 'ProductVersion', 'BuildVersion', 'DeviceClass']:
    r = ld({'Request': 'GetValue', 'Key': key, 'Label': 'kitty'})
    print(key, '=', r.get('Value', r.get('Error')))
