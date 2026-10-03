"""Check the production PNG private-chunk callback's optional metadata contract.

Compile its tags, user-data structure and callback unchanged. A nullable callable
reports an absent sink as an exception instead of crashing the host test process;
the artwork_png NRO fixture exercises the real decoder and function pointer.
"""
import argparse
from pathlib import Path
import shutil
import subprocess


HARNESS = r'''
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
using tjs_uint32 = std::uint32_t;
using tjs_uint8 = std::uint8_t;
using tjs_int = int;
#define TJS_W(value) value
struct ttstr {
    std::string value;
    ttstr(const char *s) : value(s) {}
    ttstr(int n) : value(std::to_string(n)) {}
};
using tTVPMetaInfoPushCallback = std::function<void(void *, const ttstr &, const ttstr &)>;
struct png_struct_def { void *user = nullptr; };
using png_structp = png_struct_def *;
struct png_unknown_chunk_t {
    unsigned char name[5] = {};
    unsigned char *data = nullptr;
    std::size_t size = 0;
};
using png_unknown_chunkp = png_unknown_chunk_t *;
static void *png_get_user_chunk_ptr(png_structp png) { return png->user; }
constexpr int PNG_OFFSET_PIXEL = 0, PNG_OFFSET_MICROMETER = 1;
'''


CASES = r'''
static void Require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static int Read(const char *name, std::size_t size, void *user, unsigned char unit = 0) {
    // The real Senren artwork PNG's vpAg is 785 x 132 pixels.
    unsigned char data[] = {0, 0, 3, 17, 0, 0, 0, 132, unit};
    png_unknown_chunk_t chunk;
    std::memcpy(chunk.name, name, 4); chunk.data = data; chunk.size = size;
    png_struct_def png; png.user = user;
    return PNG_read_chunk_callback(&png, &chunk);
}
int main() {
    try {
        PNG_read_chunk_callback_user_struct pixelsOnly{nullptr, {}};
        Require(Read("vpAg", 9, &pixelsOnly) == 1,
                "pixel-only PNG load must consume vpAg without a metadata sink");
        Require(Read("vpAg", 9, nullptr) == 1,
                "known chunk must tolerate missing metadata user data");
        for (unsigned char unit : {0, 1, 7}) {
            std::map<std::string, std::string> metadata;
            int calls = 0;
            PNG_read_chunk_callback_user_struct withMetadata{
                &metadata,
                [&](void *owner, const ttstr &key, const ttstr &value) {
                    Require(owner == &metadata, "metadata callback owner changed");
                    ++calls; metadata[key.value] = value.value;
                }
            };
            Require(Read("vpAg", 9, &withMetadata, unit) == 1, "vpAg recognition changed");
            Require(calls == 3 && metadata.size() == 3 && metadata["vpag_w"] == "785" &&
                    metadata["vpag_h"] == "132" && metadata["vpag_unit"] ==
                    (unit == 0 ? "pixel" : unit == 1 ? "micrometer" : "unknown"),
                    "game metadata dimensions or units changed");
            metadata.clear(); calls = 0;
            Require(Read("VPAG", 9, &withMetadata, unit) == 1 && calls == 3,
                    "existing case-insensitive vpAg handling changed");
            metadata.clear(); calls = 0;
            Require(Read("vpAg", 8, &withMetadata) == 0 && calls == 0,
                    "short private chunk must remain unhandled");
            Require(Read("zzZz", 9, &withMetadata) == 0 && calls == 0,
                    "unrecognized chunk must remain unhandled");
        }
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
    std::cout << "PASS: optional PNG metadata sink, existing dimensions/units, "
                 "case handling, and short/unknown chunks\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--git-head', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cmake', default=shutil.which('cmake'))
    args = parser.parse_args()
    if not args.cmake:
        parser.error('CMake is required; pass --cmake if it is not on PATH.')
    root = Path(__file__).resolve().parent.parent
    source = args.source or root / 'krkrsdl2/external/krkrz/visual/LoadPNG.cpp'
    text = (subprocess.check_output(['git', '-C', str(root), 'show',
                                    'HEAD:krkrsdl2/external/krkrz/visual/LoadPNG.cpp']).decode('utf-8')
            if args.git_head else source.read_text(encoding='utf-8'))
    text = text.replace('\r\n', '\n')
    tags = text[text.index('static ttstr PNG_tag_offs_x('):text.index('static png_voidp PNG_malloc(')]
    start = text.index('static int PNG_read_chunk_callback(')
    callback = text[start:text.index('\n}\n', start) + 3]
    out = (args.output_dir or root / 'build-png-metadata').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'png_metadata_test.cpp').write_text(HARNESS + tags + callback + CASES, encoding='utf-8')
    (out / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(png_metadata_test LANGUAGES CXX)\n'
        'set(CMAKE_CXX_STANDARD 17)\n'
        'add_executable(png_metadata_test png_metadata_test.cpp)\n'
        'if(MSVC)\n target_compile_options(png_metadata_test PRIVATE /EHsc /utf-8)\nendif()\n',
        encoding='utf-8')
    subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build')], check=True, timeout=90)
    subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'],
                   check=True, timeout=180)
    binary = next(p for p in (out / 'build/Release/png_metadata_test.exe',
                             out / 'build/png_metadata_test') if p.is_file())
    return subprocess.run([str(binary)], timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
