#!/usr/bin/env python3
"""xrefs.py - find rip-relative references to a set of addresses inside the
__TEXT,__text of an extracted dyld-cache dylib.

Needed because llvm-objdump on a 9 MB __text (SkyLight) is slow to grep, and
because we only want "who references this string / this selector slot".

The scan is a simple byte heuristic over the encodings that can carry a
RIP-relative memory operand:
    [REX] 8B/8D/89/3B/39/C7/FF/0F B6/B7  modrm  disp32   (modrm & 0xC7 == 0x05)
plus 4-byte disp32 fixups reported as candidates with the exact target.

usage: ./xrefs.py <dylib> 0xADDR[,0xADDR...]
"""
import struct
import subprocess
import sys
import os

LLVM_BIN = "/Library/Developer/CommandLineTools/usr/bin"


def sections(path):
    out = subprocess.run([os.path.join(LLVM_BIN, "otool"), "-l", path],
                         capture_output=True, text=True).stdout
    secs, cur = [], {}
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("sectname"):
            cur = {"name": s.split()[1]}
        elif s.startswith("segname") and "segname" not in cur:
            cur["seg"] = s.split()[1]
        elif s.startswith("addr"):
            cur["addr"] = int(s.split()[1], 16)
        elif s.startswith("size"):
            cur["size"] = int(s.split()[1], 16)
        elif s.startswith("offset"):
            cur["off"] = int(s.split()[1])
            secs.append(cur)
            cur = {}
    return secs


OPCODES1 = {0x8B, 0x8D, 0x89, 0x3B, 0x39, 0xC7, 0xFF, 0x8A, 0x88}
OPCODES2 = {(0x0F, 0xB6), (0x0F, 0xB7), (0x0F, 0xBE)}


def scan(path, targets, text_name="__text"):
    secs = sections(path)
    text = [s for s in secs if s["name"] == text_name][0]
    with open(path, "rb") as f:
        f.seek(text["off"])
        data = f.read(text["size"])
    base = text["addr"]
    hits = []
    tset = dict((t, []) for t in targets)
    n = len(data)
    for i in range(n - 7):
        b = data[i]
        if 0x40 <= b <= 0x4F:
            op = data[i + 1]
            if op in OPCODES1:
                modrm = data[i + 2]
                if (modrm & 0xC7) == 0x05:
                    disp = struct.unpack_from("<i", data, i + 3)[0]
                    end = base + i + 7
                    tgt = end + disp
                    if tgt in tset:
                        hits.append((base + i, tgt, i + 7))
            elif (op, data[i + 2]) in OPCODES2:
                modrm = data[i + 3]
                if (modrm & 0xC7) == 0x05:
                    disp = struct.unpack_from("<i", data, i + 4)[0]
                    end = base + i + 8
                    tgt = end + disp
                    if tgt in tset:
                        hits.append((base + i, tgt, i + 8))
        else:
            op = b
            if op in OPCODES1:
                modrm = data[i + 1]
                if (modrm & 0xC7) == 0x05:
                    disp = struct.unpack_from("<i", data, i + 2)[0]
                    end = base + i + 6
                    tgt = end + disp
                    if tgt in tset:
                        hits.append((base + i, tgt, i + 6))
            elif (op, data[i + 1]) in OPCODES2:
                modrm = data[i + 2]
                if (modrm & 0xC7) == 0x05:
                    disp = struct.unpack_from("<i", data, i + 3)[0]
                    end = base + i + 7
                    tgt = end + disp
                    if tgt in tset:
                        hits.append((base + i, tgt, i + 7))
    return hits


if __name__ == "__main__":
    path = sys.argv[1]
    targets = []
    for a in sys.argv[2].split(","):
        targets.append(int(a, 0))
    for insn, tgt, ln in sorted(scan(path, targets)):
        print("insn 0x%x -> 0x%x (len %d)" % (insn, tgt, ln))
