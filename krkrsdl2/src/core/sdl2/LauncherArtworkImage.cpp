/* SPDX-License-Identifier: MIT */
#include "tjsCommHead.h"
#include "LauncherArtworkStorage.h"

#ifdef __SWITCH__
#include "BinaryStream.h"
#include "LauncherArtworkRaster.h"
#include "LauncherArtworkTLG.h"
#include "CharacterSet.h"
#include "GraphicsLoaderIntf.h"
#include "LayerBitmapIntf.h"
#include "StorageIntf.h"
#include "XP3Archive.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <vector>

namespace krkrns_launcher_artwork {
namespace {

constexpr tjs_uint64 MaximumEncodedBytes = 32ULL * 1024 * 1024;
constexpr tjs_uint64 MaximumDecodedPixels = 16ULL * 1024 * 1024;
constexpr tjs_uint MaximumSide = 16384;

struct DecodeContext {
    std::unique_ptr<tTVPBaseBitmap> bitmap;
};

void ArtworkSize(void *opaque, tjs_uint width, tjs_uint height)
{
    if (!width || !height || width > MaximumSide || height > MaximumSide ||
        static_cast<tjs_uint64>(width) * height > MaximumDecodedPixels)
        throw eTJSError(TJS_W("Artwork image exceeds the 16 megapixel limit"));
    auto &context = *static_cast<DecodeContext *>(opaque);
    if (context.bitmap)
        throw eTJSError(TJS_W("Repeated artwork image dimensions"));
    context.bitmap.reset(new tTVPBaseBitmap(width, height, 32));
}

void *ArtworkScanLine(void *opaque, tjs_int y)
{
    if (y == -1) return nullptr;
    auto &context = *static_cast<DecodeContext *>(opaque);
    if (!context.bitmap || y < 0 ||
        static_cast<tjs_uint>(y) >= context.bitmap->GetHeight())
        throw eTJSError(TJS_W("Invalid artwork image row"));
    return context.bitmap->GetScanLineForWrite(static_cast<tjs_uint>(y));
}

tTVPGraphicLoadingHandler DecoderFor(const tjs_uint8 *header, tjs_uint count)
{
    if (count >= 8 && !std::memcmp(header, "\x89PNG\r\n\x1a\n", 8)) return TVPLoadPNG;
    if (count >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff) return TVPLoadJPEG;
    if (count >= 2 && !std::memcmp(header, "BM", 2)) return TVPLoadBMP;
    if (count >= 3 && !std::memcmp(header, "TLG", 3)) return TVPLoadTLG;
#ifdef TVP_IMAGE_ENABLE_WEBP
    if (count >= 12 && !std::memcmp(header, "RIFF", 4) &&
        !std::memcmp(header + 8, "WEBP", 4)) return TVPLoadWEBP;
#endif
    // No scripts, plugins or game-specific decryption are installed here.
    throw eTJSError(TJS_W("Artwork format is unreadable or needs a game decoder"));
}

// The shared JPEG handler does not check header/decode failures and allocates
// before its exception cleanup. Keep the launcher's bounded, fallible decode
// independent of that game rendering path.
void DecodeArtworkJPEG(tTJSBinaryStream &input, DecodeContext &context)
{
    std::vector<unsigned char> encoded(static_cast<std::size_t>(input.GetSize()));
    if (encoded.empty() || input.Read(encoded.data(), encoded.size()) != encoded.size())
        throw eTJSError(TJS_W("Cannot read artwork JPEG"));
    struct JpegDeleter {
        void operator()(void *handle) const { if (handle) tjDestroy(handle); }
    };
    std::unique_ptr<void, JpegDeleter> decoder(tjInitDecompress());
    if (!decoder) throw eTJSError(TJS_W("Cannot initialise artwork JPEG decoder"));
    int width = 0, height = 0, subsampling = 0;
    if (tjDecompressHeader2(decoder.get(), encoded.data(), encoded.size(),
            &width, &height, &subsampling) != 0 || width <= 0 || height <= 0)
        throw eTJSError(TJS_W("Invalid artwork JPEG header"));
    ArtworkSize(&context, width, height);
    std::unique_ptr<unsigned char, decltype(&tjFree)> pixels(
        tjAlloc(static_cast<int>(static_cast<std::size_t>(width) * height * 4)), tjFree);
    if (!pixels) throw eTJSError(TJS_W("Cannot allocate artwork JPEG pixels"));
    if (tjDecompress2(decoder.get(), encoded.data(), encoded.size(), pixels.get(),
            width, width * 4, height, TJPF_BGRA, TJFLAG_FASTDCT) != 0)
        throw eTJSError(TJS_W("Invalid artwork JPEG pixels"));
    for (int y = 0; y < height; ++y)
        std::memcpy(context.bitmap->GetScanLineForWrite(y),
            pixels.get() + static_cast<std::size_t>(y) * width * 4,
            static_cast<std::size_t>(width) * 4);
}

void RoundCorners(tTVPBaseBitmap &bitmap, int radius)
{
    const int width = bitmap.GetWidth(), height = bitmap.GetHeight();
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            const double dx = radius - x - 0.5, dy = radius - y - 0.5;
            const double coverage = std::max(0.0, std::min(1.0,
                radius + 0.5 - std::sqrt(dx * dx + dy * dy)));
            const int coordinates[4][2] = {
                {x, y}, {width - x - 1, y},
                {x, height - y - 1}, {width - x - 1, height - y - 1}
            };
            for (const auto &point : coordinates) {
                const auto pixel = bitmap.GetPoint(point[0], point[1]);
                const auto alpha = static_cast<tjs_uint32>(((pixel >> 24) & 255) * coverage + 0.5);
                bitmap.SetPoint(point[0], point[1], (pixel & 0x00ffffff) | (alpha << 24));
            }
        }
    }
}

