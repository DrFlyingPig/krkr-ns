"""Package a self-contained contract startup script into a copy of an NRO.

Usage:
    python tests/build_core_port_fixture.py --nro build-switch/krkrsdl2.nro \
        --output-dir build-core-port-fixture

The input NRO is not modified. RomFS includes the fixture script and font,
plus the bundled compatibility scripts or explicit media required by the fixture.
scenario_restart and artwork_png also generate an sdcard tree with synthetic
games or images; copy that tree into an isolated test SD root. No fixture reads
real games or saves.
"""

import argparse
import shutil
import struct
import subprocess
import zlib
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


def build_scenario_restart_games(source: Path, output_dir: Path) -> None:
    # Reuse XP3 metadata encoding, leaving the payloads unencrypted. Both games
    # request first.ks, but the native parser must see a different tag in each.
    from build_xp3_filter_fixture import XP3_MAGIC, file_chunk

    base = output_dir / "sdcard/switch/KRKR-ns/Game"
    template = (source / "game_startup.tjs").read_text(encoding="utf-8")
    for directory, tag, color in (
        ("ScenarioA", "session_a", "0xff602030"),
        ("ScenarioB", "session_b", "0xff205040"),
    ):
        startup = template.replace("@TAG@", tag).replace("@COLOR@", color)
        files = [("startup.tjs", startup.encode("utf-8")),
                 ("first.ks", ("[" + tag + "]\n").encode("utf-8"))]
        offset = len(XP3_MAGIC) + 8
        index = bytearray()
        for name, payload in files:
            index.extend(file_chunk(name, payload, offset))
            offset += len(payload)
        archive = XP3_MAGIC + struct.pack("<Q", offset)
        archive += b"".join(payload for _, payload in files)
        archive += b"\x00" + struct.pack("<Q", len(index)) + index
        target = base / directory / "data.xp3"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive)
    (base.parent / "scenario-restart-round.txt").write_text("0\n", encoding="utf-8")


def build_artwork_png_images(output_dir: Path) -> None:
    # The third page includes a valid PNG with optional virtual-page metadata.
    # These generated pixels keep the fixture independent of any real game.
    game = output_dir / "sdcard/switch/KRKR-ns/Game/ArtworkFixture"
    game.mkdir(parents=True, exist_ok=True)
    (game / "startup.tjs").write_text('// Artwork fixture; no game is launched.\n', encoding="utf-8")

    def chunk(name, payload):
        return (struct.pack(">I", len(payload)) + name + payload +
                struct.pack(">I", zlib.crc32(name + payload) & 0xffffffff))

    width, height = 96, 64
    for index in range(1, 19):
        pixel = bytes((40 + index * 7, 170 - index * 4, 80 + index * 5, 255))
        raw = (b"\0" + pixel * width) * height
        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        if index == 13:
            png += chunk(b"vpAg", struct.pack(">IIB", width, height, 0))
        png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
        (game / f"{index:02d}.png").write_bytes(png)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--fixture", choices=("core_port", "menu_contract", "menu_game_layer", "menu_ui", "menu_native", "scenario_restart", "artwork_png", "movie_contract", "movie_seek", "movie_seek_overlay", "layer_raster", "layer_input_lifetime", "movie_audio"),
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
    if args.fixture in ("menu_contract", "menu_game_layer", "menu_ui", "menu_native", "scenario_restart", "artwork_png"):
        compat = romfs / "compat/system"
        compat.mkdir(parents=True, exist_ok=True)
        # ScriptMgnIntf also invokes the namespace reinstall hook when this
        # basename is executed, including in a self-contained test package.
        for name in ("k2compat.tjs", "k2compat_reinstall.tjs", "menu_popup.tjs"):
            shutil.copy2(repo / "compat-patches/system" / name, compat / name)
    if args.fixture == "scenario_restart":
        build_scenario_restart_games(repo / "tests/fixtures/scenario_restart", output_dir)
    if args.fixture == "artwork_png":
        shutil.copy2(repo / "krkrsdl2/data/startup.tjs", romfs / "launcher-main.tjs")
        shutil.copytree(repo / "krkrsdl2/data/launcher", romfs / "launcher", dirs_exist_ok=True)
        build_artwork_png_images(output_dir)
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
