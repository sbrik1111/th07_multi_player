"""Name the functions of addresses from a CRASH line with the build's map.

    python tools/mapaddr.py build/th16.map +082D05 0x482d05 ...

"+offset" is from the image base 0x400000 (the CRASH line's form).
"""
import re
import sys


def main():
    path = sys.argv[1]
    syms = []
    for line in open(path, encoding="latin-1"):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8})\s+", line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    for arg in sys.argv[2:]:
        va = 0x400000 + int(arg[1:], 16) if arg.startswith("+") else int(arg, 16)
        best = None
        for addr, name in syms:
            if addr <= va:
                best = (addr, name)
            else:
                break
        print(f"{va:08x}: {best[1]} +0x{va - best[0]:x}" if best else f"{va:08x}: ?")


if __name__ == "__main__":
    main()
