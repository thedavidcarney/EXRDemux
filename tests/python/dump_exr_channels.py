"""
dump_exr_channels.py

Print every channel of an OpenEXR file in the order it appears in the file
header. Pure stdlib — no `pip install` required, runs in system Python or
Blender's bundled Python equally well.

Usage:
    python dump_exr_channels.py path/to/file.exr

Output:
    Index  Channel name                              Pixel type
    -----  ----------------------------------------  ----------
        0  A                                         HALF
        1  B                                         HALF
        ...

The point of this script: get a deterministic channel list from the EXR file
itself, so we can compare it to what AE's EXtractoR shows in its dropdown.

This parser handles single-part scanline/tile EXRs (which is what Blender's
OPEN_EXR_MULTILAYER format produces). Multipart and deep EXRs will print a
warning and parse only the first header.
"""

import struct
import sys


MAGIC = b"\x76\x2f\x31\x01"
PIXEL_TYPE_NAMES = {0: "UINT", 1: "HALF", 2: "FLOAT"}


def _read_null_string(buf, offset):
    end = buf.index(b"\x00", offset)
    return buf[offset:end].decode("utf-8", errors="replace"), end + 1


def _read_int32(buf, offset):
    return struct.unpack_from("<i", buf, offset)[0], offset + 4


def parse_exr_channels(path):
    with open(path, "rb") as f:
        buf = f.read()

    if buf[:4] != MAGIC:
        raise ValueError("Not an OpenEXR file (magic mismatch)")

    version_field = struct.unpack_from("<i", buf, 4)[0]
    version = version_field & 0xFF
    flags = version_field & 0xFFFFFF00

    is_multipart = bool(flags & 0x1000)
    is_deep = bool(flags & 0x0800)
    is_tiled = bool(flags & 0x0200)

    if is_multipart:
        print("WARNING: multipart file — parsing first part only.")
    if is_deep:
        print("WARNING: deep EXR — channel parse may be incomplete.")

    offset = 8
    channels = []

    while offset < len(buf):
        if buf[offset] == 0:
            break  # end of header

        name, offset = _read_null_string(buf, offset)
        attr_type, offset = _read_null_string(buf, offset)
        size, offset = _read_int32(buf, offset)
        attr_end = offset + size

        if name == "channels" and attr_type == "chlist":
            cur = offset
            while cur < attr_end:
                if buf[cur] == 0:
                    cur += 1
                    break
                ch_name, cur = _read_null_string(buf, cur)
                pix_type, cur = _read_int32(buf, cur)
                cur += 4  # pLinear (1 byte) + 3 reserved bytes
                _xs, cur = _read_int32(buf, cur)
                _ys, cur = _read_int32(buf, cur)
                channels.append((ch_name, pix_type))

        offset = attr_end

    return {
        "version": version,
        "flags": flags,
        "tiled": is_tiled,
        "multipart": is_multipart,
        "deep": is_deep,
        "channels": channels,
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    info = parse_exr_channels(path)
    channels = info["channels"]

    print("File          : {}".format(path))
    print("EXR version   : {}".format(info["version"]))
    print("Tiled         : {}".format(info["tiled"]))
    print("Multipart     : {}".format(info["multipart"]))
    print("Deep          : {}".format(info["deep"]))
    print("Channel count : {}".format(len(channels)))
    print()
    print("Index  Channel name                                                  Pixel type")
    print("-----  ------------------------------------------------------------  ----------")
    for i, (name, ptype) in enumerate(channels):
        print("{:>5}  {:<60}  {}".format(i, name, PIXEL_TYPE_NAMES.get(ptype, ptype)))

    print()
    print("Tip: paste this list (just the channel names, in order) into")
    print("CHANNEL_LIST in tests/jsx/03_extract_all_layers.jsx.")


if __name__ == "__main__":
    main()
