#!/usr/bin/env python3
"""Slice out/arm9.full.asm by address range.

usage: dis.py START [END]

Addresses are hex, with or without 0x. With one argument, dumps 0x80 bytes.
"""
import re
import sys

ASM = "out/arm9.full.asm"
LINE = re.compile(r"^\s*([0-9a-f]+):\t")


def main():
    a = int(sys.argv[1], 16)
    b = int(sys.argv[2], 16) if len(sys.argv) > 2 else a + 0x80
    for line in open(ASM):
        m = LINE.match(line)
        if m and a <= int(m.group(1), 16) < b:
            sys.stdout.write(line)


main()
