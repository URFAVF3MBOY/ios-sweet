#!/usr/bin/env python3
# Asks the PC's Apple Mobile Device Service (usbmux on 127.0.0.1:27015) for attached devices.
import socket, struct, plistlib, sys
def req(msg):
    s = socket.create_connection(('127.0.0.1', 27015), timeout=5)
    body = plistlib.dumps(msg)
    s.sendall(struct.pack('<IIII', 16 + len(body), 1, 8, 1) + body)
    hdr = s.recv(16); n = struct.unpack('<I', hdr[:4])[0]
    data = b''
    while len(data) < n - 16:
        data += s.recv(n - 16 - len(data))
    return s, plistlib.loads(data)
s, r = req({'MessageType': 'ListDevices', 'ClientVersionString': 'kitty', 'ProgName': 'kitty'})
for d in r.get('DeviceList', []):
    p = d.get('Properties', {})
    print('DeviceID', d.get('DeviceID'), 'ConnectionType', p.get('ConnectionType'), 'ProductID', hex(p.get('ProductID', 0)),
          'LocationID', p.get('LocationID'), 'Serial', (p.get('SerialNumber') or '')[:6] + '...')
print(len(r.get('DeviceList', [])), 'devices')
