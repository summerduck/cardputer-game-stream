#!/usr/bin/env python3
"""Wraps firmware.bin into a package for M5Launcher's WebUI "OTA Update" that also asks Launcher
to create a 2 MB "books" flash partition, so books can be stored without an SD card.

Launcher's web page reads the partition table at 0x8000 of the uploaded .bin, installs the app
image and creates each data partition it lists (see webUi/scripts.js analyzeFile()). The
partition gets a small non-empty stub; the reader formats it as LittleFS on first start.

usage: make_installer.py firmware.bin out.bin
"""
import struct
import sys

BOOKS_SIZE = 0x200000
STUB = b"RSVPBOOKS" + b"\x55" * (4096 - 9)


def entry(ptype, subtype, offset, size, label):
    return struct.pack("<2sBBII16sI", b"\xAA\x50", ptype, subtype, offset, size, label.encode().ljust(16, b"\0"), 0)


def main():
    app = open(sys.argv[1], "rb").read()
    assert app[0] == 0xE9, "not an ESP32 app image"
    app_part = (len(app) + 0xFFFF) & ~0xFFFF
    books_offset = 0x10000 + app_part

    table = (
        entry(0x01, 0x02, 0x9000, 0x5000, "nvs")          # first entry must be data (Launcher checks)
        + entry(0x01, 0x00, 0xE000, 0x2000, "otadata")
        + entry(0x00, 0x10, 0x10000, app_part, "app0")
        + entry(0x01, 0x83, books_offset, BOOKS_SIZE, "books")  # 0x83 = LittleFS
    )
    table += b"\xFF" * (0xC00 - len(table))

    image = bytearray(b"\xFF" * (books_offset + len(STUB)))
    image[0x8000:0x8000 + len(table)] = table
    image[0x10000:0x10000 + len(app)] = app
    image[books_offset:] = STUB
    open(sys.argv[2], "wb").write(image)
    print(f"{sys.argv[2]}: app {len(app)} B in {app_part:#x} slot, books partition {BOOKS_SIZE // 1024} KB")


if __name__ == "__main__":
    main()
