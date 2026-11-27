#!/usr/bin/env python3
"""Minimal lockdownd client (port 62078 plist protocol: 4-byte big-endian length + XML plist).

  ldclient.py [HOST [PORT]]        QueryType + unauthenticated GetValue queries
Default host 127.0.0.1 (run on the phone), port 62078.
"""
import plistlib
import socket
import struct
import sys


def call(s, req):
    body = plistlib.dumps(req)
    s.sendall(struct.pack(">I", len(body)) + body)
    hdr = b""
    while len(hdr) < 4:
        b = s.recv(4 - len(hdr))
        if not b:
            raise EOFError("lockdownd closed the connection")
        hdr += b
    n, = struct.unpack(">I", hdr)
    data = b""
    while len(data) < n:
        b = s.recv(n - len(data))
        if not b:
            raise EOFError("short reply")
        data += b
    return plistlib.loads(data)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 62078
    s = socket.create_connection((host, port), timeout=10)
    label = "ldclient"
    print("QueryType:", call(s, {"Label": label, "Request": "QueryType"}))
    r = call(s, {"Label": label, "Request": "GetValue"})
    if "Value" in r:
        for k, v in sorted(r["Value"].items()):
            print(f"  {k} = {v!r:.100}")
    else:
        print("GetValue:", r)
    for key in ("ProductType", "ProductVersion", "BuildVersion", "DeviceName", "DeviceClass",
                "UniqueDeviceID", "SerialNumber", "HardwareModel", "ActivationState",
                "WiFiAddress", "BluetoothAddress", "CPUArchitecture", "ProtocolVersion"):
        r = call(s, {"Label": label, "Request": "GetValue", "Key": key})
        print(f"{key}: {r.get('Value', r)!r:.100}")
    s.close()


if __name__ == "__main__":
    main()
