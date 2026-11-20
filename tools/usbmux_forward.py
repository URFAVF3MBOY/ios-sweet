#!/usr/bin/env python3
# usbmux_forward.py [LOCAL_PORT] [DEVICE_PORT]: TCP forwarder through the PC's usbmux (Apple Mobile Device Service on
# 127.0.0.1:27015, as iproxy does): connections to 127.0.0.1:LOCAL_PORT (default 2222) go to DEVICE_PORT (default 22)
# on the first attached device. For ssh into the phone in iPhone mode: ssh -p 2222 root@127.0.0.1
import plistlib, socket, struct, sys, threading

MUX = ('127.0.0.1', 27015)


def mux(msg, keep=False):
    s = socket.create_connection(MUX, timeout=10)
    body = plistlib.dumps(msg)
    s.sendall(struct.pack('<IIII', 16 + len(body), 1, 8, 1) + body)
    hdr = b''
    while len(hdr) < 16:
        hdr += s.recv(16 - len(hdr))
    n = struct.unpack('<I', hdr[:4])[0]
    data = b''
    while len(data) < n - 16:
        data += s.recv(n - 16 - len(data))
    return s, plistlib.loads(data)


def device_id():
    s, r = mux({'MessageType': 'ListDevices', 'ClientVersionString': 'fwd', 'ProgName': 'fwd'})
    s.close()
    devs = r.get('DeviceList', [])
    if not devs:
        raise SystemExit('no device attached to usbmux')
    return devs[0]['DeviceID']


def pump(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass
    for x in (a, b):
        try:
            x.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def serve(client, dport):
    try:
        s, r = mux({'MessageType': 'Connect', 'DeviceID': device_id(), 'PortNumber': socket.htons(dport),
                    'ClientVersionString': 'fwd', 'ProgName': 'fwd'})
        if r.get('Number') != 0:
            client.close()
            return
        s.settimeout(None)
        threading.Thread(target=pump, args=(client, s), daemon=True).start()
        pump(s, client)
    except Exception:
        client.close()


def main():
    lport = int(sys.argv[1]) if len(sys.argv) > 1 else 2222
    dport = int(sys.argv[2]) if len(sys.argv) > 2 else 22
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', lport))
    ls.listen(8)
    print('127.0.0.1:%d -> device port %d over usbmux' % (lport, dport), flush=True)
    while True:
        c, _ = ls.accept()
        threading.Thread(target=serve, args=(c, dport), daemon=True).start()


if __name__ == '__main__':
    main()
