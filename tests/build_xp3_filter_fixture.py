"""Build a self-contained encrypted XP3 runtime-contract fixture.

Usage:
    python tests/build_xp3_filter_fixture.py \
        --output-dir build-xp3-filter-fixture

The output game directory can be copied directly below the launcher's Game
directory.  ``xp3filter.tjs`` remains beside the archive, while both archived
TJS files are XOR-encrypted with a per-file key supplied through the content
filter context.  A successful startup therefore proves the content callback,
``[action, context]``, action=1 materialization, six extraction arguments, and
the filename/context lifetime as one end-to-end contract.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


XP3_MAGIC = b"XP3\r\n \n\x1a\x8bg\x01"
GAME_DIRECTORY = "000_XP3_CONTRACT"
ARCHIVE_NAME = "contract.xp3"


def chunk(name: bytes, payload: bytes) -> bytes:
    if len(name) != 4:
        raise ValueError("XP3 chunk names must contain four bytes")
    return name + struct.pack("<Q", len(payload)) + payload


def xor_key(size: int) -> int:
    key = (size ^ 0x5A) & 0xFF
    return key or 0xA5


def encrypt(payload: bytes) -> bytes:
    key = xor_key(len(payload))
    return bytes(value ^ key for value in payload)


def file_chunk(name: str, payload: bytes, archive_offset: int) -> bytes:
    encoded_name = name.encode("utf-16le")
    info = struct.pack(
        "<IQQH", 0, len(payload), len(payload), len(encoded_name) // 2
    ) + encoded_name
    segment = struct.pack(
        "<IQQQ", 0, archive_offset, len(payload), len(payload)
    )
    checksum = struct.pack("<I", zlib.adler32(payload) & 0xFFFFFFFF)
    return chunk(
        b"File",
        chunk(b"info", info) + chunk(b"segm", segment) + chunk(b"adlr", checksum),
    )


def build_archive(files: list[tuple[str, bytes]]) -> bytes:
    header_size = len(XP3_MAGIC) + 8
    encrypted_files: list[tuple[str, bytes, bytes, int]] = []
    offset = header_size
    for name, payload in files:
        encrypted = encrypt(payload)
        encrypted_files.append((name, payload, encrypted, offset))
        offset += len(encrypted)

    index = b"".join(
        file_chunk(name, payload, archive_offset)
        for name, payload, _encrypted, archive_offset in encrypted_files
    )
    archive = bytearray(XP3_MAGIC)
    archive.extend(struct.pack("<Q", offset))
    for _name, _payload, encrypted, _archive_offset in encrypted_files:
        archive.extend(encrypted)
    archive.extend(b"\x00")  # raw index, no continuation block
    archive.extend(struct.pack("<Q", len(index)))
    archive.extend(index)
    return bytes(archive)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    source = repo / "tests" / "fixtures" / "xp3_filter"
    output = args.output_dir.resolve() / GAME_DIRECTORY
    output.mkdir(parents=True, exist_ok=True)

    startup = (source / "startup.tjs").read_bytes()
    payload = (source / "payload.tjs").read_bytes()
    payload += b"\n/* action=1 padding\n" + (b"0123456789abcdef" * 1024) + b"\n*/\n"
    archive = build_archive([
        ("startup.tjs", startup),
        ("payload.tjs", payload),
    ])

    archive_path = output / ARCHIVE_NAME
    archive_path.write_bytes(archive)
    filter_path = output / "xp3filter.tjs"
    filter_path.write_bytes((source / "xp3filter.tjs").read_bytes())

    # Builder-side invariants complement the runtime assertions without
    # pretending to replace them.
    if archive[: len(XP3_MAGIC)] != XP3_MAGIC:
        raise AssertionError("XP3 magic mismatch")
    for name, plain in (("startup.tjs", startup), ("payload.tjs", payload)):
        cipher = encrypt(plain)
        if encrypt(cipher) != plain:
            raise AssertionError(f"XOR round trip failed for {name}")

    print(archive_path)
    print(filter_path)


if __name__ == "__main__":
    main()
