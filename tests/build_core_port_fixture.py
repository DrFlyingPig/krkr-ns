"""Package a self-contained contract startup script into a copy of an NRO.

Usage:
    python tests/build_core_port_fixture.py --nro build-switch/krkrsdl2.nro \
        --output-dir build-core-port-fixture

The input NRO is not modified. RomFS includes the fixture script and font,
plus the bundled compatibility scripts or explicit synthetic media required
by the selected fixture. The startup scripts never read game directories or saves.
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
    parser.add_argument("--fixture", choices=("core_port", "menu_contract", "menu_game_layer", "menu_ui", "menu_native", "movie_contract", "movie_seek", "movie_seek_overlay", "layer_raster", "movie_audio"),
                        default="core_port")
    parser.add_argument("--media", type=Path,
                        help="Local movie to include as sample.mp4 in a movie fixture")
    parser.add_argument(
        "--romfs-tool", default=r"D:\devkitPro\tools\bin\build_romfs.exe"
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    output_dir = args.output_dir.resolve()
    romfs = output_dir / "romfs"
    romfs.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / "tests/fixtures" / args.fixture / "startup.tjs", romfs / "startup.tjs")
    shutil.copy2(repo / "krkrsdl2/data/notosanssc.ttf", romfs / "notosanssc.ttf")
    if args.fixture in ("menu_contract", "menu_game_layer", "menu_ui", "menu_native"):
        compat = romfs / "compat/system"
        compat.mkdir(parents=True, exist_ok=True)
        # ScriptMgnIntf also invokes the namespace reinstall hook when this
        # basename is executed, including in a self-contained test package.
        for name in ("k2compat.tjs", "k2compat_reinstall.tjs", "menu_popup.tjs"):
            shutil.copy2(repo / "compat-patches/system" / name, compat / name)
    if args.fixture in ("movie_contract", "movie_seek", "movie_seek_overlay", "movie_audio"):
        if args.media is None:
            parser.error(args.fixture + " requires --media pointing to a local test movie")
        shutil.copy2(args.media, romfs / "sample.mp4")

    stem = "core-port" if args.fixture == "core_port" else args.fixture.replace("_", "-")
    image = output_dir / (stem + ".romfs")
    subprocess.run([args.romfs_tool, str(romfs), str(image)], check=True)
    target = output_dir / (stem + ".nro")
    replace_romfs(args.nro.resolve(), image, target)
    print(target)


if __name__ == "__main__":
    main()
