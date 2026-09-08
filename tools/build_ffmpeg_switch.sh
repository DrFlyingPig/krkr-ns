#!/usr/bin/env bash
# Build the KRKR-ns private FFmpeg 7.1 subset for Nintendo Switch.
# Supports the source-faithful movie cases currently observed:
#   ASF/WMV/VC-1/WMA and MOV/MP4/H.264/AAC.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src_parent="$root/out/ffmpeg_src"
src="$src_parent/ffmpeg-7.1"
prefix="$root/out/ffmpeg_switch"
archive_default="$root/../WA2-ns/out/ffmpeg-7.1.tar.xz"
archive="${FFMPEG_ARCHIVE:-$archive_default}"
patch_file="$root/tools/ffmpeg-horizon.patch"
host_cc="$root/tools/ffmpeg_host_cl_wrapper.sh"
make_bin_default="$root/../WA2-ns/out/msys_make/usr/bin/make.exe"
make_bin="${FFMPEG_MAKE:-$make_bin_default}"
expected_sha256=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6

if [[ -f "$archive" ]]; then
    actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
else
    actual_sha256=""
fi
if [[ "$actual_sha256" != "$expected_sha256" && "$archive" == "$archive_default" ]]; then
    echo "Ignoring sibling archive with non-release hash: $actual_sha256"
    archive="$root/out/ffmpeg-7.1.tar.xz"
    if [[ -f "$archive" ]]; then
        actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
    else
        actual_sha256=""
    fi
fi
if [[ ! -f "$archive" || "$actual_sha256" != "$expected_sha256" ]]; then
    curl --fail --location --output "$archive" \
        https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
    actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
fi
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "FFmpeg archive hash mismatch: $actual_sha256" >&2
    exit 1
fi
if [[ ! -x "$make_bin" ]]; then
    echo "GNU make not found: $make_bin" >&2
    exit 1
fi

# These exact generated directories are disposable. Refuse broad or escaped
# paths before removing an earlier dependency build/install.
case "$src" in "$root"/out/ffmpeg_src/ffmpeg-7.1) ;; *) exit 1 ;; esac
case "$prefix" in "$root"/out/ffmpeg_switch) ;; *) exit 1 ;; esac
rm -rf -- "$src" "$prefix"
mkdir -p "$src_parent" "$prefix"
tar -xf "$archive" -C "$src_parent"
patch -d "$src" -p1 < "$patch_file"

dkp="${DEVKITPRO:-/d/devkitPro}"
if [[ "$dkp" == *:\\* ]]; then dkp="$(cygpath -u "$dkp")"; fi
if [[ ! -d "$dkp/devkitA64" && -d /d/devkitPro/devkitA64 ]]; then
    dkp=/d/devkitPro
fi
dkpa64="${DEVKITA64:-$dkp/devkitA64}"
if [[ "$dkpa64" == *:\\* ]]; then dkpa64="$(cygpath -u "$dkpa64")"; fi
if [[ ! -d "$dkpa64/bin" ]]; then dkpa64="$dkp/devkitA64"; fi
export DEVKITPRO="$dkp"
export DEVKITA64="$dkpa64"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$(dirname "$make_bin"):$PATH"

cd "$src"
./configure \
  --prefix="$prefix" \
  --cross-prefix=aarch64-none-elf- \
  --enable-cross-compile \
  --host-cc="$host_cc" \
  --arch=aarch64 \
  --cpu=cortex-a57 \
  --target-os=horizon \
  --enable-pic \
  --extra-cflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -ffunction-sections -fdata-sections' \
  --extra-ldflags="-fPIE -L$DEVKITPRO/libnx/lib -Wl,--gc-sections" \
  --disable-runtime-cpudetect \
  --disable-programs \
  --disable-debug \
  --disable-doc \
  --disable-autodetect \
  --disable-network \
  --disable-everything \
  --disable-gpl \
  --disable-version3 \
  --disable-nonfree \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-swresample \
  --enable-demuxer=asf,mov \
  --enable-decoder=wmv3,vc1,wmav1,wmav2,wmapro,wmavoice,h264,aac \
  --enable-parser=vc1,h264,aac \
  --enable-protocol=file \
  --enable-pthreads \
  --enable-asm \
  --enable-neon \
  --enable-small

"$make_bin" -j4
"$make_bin" install

echo "FFmpeg Switch subset installed at $prefix"
ls -la "$prefix/lib"
