#include "RasterCopy.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {
int checks = 0, failures = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        if (failures < 12) std::cerr << message << '\n';
    }
}

struct Image {
    int width, height, stride;
    std::ptrdiff_t pitch;
    std::vector<std::uint8_t> bytes;
    Image(int w, int h, bool negative, std::uint32_t seed)
        : width(w), height(h), stride(w * 4 + 12),
          pitch(negative ? -stride : stride), bytes(32 + stride * h, 0xa7)
    {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::uint32_t pixel = seed ^ (0x79b135du * (y * w + x + 1));
                std::memcpy(Row(y) + x * 4, &pixel, 4);
            }
    }
    std::uint8_t* Data() { return bytes.data() + 16 + (pitch < 0 ? (height - 1) * stride : 0); }
    const std::uint8_t* Data() const { return bytes.data() + 16 + (pitch < 0 ? (height - 1) * stride : 0); }
    std::uint8_t* Row(int y) { return Data() + y * pitch; }
    const std::uint8_t* Row(int y) const { return Data() + y * pitch; }
};

// Independently copy each covered pixel from an immutable source snapshot.
// Uncovered edges, padding and guards retain their original destination bytes.
void Reference(Image& destination, const Image& source, int amplitude,
               int lines, int cycle, std::int64_t time)
{
    const double omega = 2 * std::acos(-1.0) / lines;
    double phase = -omega * time / cycle * (source.height / 2);
    for (int y = 0; y < source.height; ++y, phase += omega) {
        const double raw = std::sin(phase) * amplitude;
        if (raw >= source.width || raw <= -static_cast<double>(source.width)) continue;
        const int offset = static_cast<int>(raw);
        for (int x = 0; x < source.width; ++x) {
            const int inputX = x - offset;
            if (inputX >= 0 && inputX < source.width)
                std::memcpy(destination.Row(y) + x * 4, source.Row(y) + inputX * 4, 4);
        }
    }
}

void Compare(const Image& actual, const Image& expected, const char* message)
{
    for (std::size_t i = 0; i < actual.bytes.size(); ++i)
        Check(actual.bytes[i] == expected.bytes[i], message);
}

void GoldenPhaseAndTruncation()
{
    Image source(7, 3, false, 0x12345678), destination(7, 3, false, 0xabcdef01);
    Image expected = destination;
    // Odd height is integer-divided: phase=-pi/2, producing shifts -2,0,+2.
    const int offsets[] = {-2, 0, 2};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 7; ++x) {
            const int inputX = x - offsets[y];
            if (inputX >= 0 && inputX < 7)
                std::memcpy(expected.Row(y) + x * 4, source.Row(y) + inputX * 4, 4);
        }
    Check(krkrns::CopyRaster(destination.Data(), destination.pitch, source.Data(), source.pitch,
                            7, 3, 2, 4, 100, 100), "odd-height raster accepted");
    Compare(destination, expected, "integer height/2 golden phase");

    Image truncated(7, 2, false, 0xabcdef01);
    const Image original = truncated;
    Check(krkrns::CopyRaster(truncated.Data(), truncated.pitch, source.Data(), source.pitch,
                            7, 2, 3, 8, 100, 0), "truncation raster accepted");
    // sin(pi/4)*3=2.121... must truncate to +2, preserving the left two pixels.
    Check(std::memcmp(truncated.Row(1), original.Row(1), 8) == 0, "uncovered left edge retained");
    Check(std::memcmp(truncated.Row(1) + 8, source.Row(1), 20) == 0, "positive fractional displacement truncates");
}

void InvalidInputs()
{
    Image source(5, 4, false, 0x31415926), destination(5, 4, true, 0x27182818);
    const Image before = destination;
    Check(!krkrns::CopyRaster(destination.Data(), destination.pitch, source.Data(), source.pitch,
                             5, 4, 2, 0, 100, 0), "zero lines rejected");
    Check(!krkrns::CopyRaster(destination.Data(), destination.pitch, source.Data(), source.pitch,
                             5, 4, 2, 8, 0, 0), "zero cycle rejected");
    Check(!krkrns::CopyRaster(destination.Data(), 4, source.Data(), source.pitch,
                             5, 4, 2, 8, 100, 0), "short destination pitch rejected");
    Check(!krkrns::CopyRaster(destination.Data(), destination.pitch, source.Data(), 0,
                             5, 4, 2, 8, 100, 0), "zero source pitch rejected");
    Check(!krkrns::CopyRaster(nullptr, destination.pitch, source.Data(), source.pitch,
                             5, 4, 2, 8, 100, 0), "null destination rejected");
    Compare(destination, before, "invalid arguments leave bytes unchanged");
}
} // namespace

int main()
{
    GoldenPhaseAndTruncation();
    InvalidInputs();
    const int amplitudes[] = {0, 1, -3, 8, 99, std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max()};
    const std::int64_t times[] = {0, 1, 117, -241, 0x100000001LL,
                                  std::numeric_limits<std::int64_t>::max()};
    for (int width : {1, 7, 17})
        for (int height : {3, 4})
            for (bool sourceNegative : {false, true})
                for (bool destinationNegative : {false, true})
                    for (int amplitude : amplitudes)
                        for (int lines : {4, 9, -7})
                            for (int cycle : {113, -101})
                                for (std::int64_t time : times) {
                                    Image source(width, height, sourceNegative, 0x16273849);
                                    const Image sourceBefore = source;
                                    Image destination(width, height, destinationNegative, 0x93827160);
                                    Image expected = destination;
                                    Reference(expected, sourceBefore, amplitude, lines, cycle, time);
                                    Check(krkrns::CopyRaster(destination.Data(), destination.pitch,
                                                            source.Data(), source.pitch, width, height,
                                                            amplitude, lines, cycle, time), "raster accepted");
                                    Compare(destination, expected, "raster pixel/padding/guard mismatch");
                                    Compare(source, sourceBefore, "source changed");
                                }
    for (bool negative : {false, true})
        for (int amplitude : {-5, 0, 5, 80}) {
            Image self(13, 7, negative, 0x87654321);
            const Image sourceSnapshot = self;
            Image expected = self;
            Reference(expected, sourceSnapshot, amplitude, 9, 127, 81);
            Check(krkrns::CopyRaster(self.Data(), self.pitch, self.Data(), self.pitch,
                                    13, 7, amplitude, 9, 127, 81), "in-place raster accepted");
            Compare(self, expected, "in-place raster snapshot mismatch");
        }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
