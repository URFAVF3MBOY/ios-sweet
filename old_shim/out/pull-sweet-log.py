#!/usr/bin/env python3
"""
Pull and decode the SWEET persistent kernel log from a OnePlus 7T in TWRP/OrangeFox.

Requirements:
  - Windows, macOS, or Linux with adb installed (Windows can use C:\\Android\\adb.exe)
  - Phone booted into recovery with ADB enabled
  - Python 3

Run from the ios-sweet repository root:
    python pull-sweet-log.py

Writes:
    sweet-area.bin  Raw 4 MiB SWEET log area
    sweet-log.txt   Decoded log (if a SWEETLOG header is present)
"""

import shutil
import struct
import subprocess
import sys
from pathlib import Path

BLOCK_SIZE = 4096
BLOCK_COUNT = 1025
EXPECTED_BYTES = BLOCK_SIZE * BLOCK_COUNT
OUT_BIN = Path("sweet-area.bin")
OUT_TXT = Path("sweet-log.txt")


def find_adb():
    candidates = [
        shutil.which("adb"),
        r"C:\Android\adb.exe",
        r"C:\platform-tools\adb.exe",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise RuntimeError(
        "Could not find adb. Add platform-tools to PATH or edit find_adb() "
        "to point to your adb.exe."
    )


def run(args, *, input_bytes=None):
    try:
        result = subprocess.run(
            args,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise RuntimeError(f"Could not run {args[0]!r}: {exc}") from exc
    if result.returncode != 0:
        err = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(
            f"Command failed (exit {result.returncode}): {' '.join(args)}"
            + (f"\n{err}" if err else "")
        )
    return result.stdout.decode("utf-8", "replace").strip(), result.stdout


def adb_text(adb, *args):
    text, _ = run([adb, *args])
    return text


def main():
    adb = find_adb()
    print(f"Using ADB: {adb}")

    # Check that ADB can see an online device.
    devices = adb_text(adb, "devices", "-l")
    online = []
    for line in devices.splitlines()[1:]:
        parts = line.split()
        # ADB reports Android normally as "device" and recovery as "recovery".
        # Both are valid connected states for this read-only log pull.
        if len(parts) >= 2 and parts[1] in ("device", "recovery"):
            online.append(parts[0])

    if not online:
        print(devices or "(no device list returned)")
        raise RuntimeError(
            "No online ADB device found. Boot into TWRP/OrangeFox and enable ADB."
        )
    if len(online) > 1:
        print("Multiple ADB devices found:")
        print(devices)
        raise RuntimeError(
            "Leave only the intended phone connected, or edit the script to select a serial."
        )

    serial = online[0]
    print(f"Connected device: {serial}")

    # Find userdata by-name symlink in recovery.
    shell = (
        'for d in /dev/block/by-name/userdata '
        '/dev/block/bootdevice/by-name/userdata; do '
        'if [ -e "$d" ]; then readlink -f "$d"; exit 0; fi; '
        'done; exit 1'
    )
    dev = adb_text(adb, "-s", serial, "shell", shell).strip()
    if not dev.startswith("/dev/"):
        raise RuntimeError(
            f"Could not resolve userdata block device; ADB returned: {dev!r}"
        )
    print(f"Userdata block device: {dev}")

    size_text = adb_text(
        adb, "-s", serial, "shell", "blockdev", "--getsize64", dev
    )
    try:
        total_bytes = int(size_text.strip())
    except ValueError as exc:
        raise RuntimeError(f"Unexpected userdata size: {size_text!r}") from exc

    total_blocks = total_bytes // BLOCK_SIZE
    skip = total_blocks - BLOCK_COUNT
    if skip < 0:
        raise RuntimeError(
            f"Partition is too small ({total_bytes} bytes) for the requested log area."
        )

    print(f"Userdata size: {total_bytes:,} bytes")
    print(f"Reading final {BLOCK_COUNT} blocks (block {skip} onward).")
    print("Reading only; this script does not write to the phone.")

    # Capture stdout as bytes via Python, avoiding PowerShell's binary-redirection
    # behavior in Windows PowerShell 5.1.
    dd_command = (
        f"dd if={dev} bs={BLOCK_SIZE} skip={skip} "
        f"count={BLOCK_COUNT} 2>/dev/null"
    )
    _, raw = run([adb, "-s", serial, "exec-out", dd_command])

    OUT_BIN.write_bytes(raw)
    print(f"Saved raw area: {OUT_BIN.resolve()} ({len(raw):,} bytes)")

    if len(raw) != EXPECTED_BYTES:
        raise RuntimeError(
            f"Expected {EXPECTED_BYTES:,} bytes but received {len(raw):,}. "
            "The raw file was kept for inspection; check recovery and ADB."
        )

    # SWEET log format: 4 KiB header followed by a circular byte ring.
    if len(raw) < 4096 + 24:
        raise RuntimeError("Raw data is too short to contain a SWEETLOG header.")

    magic, version, ring_size, total, flush_seq = struct.unpack_from(
        "<8sIIQQ", raw, 0
    )
    if magic != b"SWEETLOG":
        print(
            "No SWEETLOG header found. The raw dump is saved as "
            f"{OUT_BIN.resolve()}.\n"
            "Possible causes: the running kernel did not flush the persistent "
            "logger, or this log area belongs to a different partition layout."
        )
        return 2

    ring = raw[4096 : 4096 + ring_size]
    if ring_size <= 0 or len(ring) != ring_size:
        raise RuntimeError(
            f"Invalid SWEETLOG ring size in header: {ring_size}"
        )

    if total <= ring_size:
        log_bytes = ring[:total]
        wrapped = False
    else:
        start = total % ring_size
        log_bytes = ring[start:] + ring[:start]
        wrapped = True

    text = log_bytes.decode("utf-8", "replace").replace("\r", "")
    OUT_TXT.write_text(text, encoding="utf-8", newline="")
    print(
        f"SWEETLOG v{version}: ring={ring_size:,} bytes, "
        f"total={total:,} bytes, flush=#{flush_seq}"
        + (" (wrapped; oldest bytes overwritten)" if wrapped else "")
    )
    print(f"Saved decoded log: {OUT_TXT.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, struct.error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