void HashBytes(std::uint64_t &hash, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
}

std::string Utf8(const ttstr &value)
{
    std::string result;
    if (!TVPUtf16ToUtf8(result, value.AsStdString()))
        throw eTJSError(TJS_W("Invalid artwork path encoding"));
    return result;
}

} // namespace

ttstr MakeGameArtworkThumbnail(const ttstr &folder, const ttstr &source,
                              const ttstr &role)
{
    if (!IsLauncherArtworkMode())
        throw eTJSError(TJS_W("Artwork can only be edited from the launcher"));
    if (TVPHasXP3ArchiveFilters())
        throw eTJSError(TJS_W("Artwork decoding cannot use active game archive filters"));
    if (!ValidateArtworkSource(folder, source))
        throw eTJSError(TJS_W("Artwork source must belong to the selected game or Artwork directory"));

    int width = 0, height = 0, radius = 0;
    if (role == TJS_W("grid")) { width = 240; height = 135; radius = 12; }
    else if (role == TJS_W("preview")) { width = 860; height = 358; radius = 29; }
    else if (role == TJS_W("avatar")) { width = 96; height = 96; radius = 14; }
    else throw eTJSError(TJS_W("Unknown artwork role"));

    // Direct stream decoders avoid auto-path registration, sidecar mask
    // probes, and putting every full-resolution CG in the graphics cache.
    const auto delimiter = source.AsStdString().find(TJS_W('>'));
    if (delimiter != tjs_string::npos) {
        const auto path = source.AsStdString();
        ValidateArtworkArchiveIndex(ttstr(path.substr(0, delimiter)),
            ttstr(path.substr(delimiter + 1)));
    }
    std::unique_ptr<tTJSBinaryStream> input(TVPCreateStream(source, TJS_BS_READ));
    if (input->GetSize() > MaximumEncodedBytes)
        throw eTJSError(TJS_W("Artwork file exceeds the 32 MiB limit"));
    tjs_uint8 header[16] = {};
    const auto count = input->Read(header, sizeof header);
    input->SetPosition(0);
    DecodeContext context;
    const auto decoder = DecoderFor(header, count);
    if (decoder == TVPLoadBMP) PreflightArtworkBMP(*input);
    if (decoder == TVPLoadTLG) PreflightArtworkTLG(*input);
    if (decoder == TVPLoadJPEG) DecodeArtworkJPEG(*input, context);
    else decoder(nullptr, &context, ArtworkSize, ArtworkScanLine,
        nullptr, input.get(), -1, glmNormal);
    if (!context.bitmap)
        throw eTJSError(TJS_W("Artwork image has no pixels"));
    input.reset();

    // Preserve aspect ratio and crop about the centre, rather than distort
    // the original image to fit either a wide banner or a square avatar.
    const int sourceWidth = context.bitmap->GetWidth();
    const int sourceHeight = context.bitmap->GetHeight();
    const double scale = std::max(static_cast<double>(width) / sourceWidth,
                                  static_cast<double>(height) / sourceHeight);
    const int cropWidth = std::max(1, std::min(sourceWidth, static_cast<int>(std::round(width / scale))));
    const int cropHeight = std::max(1, std::min(sourceHeight, static_cast<int>(std::round(height / scale))));
    const int left = (sourceWidth - cropWidth) / 2;
    const int top = (sourceHeight - cropHeight) / 2;
    tTVPBaseBitmap thumbnail(width, height, 32);
    const tTVPRect destination(0, 0, width, height);
    thumbnail.Fill(destination, 0);
    thumbnail.StretchBlt(destination, destination, context.bitmap.get(),
        tTVPRect(left, top, left + cropWidth, top + cropHeight),
        bmCopy, 255, false, stLinear);
    context.bitmap.reset();
    RoundCorners(thumbnail, radius);

    // The result's identity includes its pixels. Replacing an original CG
    // therefore generates a new cache file instead of serving a stale image.
    std::uint64_t hash = 14695981039346656037ULL;
    const auto source8 = Utf8(source), role8 = Utf8(role);
    HashBytes(hash, source8.data(), source8.size());
    HashBytes(hash, role8.data(), role8.size());
    for (int y = 0; y < height; ++y)
        HashBytes(hash, thumbnail.GetScanLine(y), static_cast<std::size_t>(width) * 4);
    char basename[80];
    std::snprintf(basename, sizeof basename, "%s-%016llx.png", role8.c_str(),
        static_cast<unsigned long long>(hash));
    const ttstr cachePath = ArtworkCacheDirectory() + ttstr(basename);
    const auto nativePath = Utf8(cachePath);
    struct stat info;
    if (stat(nativePath.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 64)
        return cachePath;

    const ttstr temporary = cachePath + TJS_W(".tmp");
    const auto nativeTemporary = Utf8(temporary);
    try {
        {
            std::unique_ptr<tTJSBinaryStream> output(TVPCreateStream(temporary, TJS_BS_WRITE));
            TVPSaveAsPNG(nullptr, output.get(), &thumbnail, TJS_W("png"), nullptr);
        }
        if (std::rename(nativeTemporary.c_str(), nativePath.c_str()) != 0)
            throw eTJSError(TJS_W("Cannot commit artwork thumbnail"));
    } catch (...) {
        std::remove(nativeTemporary.c_str());
        throw;
    }
    return cachePath;
}

} // namespace krkrns_launcher_artwork
#endif
