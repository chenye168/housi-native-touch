#!/usr/bin/env python3
"""Resolve selector refs / stub targets in an extracted dyld-cache dylib."""
import subprocess, struct, sys

def load_sections(path):
    out = subprocess.check_output(['otool', '-l', path], text=True, errors='replace')
    sections = []
    cur = {}
    for line in out.splitlines():
        s = line.strip()
        if s.startswith('sectname'):
            cur = {'sect': s.split()[1]}
        elif s.startswith('segname'):
            cur['seg'] = s.split()[1]
        elif s.startswith('addr ') and 'addr' in cur.keys() if False else False:
            pass
    # second, more robust parse: blocks separated by "Section"
    blocks = out.split('Section')
    for b in blocks[1:]:
        d = {}
        for line in b.splitlines():
            ls = line.strip()
            if ls.startswith('sectname'):
                d['sect'] = ls.split()[1]
            elif ls.startswith('segname'):
                d['seg'] = ls.split()[1]
            elif ls.startswith('addr ') and 'addr' not in d:
                try: d['addr'] = int(ls.split()[1], 16)
                except Exception: pass
            elif ls.startswith('size ') and 'size' not in d:
                try: d['size'] = int(ls.split()[1], 16)
                except Exception: pass
            elif ls.startswith('offset ') and 'offset' not in d:
                try: d['offset'] = int(ls.split()[1])
                except Exception: pass
        if 'sect' in d and 'addr' in d and 'size' in d and 'offset' in d:
            sections.append(d)
    return sections

def main():
    path = sys.argv[1]
    addrs = [int(a, 16) for a in sys.argv[2:]]
    data = open(path, 'rb').read()
    secs = load_sections(path)

    def find_sec(addr):
        for s in secs:
            if s['addr'] <= addr < s['addr'] + s['size']:
                return s
        return None

    def read_bytes(addr, n):
        s = find_sec(addr)
        if not s: return None
        off = s['offset'] + (addr - s['addr'])
        return data[off:off+n], s

    def read_ptr(addr):
        r = read_bytes(addr, 8)
        if not r: return None
        v = struct.unpack('<Q', r[0])[0]
        if v >> 44:          # chained fixup encoded
            v &= 0xFFFFFFFFFF
        return v

    def read_cstr(addr):
        s = find_sec(addr)
        if not s: return None
        off = s['offset'] + (addr - s['addr'])
        end = data.find(b'\0', off)
        if end < 0: return None
        return data[off:end].decode('utf-8', 'replace'), s['sect']

    for a in addrs:
        print(f"--- data @ 0x{a:x} ---")
        sec = find_sec(a)
        print("  section:", sec['seg'] + ',' + sec['sect'] if sec else None)
        v = read_ptr(a)
        print("  value:", hex(v) if v else None)
        if v:
            c = read_cstr(v)
            if c and c[0].isprintable():
                print("  points-to-string:", repr(c[0]), "in", c[1])

if __name__ == '__main__':
    main()
