#!/usr/bin/env python3
"""Flash a new firmware image over the command UART (no debug probe needed).

    python3 tools/fw_update.py /dev/ttyACM0 build/hid_proxy.bin
    python3 tools/fw_update.py COM7 build/hid_proxy.bin

Needs pyserial (pip install pyserial). Protocol: see source/fw_update.h.

The board receives the image into a separate staging area and checks its
CRC and boot header before touching the running firmware, so a failed or
interrupted transfer is harmless: the board stays on the old firmware and
you can simply run this again. Only the final copy (about a second) must
not lose power.
"""
import argparse
import struct
import sys
import time
import zlib

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

BAUD = 6_000_000
CHUNK = 1024              # APP_UPDATE_CHUNK
SLOT_SIZE = 0x100000      # APP_SLOT_SIZE
FLASH_BASE = 0x60000000   # APP_FLASH_BASE
SECTOR = 4096


def check_image(img: bytes) -> None:
    """Same checks as fw_update_image_ok() on the board, to fail early."""
    if len(img) < 0x2400 or len(img) > SLOT_SIZE:
        sys.exit(f"image size {len(img)} is outside 0x2400..{SLOT_SIZE:#x}")
    if img[:4] != b"FCFB":
        sys.exit("not a bootable image: no FlexSPI configuration block (use the .bin, not the .hex/.elf)")
    entry, = struct.unpack_from("<I", img, 0x1004)
    start, = struct.unpack_from("<I", img, 0x1020)
    if img[0x1000] != 0xD1 or start != FLASH_BASE or not (FLASH_BASE + 0x2000 <= entry < FLASH_BASE + len(img)):
        sys.exit("not a bootable image: bad image vector table")


class Link:
    def __init__(self, port: str):
        self.s = serial.Serial(port, BAUD, timeout=0.05)
        self.buf = b""

    def readline(self, timeout: float) -> str:
        """One line without the line end; raises TimeoutError."""
        end = time.monotonic() + timeout
        while b"\r\n" not in self.buf:
            if time.monotonic() > end:
                raise TimeoutError(self.buf.decode(errors="replace"))
            self.buf += self.s.read(256)
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode(errors="replace")

    def wait_prompt(self, timeout: float) -> None:
        end = time.monotonic() + timeout
        while b">>> " not in self.buf:
            if time.monotonic() > end:
                raise TimeoutError("no '>>> ' prompt from the board")
            self.buf += self.s.read(256)
        self.buf = self.buf.split(b">>> ")[-1]

    def send(self, data: bytes) -> None:
        self.s.write(data)
        self.s.flush()


def expect(link: Link, prefix: str, timeout: float) -> str:
    while True:
        line = link.readline(timeout)
        if line.startswith("error"):
            sys.exit(f"board: {line} (the old firmware is still running)")
        if line.startswith(prefix):
            return line


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="serial port of the CH343P, e.g. /dev/ttyACM0 or COM7")
    ap.add_argument("image", help="build/hid_proxy.bin")
    args = ap.parse_args()

    img = open(args.image, "rb").read()
    check_image(img)
    crc = zlib.crc32(img) & 0xFFFFFFFF

    link = Link(args.port)
    link.send(b"\r\n")                       # end any half-typed line
    link.send(b"cmd.version()\r\n")
    expect(link, "cmd.version()", 2.0)
    print(f"board firmware {link.readline(2.0)}")
    link.wait_prompt(2.0)

    print(f"sending {len(img)} bytes, crc32 {crc:#010x}")
    link.send(f"cmd.update({len(img)},{crc})\r\n".encode())
    expect(link, "cmd.update(", 2.0)
    expect(link, "erasing", 2.0)
    sectors = (len(img) + SECTOR - 1) // SECTOR
    expect(link, "ready", 2.0 + sectors * 0.5)

    sent = 0
    t0 = time.monotonic()
    while sent < len(img):
        part = img[sent:sent + CHUNK]
        link.send(part)
        sent += len(part)
        line = expect(link, "ok ", 5.0)
        if int(line.split()[1]) != sent:
            sys.exit(f"board acknowledged {line!r}, expected ok {sent}")
        print(f"\r{sent * 100 // len(img):3d}%  {sent}/{len(img)}", end="", flush=True)
    print(f"\ntransferred in {time.monotonic() - t0:.1f} s")

    expect(link, "installing", 10.0)
    print("verified; installing and restarting (do not unplug)...")
    link.wait_prompt(20.0)                   # the new firmware prints ">>> " at boot
    link.send(b"cmd.version()\r\n")
    expect(link, "cmd.version()", 2.0)
    print(f"done: firmware {link.readline(2.0)}")


if __name__ == "__main__":
    main()
