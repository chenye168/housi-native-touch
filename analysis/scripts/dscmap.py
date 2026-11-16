#!/usr/bin/env python3
"""Minimal read-only parser for the x86_64 (Rosetta) dyld shared cache.

Purpose: the binaries produced by /usr/lib/dsc_extractor.bundle keep their
original cache vmaddrs in the Mach-O section headers (e.g. 0x7FFD00953500)
and keep *cache-encoded* pointer values in data sections.  This module maps a
cache address back to (file, file offset) so we can resolve those pointers
(e.g. ObjC selrefs -> selector strings).

Header struct: tools/dyldsrc/include/mach-o/dyld_cache_format.h
mapping      : struct dyld_cache_mapping_info {u64 address,size,fileOffset;u32 maxProt,initProt}
"""
import os
import struct
import sys

DSC_DIR = "/System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld"
CACHE_PREFIX = "dyld_shared_cache_x86_64"


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Region(object):
    __slots__ = ("file", "vaddr", "size", "fileoff", "prot")

    def __init__(self, file, vaddr, size, fileoff, prot):
        self.file, self.vaddr, self.size, self.fileoff, self.prot = (
            file, vaddr, size, fileoff, prot)

    def __repr__(self):
        return "<%s 0x%x-0x%x fileoff 0x%x>" % (
            os.path.basename(self.file), self.vaddr, self.vaddr + self.size,
            self.fileoff)


class Cache(object):
    def __init__(self, dsc_dir=DSC_DIR):
        self.regions = []
        self.files = {}
        for name in sorted(os.listdir(dsc_dir)):
            if not name.startswith(CACHE_PREFIX):
                continue
            path = os.path.join(dsc_dir, name)
            if not os.path.isfile(path):
                continue
            with open(path, "rb") as f:
                head = f.read(0x1000)
            if not head.startswith(b"dyld_v1"):
                continue
            mo, mc = _u32(head, 0x10), _u32(head, 0x14)
            for i in range(mc):
                addr, size, foff, maxp, initp = struct.unpack_from(
                    "<QQQII", head, mo + i * 32)
                self.regions.append(Region(path, addr, size, foff, initp))
                self.files.setdefault(path, []).append(self.regions[-1])
            maybe = head[16:32].split(b"\0")[0]
            # sub-cache entries point at the sibling files: not needed, we
            # simply scan every file in the directory.
            _ = maybe
        self.regions.sort(key=lambda r: r.vaddr)
        self._fh = {}

    # -- io ---------------------------------------------------------------
    def handle(self, path):
        fh = self._fh.get(path)
        if fh is None:
            fh = self._fh[path] = open(path, "rb")
        return fh

    def region_for(self, addr):
        lo, hi = 0, len(self.regions) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            r = self.regions[mid]
            if addr < r.vaddr:
                hi = mid - 1
            elif addr >= r.vaddr + r.size:
                lo = mid + 1
            else:
                return r
        return None

    def read(self, addr, n):
        r = self.region_for(addr)
        if r is None:
            return None
        fh = self.handle(r.file)
        fh.seek(r.fileoff + (addr - r.vaddr))
        return fh.read(n)

    def cstring(self, addr, limit=512):
        r = self.region_for(addr)
        if r is None:
            return None
        fh = self.handle(r.file)
        fh.seek(r.fileoff + (addr - r.vaddr))
        out = bytearray()
        while len(out) < limit:
            chunk = fh.read(1)
            if not chunk or chunk == b"\0":
                break
            out += chunk
        try:
            return out.decode("utf-8")
        except UnicodeDecodeError:
            return None

    # -- pointer decoding -------------------------------------------------
    # In this cache, the low 32 bits of a stored pointer equal the low 32 bits
    # of the target cache address, and the high 32 bits carry chained-fixup
    # metadata (diversity / next / bind / auth bits).  The high 32 bits were
    # observed as 0x00000205 / 0x00000405 / 0x00000200 / ... so the region base
    # must be recovered by trial: for every known mapping base whose low 32
    # bits are 0 we test whether base|low32 lands inside a mapping and whether
    # that address then reads back as a sane string / is a plausible pointer.
    def decode_ptr(self, raw, want_string=True):
        low = raw & 0xFFFFFFFF
        cands = []
        for r in self.regions:
            base = r.vaddr & ~0xFFFFFFFF
            if base & 0xFFFFFFFF:
                continue
            a = base | low
            if self.region_for(a) is None:
                continue
            if want_string:
                s = self.cstring(a, 256)
                if not s or len(s) < 2 or any(ord(c) < 0x20 or ord(c) > 0x7E
                                              for c in s):
                    continue
            cands.append(a)
        # de-dup, prefer the region of the dylib we are looking at (caller can
        # pass the expected base through `bases`)
        seen, uniq = set(), []
        for a in cands:
            if a not in seen:
                seen.add(a)
                uniq.append(a)
        return uniq


if __name__ == "__main__":
    c = Cache()
    print("regions:", len(c.regions))
    for r in c.regions:
        print(" ", r)
    if len(sys.argv) > 1:
        for a in sys.argv[1:]:
            a = int(a, 0)
            print(hex(a), "->", repr(c.cstring(a, 200)))
