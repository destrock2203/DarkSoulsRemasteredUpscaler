"""Sets one s32 field of rows in a Dark Souls Remastered *.parambnd.dcx, in place.

DCX (DFLT / zlib) -> BND3 -> PARAM. The value keeps its size, so the BND3 and PARAM layouts are
unchanged: only the 4 bytes change, then the BND3 is recompressed and the DCX sizes updated.

    python dsr_param.py <file.parambnd.dcx> <ParamName> <rowId[,rowId...]> <byteOffset> <value> [--expect V]

Example (no death penalty: the effects always on the player, "no resonance" 40 and resonance levels
41-44, each add the Rare Ring of Sacrifice's effect 2131 when they expire, i.e. every few seconds):
    python dsr_param.py GameParam.parambnd.dcx SpEffectParam 40,41,42,43,44 292 2131 --expect -1
"""

import argparse
import struct
import sys
import zlib


def read_dcx(raw):
    if raw[:4] != b"DCX\0" or raw[0x28:0x2C] != b"DFLT":
        sys.exit("not a DCX_DFLT file")
    dca = raw.index(b"DCA\0")
    data = zlib.decompress(raw[dca + 8:])
    dcs = raw.index(b"DCS\0")
    if struct.unpack_from(">I", raw, dcs + 4)[0] != len(data):
        sys.exit("DCX uncompressed size mismatch")
    return raw[:dca + 8], data


def write_dcx(header, data):
    packed = zlib.compress(bytes(data), 9)
    header = bytearray(header)
    dcs = header.index(b"DCS\0")
    struct.pack_into(">II", header, dcs + 4, len(data), len(packed))
    return bytes(header) + packed


def find_param(bnd, name):
    if bnd[:4] != b"BND3" or bnd[0xC] != 0x74 or bnd[0xD] != 0:
        sys.exit("unexpected BND3 format")
    count = struct.unpack_from("<i", bnd, 0x10)[0]
    for i in range(count):
        _, size, offset, _, name_offset, _ = struct.unpack_from("<6i", bnd, 0x20 + i * 0x18)
        entry = bnd[name_offset:bnd.index(b"\0", name_offset)].decode("cp932")
        if entry.rsplit("\\", 1)[-1] == name + ".param":
            return offset, size
    sys.exit(f"{name}.param not found")


def find_row(bnd, param_offset, row_id):
    rows = struct.unpack_from("<H", bnd, param_offset + 0xA)[0]
    for r in range(rows):
        rid, data_offset, _ = struct.unpack_from("<iII", bnd, param_offset + 0x30 + r * 12)
        if rid == row_id:
            return param_offset + data_offset
    sys.exit(f"row {row_id} not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("param")
    ap.add_argument("rows", type=lambda v: [int(r) for r in v.split(",")])
    ap.add_argument("offset", type=int)
    ap.add_argument("value", type=int)
    ap.add_argument("--expect", type=int, help="refuse unless the field currently holds this value")
    a = ap.parse_args()

    raw = open(a.file, "rb").read()
    header, data = read_dcx(raw)
    data = bytearray(data)
    param_offset, _ = find_param(data, a.param)
    changed = False
    for row in a.rows:
        at = find_row(data, param_offset, row) + a.offset
        old = struct.unpack_from("<i", data, at)[0]
        if old == a.value:
            print(f"{a.param}[{row}]+{a.offset} already {a.value}")
            continue
        if a.expect is not None and old != a.expect:
            sys.exit(f"{a.param}[{row}]+{a.offset} is {old}, expected {a.expect}: nothing patched")
        struct.pack_into("<i", data, at, a.value)
        print(f"{a.param}[{row}]+{a.offset}: {old} -> {a.value}")
        changed = True
    if not changed:
        return
    out = write_dcx(header, data)
    if read_dcx(out)[1] != data:
        sys.exit("round trip failed")
    open(a.file, "wb").write(out)


if __name__ == "__main__":
    main()
