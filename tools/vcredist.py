#!/usr/bin/env python3
"""Extracts runtime DLLs from a Visual C++ redistributable installer.

    python3 -I tools/vcredist.py VC_redist.x64.exe OUT_DIR [DLL ...]

Games ship Microsoft's redistributable (VC_redist.x64.exe, a WiX Burn
bundle) next to themselves; its runtime DLLs can be lifted with a game's
own modules (GameConfig::lifted_dlls with a host_path). The bundle carries
its payload as cabinets appended to the EXE, and those hold the DLLs in
further cabinets, under names such as "msvcp140.dll_amd64". This finds
every cabinet (MSCF signature) in the file, opens nested ones, and writes
each requested DLL (default: msvcp140.dll) of the bundle's architecture
(amd64 for x64, x86 for 32-bit) to OUT_DIR under its own name.

Pure Python (zlib only): cabinets here use MSZIP, deflate blocks that
share one 32 KiB window across a folder. Other compression types (LZX,
Quantum) are refused.
"""
import os
import struct
import sys
import zlib


def cab_files(data):
    """{name: bytes} of a cabinet's files (MSZIP or uncompressed folders)."""
    sig, _, size, _, files_off, _, _, _, nfolders, nfiles, flags = struct.unpack_from(
        "<4sIIIIIBBHHH", data, 0)
    if sig != b"MSCF":
        raise ValueError("not a cabinet")
    o = 36
    hdr_res = folder_res = data_res = 0
    if flags & 4:  # cfhdrRESERVE_PRESENT
        hdr_res, folder_res, data_res = struct.unpack_from("<HBB", data, o)
        o += 4 + hdr_res
    if flags & 1:  # cfhdrPREV_CABINET: two names to skip
        for _ in range(2):
            o = data.index(b"\0", o) + 1
    if flags & 2:  # cfhdrNEXT_CABINET
        for _ in range(2):
            o = data.index(b"\0", o) + 1
    folders = []
    for _ in range(nfolders):
        first, ndata, comp = struct.unpack_from("<IHH", data, o)
        folders.append((first, ndata, comp))
        o += 8 + folder_res
    # Each folder's uncompressed stream.
    streams = []
    for first, ndata, comp in folders:
        kind = comp & 0xF
        if kind not in (0, 1):
            raise ValueError(f"cabinet compression {kind} (only none and MSZIP)")
        out = bytearray()
        p = first
        for _ in range(ndata):
            _, cb, _ = struct.unpack_from("<IHH", data, p)
            block = data[p + 8 + data_res:p + 8 + data_res + cb]
            p += 8 + data_res + cb
            if kind == 0:
                out += block
                continue
            if block[:2] != b"CK":
                raise ValueError("MSZIP block without its CK signature")
            # Each block is a complete deflate stream whose window starts
            # with the folder's previous 32 KiB of output.
            d = zlib.decompressobj(-15, zdict=bytes(out[-32768:]))
            out += d.decompress(block[2:]) + d.flush()
        streams.append(bytes(out))
    files = {}
    o = files_off
    for _ in range(nfiles):
        fsize, foff, ifolder, _, _, _ = struct.unpack_from("<IIHHHH", data, o)
        name_end = data.index(b"\0", o + 16)
        name = data[o + 16:name_end].decode("latin1")
        o = name_end + 1
        if ifolder < len(streams):
            files[name] = streams[ifolder][foff:foff + fsize]
    return files


def all_files(blob, depth=0):
    """Every file of every cabinet in blob, nested cabinets opened."""
    found = {}
    i = 0
    while True:
        i = blob.find(b"MSCF\0\0\0\0", i)
        if i < 0:
            break
        size = struct.unpack_from("<I", blob, i + 8)[0]
        if size < 36 or size > len(blob) - i:
            i += 4
            continue
        try:
            files = cab_files(blob[i:i + size])
        except (ValueError, struct.error, zlib.error):
            i += 4
            continue
        for name, content in files.items():
            if content[:4] == b"MSCF" and depth < 4:
                found.update(all_files(content, depth + 1))
            else:
                found[name] = content
        i += size
    return found


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bundle, out_dir = sys.argv[1], sys.argv[2]
    wanted = [n.lower() for n in sys.argv[3:]] or ["msvcp140.dll"]
    arch = "x86" if ".x86." in os.path.basename(bundle).lower() else "amd64"
    files = all_files(open(bundle, "rb").read())
    os.makedirs(out_dir, exist_ok=True)
    missing = []
    for dll in wanted:
        content = files.get(f"{dll}_{arch}")
        if content is None:
            missing.append(dll)
            continue
        with open(os.path.join(out_dir, dll), "wb") as f:
            f.write(content)
        print(f"{dll} ({arch}): {len(content)} bytes")
    if missing:
        sys.exit(f"not in {bundle}: {', '.join(missing)}")


if __name__ == "__main__":
    main()
