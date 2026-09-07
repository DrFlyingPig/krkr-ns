#!/usr/bin/env python3
"""Inspect or extract files from a standard XP3 archive.

This intentionally does not apply game-specific XP3 content filters.  It is a
small diagnostics tool for the engine port, not a game conversion pipeline.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


XP3_MAGIC = b"XP3\r\n \n\x1a\x8bg\x01"


def chunks(data: bytes):
    offset = 0
    while offset + 12 <= len(data):
        name = data[offset : offset + 4]
        size = struct.unpack_from("<Q", data, offset + 4)[0]
        start = offset + 12
        end = start + size
        if end > len(data):
            raise ValueError(f"chunk {name!r} exceeds index boundary")
        yield name, data[start:end]
        offset = end


def read_index_blocks(handle):
    handle.seek(len(XP3_MAGIC))
    while True:
        (index_offset,) = struct.unpack("<Q", handle.read(8))
        handle.seek(index_offset)
        flag = handle.read(1)[0]
        method = flag & 0x07
        if method == 1:
            compressed_size, original_size = struct.unpack("<QQ", handle.read(16))
            index = zlib.decompress(handle.read(compressed_size))
            if len(index) != original_size:
                raise ValueError("decompressed index size mismatch")
        elif method == 0:
            (original_size,) = struct.unpack("<Q", handle.read(8))
            index = handle.read(original_size)
        else:
            raise ValueError(f"unsupported XP3 index encoding: {method}")
        yield index
        if not flag & 0x80:
            break


def entries(path: Path):
    with path.open("rb") as handle:
        if handle.read(len(XP3_MAGIC)) != XP3_MAGIC:
            raise ValueError("not a standard XP3 archive")
        for index in read_index_blocks(handle):
            for chunk_name, file_data in chunks(index):
                if chunk_name != b"File":
                    continue
                info = next(
                    (payload for name, payload in chunks(file_data) if name == b"info"),
                    None,
                )
                segments = next(
                    (payload for name, payload in chunks(file_data) if name == b"segm"),
                    None,
                )
                if info is None or segments is None or len(info) < 22:
                    continue
                original_size, archived_size = struct.unpack_from("<QQ", info, 4)
                (name_length,) = struct.unpack_from("<H", info, 20)
                name = info[22 : 22 + name_length * 2].decode("utf-16le")
                yield name, original_size, archived_size, segments


def extract(path: Path, segments: bytes) -> bytes:
    output = bytearray()
    with path.open("rb") as handle:
        for offset in range(0, len(segments), 28):
            flags, archive_offset, original_size, archived_size = struct.unpack_from(
                "<IQQQ", segments, offset
            )
            handle.seek(archive_offset)
            payload = handle.read(archived_size)
            if len(payload) != archived_size:
                raise ValueError("segment exceeds archive boundary")
            method = flags & 0x07
            if method == 1:
                payload = zlib.decompress(payload)
            elif method != 0:
                raise ValueError(f"unsupported segment encoding: {method}")
            if len(payload) != original_size:
                raise ValueError("decompressed segment size mismatch")
            output.extend(payload)
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--match", default="", help="case-insensitive name substring")
    parser.add_argument("--extract", help="exact archive path to extract")
    parser.add_argument("--output", type=Path, help="destination for --extract")
    args = parser.parse_args()

    if args.extract and not args.output:
        parser.error("--extract requires --output")

    needle = args.match.casefold()
    count = 0
    total_original = 0
    extracted = False
    for name, original_size, archived_size, segments in entries(args.archive):
        count += 1
        total_original += original_size
        if args.extract == name:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(extract(args.archive, segments))
            print(f"extracted {name} -> {args.output} ({original_size} bytes)")
            extracted = True
        if not needle or needle in name.casefold():
            print(f"{original_size:>10} {archived_size:>10} {len(segments) // 28:>3}  {name}")
    print(f"entries={count} original_bytes={total_original}")
    if args.extract and not extracted:
        raise SystemExit(f"archive entry not found: {args.extract}")


if __name__ == "__main__":
    main()
