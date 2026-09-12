#!/bin/bash
# KRKR-ns: build krkrsdl2 for Nintendo Switch and produce krkrsdl2.nro
# Usage: build_nro.sh [--no-patch] [--no-emu-copy]
# Requires: DEVKITPRO, DEVKITA64 set; cmake+ninja+pkg-config in tools/ (D:\KRKR-ns-tools)
set -e
cd -P "$(dirname "$0")"

TOOLS=${KRKRNS_TOOLS:-/d/KRKR-ns-tools}
CMAKE="$TOOLS/cmake-3.31.6-windows-x86_64/bin/cmake.exe"
NINJA="$TOOLS/ninja.exe"
PKGCONF="$TOOLS/bin/pkg-config.exe"
EMU_GAMES="$TOOLS/emulator/publish/portable/games"
EMU_PATCH="$TOOLS/emulator/publish/portable/sdcard/switch/KRKR-ns/patch/system"

# NOTE: env may carry a stale DEVKITPRO (/opt/devkitpro) — force the real one
export DEVKITPRO="${KRKRNS_DEVKITPRO:-D:/devkitPro}"
export DEVKITA64="${KRKRNS_DEVKITA64:-$DEVKITPRO/devkitA64}"
export PKG_CONFIG_PATH="D:/devkitPro/portlibs/switch/lib/pkgconfig"
export PATH="$TOOLS/bin:$PATH"

DO_PATCH=1
DO_EMU_COPY=1
for a in "$@"; do
  case "$a" in
    --no-patch) DO_PATCH=0 ;;
    --no-emu-copy) DO_EMU_COPY=0 ;;
  esac
done

echo "== configure =="
"$CMAKE" -S krkrsdl2 -B build-switch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLS/Switch.toolchain.cmake" \
  -DNINTENDO_SWITCH=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOPTION_BUILD_MACOS_BUNDLE=OFF \
  -DCMAKE_MAKE_PROGRAM="$NINJA" >/dev/null

echo "== build =="
"$CMAKE" --build build-switch

ELF=build-switch/krkrsdl2
NRO=build-switch/krkrsdl2.nro

if [ "$DO_PATCH" = 1 ]; then
  echo "== patch GCS unwind (Nextendo emulator chkfeat bug workaround) =="
  python - <<'PYEOF'
import struct
data = bytearray(open('build-switch/krkrsdl2','rb').read())
pos = 0
sites = []
while True:
    i = data.find(bytes.fromhex('1F2503D5'), pos)
    if i < 0: break
    sites.append(i); pos = i+1
n = 0
for fo in sites:
    va = fo - 0x10000
    for k in range(4, 24, 4):
        w = struct.unpack('<I', data[fo+k:fo+k+4])[0]
        if (w >> 24) != 0xB5: continue
        imm19 = (w >> 5) & 0x7FFFF
        if imm19 & 0x40000: imm19 -= 0x80000
        cbnz_va = va + k
        target = cbnz_va + (imm19 << 2)
        imm26 = ((target - cbnz_va) >> 2) & 0x3FFFFFF
        data[fo+k:fo+k+4] = struct.pack('<I', 0x14000000 | imm26)
        print('  patched cbnz@%x -> b %x' % (cbnz_va, target))
        n += 1
        break
open('build-switch/krkrsdl2-patched.elf','wb').write(data)
print('patched %d chkfeat/cbnz sites' % n)
PYEOF
  ELF=build-switch/krkrsdl2-patched.elf
fi

echo "== package NRO =="
# RomFS must contain the game data at ROOT (the engine expects
# startup.tjs at the archive root); flatten the data/ copy cmake made.
ROMFS_DATA_ABS="$(realpath -m build-switch/romfs/data)"
if [[ "$ROMFS_DATA_ABS" != "$PWD/build-switch/romfs/data" ]]; then
  echo "Refusing to clean RomFS data outside the build directory" >&2
  exit 1
fi
rm -rf -- "$ROMFS_DATA_ABS"
cp -r krkrsdl2/data/. build-switch/romfs/
mkdir -p build-switch/romfs/compat/system
# Remove the retired pre-E-mote shim from incremental build trees.  Leaving it
# here shadows the game's real system/motion.tjs and disables every character
# player even though the native E-mote runtime is linked into the NRO.
rm -f build-switch/romfs/compat/system/motion.tjs
cp -r compat-patches/system/. build-switch/romfs/compat/system/
ls build-switch/romfs/
"D:/devkitPro/tools/bin/elf2nro.exe" "$ELF" "$NRO" \
  --icon=krkrsdl2/src/resources/nswitch/icon.jpg \
  --nacp=build-switch/krkrsdl2.nacp \
  --romfsdir=build-switch/romfs

if [ "$DO_EMU_COPY" = 1 ]; then
  echo "== copy to emulator games dir (single loader entry, fixed name) =="
  mkdir -p "$EMU_GAMES"
  cp "$NRO" "$EMU_GAMES/krkrsdl2.nro"
  echo "== deploy runtime compatibility patches =="
  mkdir -p "$EMU_PATCH"
  # The patch directory outranks the romfs compat folder in the auto-path order,
  # so a leftover file here SHADOWS the copy bundled in the NRO.  Stale copies
  # have already caused one silent misdiagnosis (an old k2compat_reinstall.tjs
  # was executed instead of the freshly built one).  Drop the scripts we deploy
  # before re-copying them, so the deployed set always matches this build.
  for f in k2compat.tjs k2compat_console.tjs win32dialog.tjs k2compat_reinstall.tjs motion.tjs; do
    rm -f "$EMU_PATCH/$f"
  done
  cp -r compat-patches/system/. "$EMU_PATCH/"
fi

echo "== done: $NRO =="
ls -la "$NRO"
