"""Inspect the local ISO and pin the exact USA inputs supported by the AOT port."""
import hashlib
import json
import pathlib
import struct
import sys


def inventory(path):
    found = {}
    with open(path, "rb") as f:
        def read(offset, size):
            f.seek(offset)
            return f.read(size)
        pvd = read(16 * 2048, 2048)
        assert pvd[:7] == b"\x01CD001\x01"
        def walk(record, parent=""):
            extent, size = struct.unpack_from("<I", record, 2)[0], struct.unpack_from("<I", record, 10)[0]
            data = read(extent * 2048, size)
            pos = 0
            while pos < len(data):
                n = data[pos]
                if not n:
                    pos = (pos // 2048 + 1) * 2048
                    continue
                r = data[pos:pos+n]
                pos += n
                name = r[33:33+r[32]]
                if name in (b"\0", b"\1"):
                    continue
                name = name.decode("ascii").split(";")[0]
                full = parent + name
                if r[25] & 2:
                    walk(r, full + "/")
                else:
                    found[full] = (struct.unpack_from("<I", r, 2)[0] * 2048, struct.unpack_from("<I", r, 10)[0])
        walk(pvd[156:190])
        result = []
        for name, (offset, size) in sorted(found.items()):
            if not (name.upper().startswith("PSP_GAME/USRDIR/") or name.upper() == "PSP_GAME/PARAM.SFO"):
                continue
            digest = hashlib.sha256()
            f.seek(offset)
            left = size
            while left:
                block = f.read(min(left, 1024 * 1024))
                if not block:
                    raise ValueError("Truncated ISO")
                digest.update(block)
                left -= len(block)
            result.append(dict(path=name, size=size, sha256=digest.hexdigest()))
            print(name, size, digest.hexdigest())
        return result


if __name__ == "__main__":
    result = inventory(sys.argv[1])
    pathlib.Path(sys.argv[2]).write_text(json.dumps(result, indent=2) + "\n")
