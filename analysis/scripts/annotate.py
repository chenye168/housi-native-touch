#!/usr/bin/env python3
"""annotate.py - readable disassembly for dylibs extracted from the Rosetta
x86_64 dyld shared cache (macOS 26/27 era, multi-cache layout).

Why: /usr/lib/dsc_extractor.bundle writes each dylib out with its *original
cache vmaddrs* in the Mach-O section headers, but leaves every pointer field in
data sections as a raw cache-encoded value (low 32 bits == low 32 bits of the
target cache address; the high 32 bits are chained-fixup metadata whose value
tells you which sub-cache the target lives in - see `decode_multi`).  Local
symbols survive, ObjC selector refs / cfstring pointers do not, so this tool
re-resolves them:

  * __objc_selrefs  -> selector name   (selector pool lives in the main cache)
  * __stubs         -> imported symbol (jmp *got -> decode -> owner dylib -> nm)
  * __cstring/__oslogstring -> the string itself
  * __cfstring      -> the CFString's contents
  * __const/__objc_const -> class/struct pointer (best effort)

Usage:
  ./annotate.py <dylib> --addr 0x7ffd0091e645 --stop 0x7ffd0091f3ce
  ./annotate.py <dylib> --symbol '-[BKHIDDirectTouchEventProcessor ...]'
"""
import argparse
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dscmap import Cache  # noqa: E402

DSC_DIR = "/System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld"
MAP_FILE = os.path.join(DSC_DIR, "dyld_shared_cache_x86_64.map")
EXTRACT_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "tools", "x86cache")
LLVM_BIN = "/Library/Developer/CommandLineTools/usr/bin"
MAIN_BASE = 0x7FF800000000
BASES = [0x7FF800000000, 0x7FF900000000, 0x7FFA00000000, 0x7FFB00000000,
         0x7FFC00000000, 0x7FFD00000000, 0x7FFE00000000]


# ---------------------------------------------------------------- mach-o ----
class MachO(object):
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        magic = struct.unpack_from("<I", self.data, 0)[0]
        if magic != 0xFEEDFACF:
            raise SystemExit("not a 64-bit little-endian Mach-O: %s" % path)
        self.ncmds = struct.unpack_from("<I", self.data, 16)[0]
        off = 32
        self.sections = []          # (seg, sect, addr, size, fileoff)
        self.segments = []          # (seg, vmaddr, vmsize, fileoff, filesize)
        for _ in range(self.ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.data, off)
            if cmd == 0x19:  # LC_SEGMENT_64
                segname = self.data[off + 8:off + 24].split(b"\0")[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                    "<QQQQ", self.data, off + 24)
                nsects = struct.unpack_from("<I", self.data, off + 64)[0]
                self.segments.append((segname, vmaddr, vmsize, fileoff,
                                      filesize))
                so = off + 72
                for _ in range(nsects):
                    sectname = self.data[so:so + 16].split(b"\0")[0].decode()
                    sseg = self.data[so + 16:so + 32].split(b"\0")[0].decode()
                    addr, size = struct.unpack_from("<QQ", self.data, so + 32)
                    sfoff = struct.unpack_from("<I", self.data, so + 48)[0]
                    self.sections.append((sseg, sectname, addr, size, sfoff))
                    so += 80
            off += cmdsize

    def sect(self, name):
        for s in self.sections:
            if s[1] == name:
                return s
        return None

    def read(self, addr, n):
        for sseg, sectname, saddr, ssize, sfoff in self.sections:
            if ssize and saddr <= addr and addr + n <= saddr + ssize:
                return self.data[sfoff + (addr - saddr):][:n]
        return None

    def addr_to_fileoff(self, addr):
        for sseg, sectname, saddr, ssize, sfoff in self.sections:
            if ssize and saddr <= addr < saddr + ssize:
                return sfoff + (addr - saddr)
        return None


