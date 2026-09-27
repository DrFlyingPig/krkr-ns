"""Package a natural audio completion test without changing the input NRO.

Synthetic media is generated with ffmpeg into the chosen build directory.
An optional local game sample can be included for diagnosis; it is never
stored under tests or needed by the reusable regression fixture.
"""

import argparse
import shutil
import struct
import subprocess
from pathlib import Path

from build_core_port_fixture import replace_romfs
from build_xp3_filter_fixture import XP3_MAGIC, file_chunk


def build_plain_archive(files: list[tuple[str, bytes]]) -> bytes:
    offset = len(XP3_MAGIC) + 8
    index = bytearray()
    data = bytearray()
    for name, payload in files:
        index.extend(file_chunk(name, payload, offset))
        data.extend(payload)
        offset += len(payload)
    return (XP3_MAGIC + struct.pack("<Q", offset) + data + b"\x00"
            + struct.pack("<Q", len(index)) + index)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--game-sample", type=Path)
    parser.add_argument("--launcher-cycles", action="store_true",
                        help="Also package a launcher NRO that runs the archive twice")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--romfs-tool", default=r"D:\devkitPro\tools\bin\build_romfs.exe")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    output_dir = args.output_dir.resolve()
    romfs = output_dir / "romfs"
    romfs.mkdir(parents=True, exist_ok=True)
    script = (repo / "tests/fixtures/sound_eos/startup.tjs").read_text(encoding="utf-8")
    if args.game_sample is not None:
        shutil.copy2(args.game_sample, romfs / "game-exit.ogg")
        script = script.replace("// OPTIONAL_GAME_SAMPLE",
            'eosCases.add(%[file:"romfs:/game-exit.ogg", name:"local game exit sample", stall:0]);')
    (romfs / "startup.tjs").write_text(script, encoding="utf-8")
    shutil.copy2(repo / "krkrsdl2/data/notosanssc.ttf", romfs / "notosanssc.ttf")
    for filename, rate, codec in (("short-opus.ogg", "48000", "libopus"),
                                  ("short-vorbis.ogg", "44100", "libvorbis"),
                                  ("short-pcm.wav", "48000", "pcm_s16le")):
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate={rate}:duration=0.3",
            "-ac", "1", "-c:a", codec, str(romfs / filename)], check=True)
    image = output_dir / "sound-eos.romfs"
    subprocess.run([args.romfs_tool, str(romfs), str(image)], check=True)
    target = output_dir / "sound-eos.nro"
    replace_romfs(args.nro.resolve(), image, target)
    print(target)
    # Use this archive as a launcher game to exercise a fresh engine after
    # another session. Completion closes the main window, allowing the same
    # fixture to be launched twice without modifying any real game.
    game = output_dir / "Game/000_SOUND_EOS_CONTRACT"
    game.mkdir(parents=True, exist_ok=True)
    game_script = script.replace("romfs:/", "")
    game_script = game_script.replace("var eosTimer;", "var eosTimer; var eosCloseTimer;")
    game_script = game_script.replace(
        'Debug.message("[sound-eos] COMPLETE " + eosChecks + " checks");',
        'Debug.message("[sound-eos] COMPLETE " + eosChecks + " checks");\n'
        '        eosCloseTimer = new Timer(function() {\n'
        '            eosCloseTimer.enabled = false; eosWindow.close();\n'
        '        }, "");\n'
        '        eosCloseTimer.interval = 250; eosCloseTimer.enabled = true;')
    files = [("startup.tjs", game_script.encode("utf-8"))]
    media_names = ["short-opus.ogg", "short-vorbis.ogg", "short-pcm.wav"]
    if args.game_sample is not None:
        media_names.append("game-exit.ogg")
    files.extend((name, (romfs / name).read_bytes()) for name in media_names)
    archive = game / "sound-eos.xp3"
    archive.write_bytes(build_plain_archive(files))
    print(archive)
    if args.launcher_cycles:
        marker = "sdmc:/switch/KRKR-ns/sound-eos-fixture-progress.ksd"
        prefix = ('var eosProgressFile = "' + marker + '";\n'
                  'var eosRound = 0;\n'
                  'if (Storages.isExistentStorage(eosProgressFile))\n'
                  '    eosRound = Dictionary.loadStruct(eosProgressFile).round;\n'
                  'Debug.message("[sound-eos] SESSION " + (eosRound + 1));\n')
        two_session_script = script.replace("romfs:/", "")
        two_session_script = prefix + two_session_script
        two_session_script = two_session_script.replace(
            'Debug.message("[sound-eos] COMPLETE " + eosChecks + " checks");',
            'Debug.message("[sound-eos] COMPLETE " + eosChecks + " checks");\n'
            '        var progress = %[round:eosRound + 1, checks:eosChecks];\n'
            '        (Dictionary.saveStruct incontextof progress)(eosProgressFile, "b");\n'
            '        if (eosRound == 0) System.exit(0);\n'
            '        else Debug.message("[sound-eos] TWO SESSIONS COMPLETE");')
        cycle_archive = game / "sound-eos.xp3"
        cycle_files = [("startup.tjs", two_session_script.encode("utf-8"))] + files[1:]
        cycle_archive.write_bytes(build_plain_archive(cycle_files))
        launcher_romfs = output_dir / "launcher-romfs"
        shutil.copytree(repo / "krkrsdl2/data", launcher_romfs, dirs_exist_ok=True)
        shutil.copytree(repo / "compat-patches/system", launcher_romfs / "compat/system",
                        dirs_exist_ok=True)
        launcher = (repo / "krkrsdl2/data/startup.tjs").read_text(encoding="utf-8")
        autocycle = ('if (Storages.isExistentStorage("sdmc:/switch/KRKR-ns/autocycle.txt") '
                     '&& games.count > 0) {\n'
                     '    selectFolder(Storages.getAutocycleRound() % games.count);\n'
                     '    requestLaunch();\n}\n')
        automatic = ('var eosRound = 0;\n'
            'if (Storages.isExistentStorage("' + marker + '"))\n'
            '    eosRound = Dictionary.loadStruct("' + marker + '").round;\n'
            'if (eosRound < 2) {\n'
            '    for (var eosIndex = 0; eosIndex < games.count; eosIndex++) {\n'
            '        if (games[eosIndex].folder == "000_SOUND_EOS_CONTRACT") {\n'
            '            selectFolder(eosIndex); requestLaunch(); break;\n'
            '        }\n'
            '    }\n'
            '}\n')
        if autocycle not in launcher:
            raise ValueError("launcher autocycle tail changed; update fixture packaging")
        launcher = launcher.replace(autocycle, automatic)
        (launcher_romfs / "startup.tjs").write_text(launcher, encoding="utf-8")
        launcher_image = output_dir / "sound-eos-launcher.romfs"
        subprocess.run([args.romfs_tool, str(launcher_romfs), str(launcher_image)], check=True)
        launcher_target = output_dir / "sound-eos-launcher.nro"
        replace_romfs(args.nro.resolve(), launcher_image, launcher_target)
        print(launcher_target)
        print("Before each two-session run, remove only this progress marker: " + marker)


if __name__ == "__main__":
    main()
