"""Package generated, non-game test data into a copy of an NRO's RomFS.

Usage: python tests/build_menu_perf_fixture.py --nro path --output-dir directory
Requires the local devkitPro build_romfs and 7-Zip; configurable below.
The input NRO and game/save directories are never modified.
"""
import argparse
import random
import shutil
import struct
import subprocess
import zlib
from pathlib import Path


def array(values):
    count_width = max(1, (len(values).bit_length() + 7) // 8)
    width = max(1, (max(values, default=0).bit_length() + 7) // 8)
    return bytes([0x0C + count_width]) + len(values).to_bytes(count_width, "little") + bytes([0x0C + width]) + b"".join(x.to_bytes(width, "little") for x in values)


def bmp(color):
    w = h = 128
    pixels = struct.pack("<I", color | 0xFF000000) * w * h
    return b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(pixels), 0, 0, 0, 0) + pixels


def psb():
    resources = [bmp(0x30A060 + i) for i in range(16)]
    names = [f"{i}.bmp".encode() + b"\0" for i in range(len(resources))]
    data = bytearray(40)
    def add(blob):
        offset = len(data)
        data.extend(blob)
        return offset
    positions, pos = [], 0
    for n in names:
        positions.append(pos)
        pos += len(n)
    name_index = add(array(positions))
    name_data = add(b"".join(names))
    strings = add(array([]))
    string_data = len(data)
    offsets, pos = [], 0
    for resource in resources:
        offsets.append(pos)
        pos += len(resource)
    chunk_offsets = add(array(offsets))
    chunk_lengths = add(array([len(r) for r in resources]))
    entries = add(b"\x21" + array(list(range(16))) + array([i * 2 for i in range(16)]) + b"".join(bytes([0x19, i]) for i in range(16)))
    chunk_data = add(b"".join(resources))
    struct.pack_into("<4sHH8I", data, 0, b"PSB\0", 1, 0, name_index, name_data, strings, string_data, chunk_offsets, chunk_lengths, chunk_data, entries)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sevenzip", default=r"C:\Program Files\7-Zip\7z.exe")
    parser.add_argument("--romfs-tool", default=r"D:\devkitPro\tools\bin\build_romfs.exe")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    romfs = out / "romfs"
    romfs.mkdir(exist_ok=True)
    shutil.copy2(repo / "tests/fixtures/menu_perf/startup.tjs", romfs / "startup.tjs")
    shutil.copy2(repo / "krkrsdl2/data/notosanssc.ttf", romfs / "notosanssc.ttf")
    raw = psb()
    (romfs / "fixture.pimg").write_bytes(raw)
    (romfs / "text.psb").write_bytes(text_psb())
    (romfs / "wrapped.pimg").write_bytes(b"mdf\0" + struct.pack("<I", len(raw)) + zlib.compress(raw))
    members = out / "members"
    members.mkdir(exist_ok=True)
    rng = random.Random(1739)
    # A 4 MiB solid block: adjacent members should not each decode the block.
    for i in range(32):
        tail = "".join(chr(65 + rng.randrange(26)) for _ in range(65500))
        (members / f"part{i:02}.txt").write_bytes(b"\xff\xfe" + (f"part={i}\r\n" + tail).encode("utf-16le"))
    subprocess.run([args.sevenzip, "a", "-t7z", "-m0=lzma2", "-ms=on", "-mx=1", str(romfs / "solid.xp3"), "*.txt"], cwd=members, check=True, stdout=subprocess.DEVNULL)
    image = out / "test.romfs"
    subprocess.run([args.romfs_tool, str(romfs), str(image)], check=True, stdout=subprocess.DEVNULL)
    # libnx switch/nro.h: NroStart (16), NroHeader.size (+8), then
    # NroAssetHeader { magic, version, icon, nacp, romfs }. Offsets are relative
    # to the asset header. Preserve code, build ID, icon and application metadata.
    data = args.nro.read_bytes()
    assert data[16:20] == b"NRO0"
    asset = struct.unpack_from("<I", data, 24)[0]
    assert data[asset:asset + 4] == b"ASET"
    romfs_offset, _ = struct.unpack_from("<QQ", data, asset + 40)
    output = bytearray(data[:asset + romfs_offset])
    replacement = image.read_bytes()
    output.extend(replacement)
    struct.pack_into("<QQ", output, asset + 40, romfs_offset, len(replacement))
    assert output[:asset] == data[:asset]
    target = out / "menu-perf.nro"
    target.write_bytes(output)
    print(target)


def text_psb():
    """A large UI-like dictionary with many references to shared strings."""
    count = 10000
    data = bytearray(40)
    def add(blob):
        offset = len(data)
        data.extend(blob)
        return offset
    names = [f"item{i}".encode() + b"\0" for i in range(count)]
    positions, pos = [], 0
    for name in names:
        positions.append(pos)
        pos += len(name)
    name_index = add(array(positions))
    name_data = add(b"".join(names))
    values = [b"\0", "共用文字 café\0".encode(), ("value" * 80 + "\0").encode()]
    string_index = add(array([0, len(values[0]), len(values[0]) + len(values[1])]))
    string_data = add(b"".join(values))
    chunks = add(array([]))
    entries = add(b"\x21" + array(list(range(count))) + array([i * 2 for i in range(count)]) + b"".join(bytes([0x15, i % 3]) for i in range(count)))
    struct.pack_into("<4sHH8I", data, 0, b"PSB\0", 1, 0, name_index, name_data, string_index, string_data, chunks, chunks, len(data), entries)
    return bytes(data)


if __name__ == "__main__":
    main()