# ------------------------------------------------------------ cache glue ----
class Resolver(object):
    def __init__(self):
        self.cache = Cache()
        self._map = None
        self._nmsyms = {}
        self._stub_memo = {}

    # -- .map: which dylib owns a cache address ---------------------------
    @property
    def map(self):
        if self._map is None:
            segs = {}
            cur = None
            with open(MAP_FILE, "r") as f:
                for line in f:
                    if line.startswith("mapping"):
                        continue
                    if line.startswith("/"):
                        cur = line.strip()
                        segs[cur] = []
                    elif cur and line.strip():
                        p = line.split()
                        if len(p) >= 4 and p[1].startswith("0x") \
                                and p[3].startswith("0x"):
                            segs[cur].append((p[0], int(p[1], 16),
                                              int(p[3], 16)))
            self._map = segs
        return self._map

    def owner(self, addr):
        for path, segs in self.map.items():
            for name, a, b in segs:
                if a <= addr < b:
                    return path, name
        return None, None

    # -- imported symbols -------------------------------------------------
    def nm(self, path):
        if path in self._nmsyms:
            return self._nmsyms[path]
        rel = path.lstrip("/")
        f = os.path.join(EXTRACT_ROOT, rel)
        syms = {}
        if os.path.exists(f):
            try:
                out = subprocess.run(
                    [os.path.join(LLVM_BIN, "llvm-nm"), "-a", f],
                    capture_output=True, text=True, timeout=600).stdout
            except Exception:
                out = ""
            for line in out.splitlines():
                p = line.split()
                if len(p) == 3:
                    try:
                        syms[int(p[0], 16)] = (p[1], p[2])
                    except ValueError:
                        pass
        self._nmsyms[path] = syms
        return syms

    def symbol_at(self, addr):
        path, seg = self.owner(addr)
        if not path:
            return None, None, None
        sym = self.nm(path).get(addr)
        return path, seg, sym

    def decode_multi(self, raw, want="any"):
        """Return [(addr, owner_path, section)] for every sub-cache base."""
        low = raw & 0xFFFFFFFF
        out = []
        for base in BASES:
            a = base | low
            path, seg = self.owner(a)
            if path:
                out.append((a, path, seg))
        return out

    def stub_target(self, addr):
        """Resolve a __TEXT,__stubs entry (jmp *disp32(%rip)) to a symbol."""
        if addr in self._stub_memo:
            return self._stub_memo[addr]
        res = None
        b = self.cache.read(addr, 6)
        if b and b[0] == 0xFF and b[1] == 0x25:
            disp = struct.unpack_from("<i", b, 2)[0]
            got = addr + 6 + disp
            raw = self.cache.read(got, 8)
            if raw:
                v = struct.unpack("<Q", raw)[0]
                for cand, path, seg in self.decode_multi(v):
                    p, s, sym = self.symbol_at(cand)
                    if sym and not sym[1].startswith("t"):
                        res = (sym[1], path, cand)
                        break
                    if sym and res is None:
                        res = (sym[1], path, cand)
                if res is None:
                    res = ("<got %x -> %016x>" % (got, v), None, None)
        self._stub_memo[addr] = res
        return res


# ------------------------------------------------------------- strings -----
def cfstring_text(macho, addr):
    b = macho.read(addr, 32)
    if not b:
        return None
    flags, cstr_raw, length = struct.unpack_from("<QQQ", b, 8)
    r = RESOLVER
    for base in BASES:
        a = base | (cstr_raw & 0xFFFFFFFF)
        s = r.cache.cstring(a, min(length + 8, 512) if length < 512 else 512)
        if s and (len(s) == length):
            return s
        if s and 0 < len(s) <= 512:
            return s
    return None


def describe_addr(macho, addr):
    for sseg, sectname, saddr, ssize, sfoff in macho.sections:
        if not (ssize and saddr <= addr < saddr + ssize):
            continue
        if sectname in ("__cstring", "__oslogstring"):
            s = macho.read(addr, min(ssize - (addr - saddr), 300))
            if s:
                z = s.find(b"\0")
                s = s if z < 0 else s[:z]
                return "%s %r" % (sectname, s.decode("utf-8", "replace"))
        if sectname == "__cfstring":
            s = cfstring_text(macho, addr)
            return "__cfstring %r" % s
        if sectname == "__objc_selrefs":
            return "__objc_selrefs %s" % (selname(macho, addr) or "?")
        return sectname
    return None


_SEL_CACHE = {}


