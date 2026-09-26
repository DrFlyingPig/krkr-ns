"""Package the native storage-move regression in a separate NRO copy."""

import argparse
import shutil
import struct
import subprocess
from pathlib import Path

from build_core_port_fixture import replace_romfs
from build_xp3_filter_fixture import XP3_MAGIC, file_chunk


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--romfs-tool", default=r"D:\devkitPro\tools\bin\build_romfs.exe")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    out = args.output_dir.resolve()
    romfs = out / "romfs"
    romfs.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / "tests/fixtures/storage_move/startup.tjs", romfs / "startup.tjs")
    payload = b"immutable archive payload"
    offset = len(XP3_MAGIC) + 8
    index = file_chunk("payload.txt", payload, offset)
    archive = (XP3_MAGIC + struct.pack("<Q", offset + len(payload)) + payload
               + b"\x00" + struct.pack("<Q", len(index)) + index)
    (romfs / "contract.xp3").write_bytes(archive)
    image = out / "storage-move.romfs"
    subprocess.run([args.romfs_tool, str(romfs), str(image)], check=True)
    target = out / "storage-move.nro"
    replace_romfs(args.nro.resolve(), image, target)
    print(target)


if __name__ == "__main__":
    main()
