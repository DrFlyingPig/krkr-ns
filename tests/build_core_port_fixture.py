"""Package the self-contained core-port startup script into a copy of an NRO.

Usage:
    python tests/build_core_port_fixture.py --nro build-switch/krkrsdl2.nro \
        --output-dir build-core-port-fixture

The input NRO is not modified.  The result contains only the regression
startup script and bundled font in RomFS, so it never reads game data.
"""

import argparse
import shutil
import struct
import subprocess
from pathlib import Path


def replace_romfs(nro: Path, image: Path, target: Path) -> None:
    data = nro.read_bytes()
    assert data[16:20] == b"NRO0", "input is not an NRO"
    asset = struct.unpack_from("<I", data, 24)[0]
    assert data[asset:asset + 4] == b"ASET", "NRO has no asset header"
    romfs_offset, _ = struct.unpack_from("<QQ", data, asset + 40)
    output = bytearray(data[:asset + romfs_offset])
    replacement = image.read_bytes()
    output.extend(replacement)
    struct.pack_into("<QQ", output, asset + 40, romfs_offset, len(replacement))
    assert output[:asset] == data[:asset]
    target.write_bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--romfs-tool", default=r"D:\devkitPro\tools\bin\build_romfs.exe"
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    output_dir = args.output_dir.resolve()
    romfs = output_dir / "romfs"
    romfs.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / "tests/fixtures/core_port/startup.tjs", romfs / "startup.tjs")
    shutil.copy2(repo / "krkrsdl2/data/notosanssc.ttf", romfs / "notosanssc.ttf")

    image = output_dir / "core-port.romfs"
    subprocess.run([args.romfs_tool, str(romfs), str(image)], check=True)
    target = output_dir / "core-port.nro"
    replace_romfs(args.nro.resolve(), image, target)
    print(target)


if __name__ == "__main__":
    main()