def selname(macho, slot_addr):
    key = (macho.path, slot_addr)
    if key in _SEL_CACHE:
        return _SEL_CACHE[key]
    raw = macho.read(slot_addr, 8)
    name = None
    if raw:
        v = struct.unpack("<Q", raw)[0]
        low = v & 0xFFFFFFFF
        s = RESOLVER.cache.cstring(MAIN_BASE | low, 256)
        if s and re.match(r"^[A-Za-z_][A-Za-z0-9_:]*$", s):
            name = s
    _SEL_CACHE[key] = name
    return name


RESOLVER = None


# -------------------------------------------------------------- driver -----
def main():
    global RESOLVER
    ap = argparse.ArgumentParser()
    ap.add_argument("dylib")
    ap.add_argument("--addr", default=None, help="start address (hex)")
    ap.add_argument("--stop", default=None, help="stop address (hex)")
    ap.add_argument("--symbol", default=None,
                    help="disassemble the function with this exact symbol")
    args = ap.parse_args()

    macho = MachO(args.dylib)
    RESOLVER = Resolver()

    if args.symbol:
        out = subprocess.run([os.path.join(LLVM_BIN, "llvm-nm"), "-an",
                              args.dylib], capture_output=True, text=True).stdout
        syms = []
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3:
                try:
                    syms.append((int(p[0], 16), p[2]))
                except ValueError:
                    pass
        syms.sort()
        start = None
        for i, (a, n) in enumerate(syms):
            if n == args.symbol:
                start = a
                stop = syms[i + 1][0] if i + 1 < len(syms) else a + 0x2000
                break
        if start is None:
            raise SystemExit("symbol not found: %s" % args.symbol)
    else:
        if args.addr:
            start = int(args.addr, 16)
            stop = int(args.stop, 16)
        else:                       # whole __TEXT,__text
            t = macho.sect("__text")
            start, stop = t[2], t[2] + t[3]

    cmd = [os.path.join(LLVM_BIN, "llvm-objdump"), "-d",
           "--start-address=0x%x" % start, "--stop-address=0x%x" % stop,
           args.dylib]
    txt = subprocess.run(cmd, capture_output=True, text=True).stdout

    # stub range of this dylib
    stubs = macho.sect("__stubs")
    stub_lo = stubs[2] if stubs else 0
    stub_hi = (stubs[2] + stubs[3]) if stubs else 0

    # index: label for call targets that are exported symbols
    nmout = subprocess.run([os.path.join(LLVM_BIN, "llvm-nm"), "-an",
                            args.dylib], capture_output=True, text=True).stdout
    labels = {}
    for line in nmout.splitlines():
        p = line.split()
        if len(p) == 3:
            try:
                labels[int(p[0], 16)] = p[2]
            except ValueError:
                pass

    for line in txt.splitlines():
        m = re.match(
            r"^([0-9a-f]+):\s+([0-9a-f]{2}(?: [0-9a-f]{2})*)\s*\t(.*)$", line)
        if not m:
            print(line)
            continue
        addr = int(m.group(1), 16)
        insn = m.group(3)
        extra = ""

        # callq <direct target>
        c = re.match(r"callq\s+0x([0-9a-f]+)", insn)
        if c:
            t = int(c.group(1), 16)
            if t in labels and labels[t] != "":
                extra = "  ; %s" % labels[t]
            if stub_lo <= t < stub_hi:
                sym = RESOLVER.stub_target(t)
                if sym and sym[0]:
                    extra = "  ; IMPORT %s" % sym[0]

        # rip-relative operand: "0x...(%rip)" -> absolute target
        r = re.search(r"0x([0-9a-f]+)\(%rip\)", insn)
        if r:
            disp = int(r.group(1), 16)
            # signed 32-bit displacement
            if disp >= 0x80000000:
                disp -= 0x100000000
            tgt = addr + len(bytes.fromhex(m.group(2).replace(" ", ""))) + disp
            d = describe_addr(macho, tgt)
            if d:
                extra = "  ; -> 0x%x %s" % (tgt, d)
            elif tgt in labels:
                extra = "  ; -> 0x%x %s" % (tgt, labels[tgt])

        print("%s: %-28s\t%s%s" % (m.group(1), m.group(2), insn, extra))


if __name__ == "__main__":
    main()
