// Huffman specifications, integer IDCT and AMV block order adapted from
// krkrsdl3/plugins/AlphaMovie.cpp, f3b76d2; see LICENSE.krkrsdl3.
// Bounded bit reader, container/player and bitmap conversion are KRKR-ns code.
#include "AmvMovie.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace krkr { namespace amv { namespace {
using JLONG = int64_t;
using JCOEF = int16_t;
using JCOEFPTR = JCOEF*;
using JSAMPLE = uint8_t;
using JSAMPROW = JSAMPLE*;
#define DCTSIZE 8
#define DCTSIZE2 64
static const int jpeg_natural_order[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,
    41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,
    15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
[[noreturn]] void Fail(const char* why) { throw std::runtime_error(why); }
struct BufferManager {
    const uint8_t* data;
    size_t length, position = 0;
    uint64_t buffer = 0;
    unsigned available = 0;
    int32_t decodeUVStatus = 0, decodeYStatus = 0;
    BufferManager(const uint8_t* bytes, size_t count) : data(bytes), length(count) {}
    void Fill(unsigned bits) {
        while (available < bits && position < length) {
            buffer = (buffer << 8) | data[position++]; available += 8;
        }
    }
    uint32_t Peek(unsigned bits) {
        Fill(bits);
        if (available < bits) Fail("truncated AMV entropy stream");
        return static_cast<uint32_t>((buffer >> (available - bits)) & ((uint64_t(1) << bits)-1));
    }
    uint32_t Read(unsigned bits) {
        if (!bits) return 0;
        const auto result = Peek(bits); available -= bits; return result;
    }
};
struct _CompactHuffmanSpec
{
    uint8_t bits[16];
    uint8_t values[256];
    size_t value_count;
};

static const _CompactHuffmanSpec kJpegLumaAcSpec = {
    {0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03,
     0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
     0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
     0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
     0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
     0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
     0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
     0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
     0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
     0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
     0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
     0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
     0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
     0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
     0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
     0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
     0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
     0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
     0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
     0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
     0xF9, 0xFA},
    162};

static const _CompactHuffmanSpec kJpegLumaDcSpec = {
    {0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
     0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B},
    12};

static const _CompactHuffmanSpec kJpegChromaAcSpec = {
    {0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04,
     0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
     0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
     0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
     0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
     0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
     0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
     0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
     0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
     0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
     0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
     0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
     0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
     0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
     0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
     0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
     0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
     0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
     0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
     0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
     0xF9, 0xFA},
    162};

static const _CompactHuffmanSpec kJpegChromaDcSpec = {
    {0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
     0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B},
    11};


struct Huffman {
    std::array<int, 17> maximum{}, offset{};
    std::array<uint8_t, 256> symbols{};
    std::array<uint8_t, 512> fastSymbol{}, fastLength{};
    explicit Huffman(const _CompactHuffmanSpec& spec) {
        maximum.fill(-1);
        std::copy(spec.values, spec.values + spec.value_count, symbols.begin());
        int code = 0, index = 0;
        for (int bits = 1; bits <= 16; ++bits) {
            const int count = spec.bits[bits - 1];
            offset[bits] = index - code;
            if (count) maximum[bits] = code + count - 1;
            for (int n = 0; n < count; ++n, ++code, ++index) {
                if (bits <= 9) {
                    const int start = code << (9 - bits), end = (code + 1) << (9 - bits);
                    for (int p = start; p < end; ++p) {
                        fastSymbol[p] = spec.values[index]; fastLength[p] = static_cast<uint8_t>(bits);
                    }
                }
            }
            code <<= 1;
        }
    }
    uint8_t Decode(BufferManager& input) const {
        input.Fill(9);
        if (input.available >= 9) {
            const auto p = input.Peek(9);
            if (fastLength[p]) { input.Read(fastLength[p]); return fastSymbol[p]; }
        }
        int code = 0;
        for (int bits = 1; bits <= 16; ++bits) {
            code = (code << 1) | input.Read(1);
            if (maximum[bits] >= 0 && code <= maximum[bits]) {
                const int index = offset[bits] + code;
                if (index < 0 || index >= 256) Fail("invalid AMV Huffman symbol");
                return symbols[index];
            }
        }
        Fail("invalid AMV Huffman code");
    }
};
static const Huffman HuffmanTable[4] = {Huffman(kJpegChromaDcSpec), Huffman(kJpegChromaAcSpec),
                                       Huffman(kJpegLumaDcSpec), Huffman(kJpegLumaAcSpec)};
int SignedValue(BufferManager& input, unsigned bits) {
    if (!bits) return 0;
    const int value = input.Read(bits);
    return value < (1 << (bits - 1)) ? value - ((1 << bits) - 1) : value;
}
void decode_dc_run_length(const Huffman* table, BufferManager* input, int16_t* block, int32_t& previous) {
    const unsigned bits = table->Decode(*input);
    if (bits > 11) Fail("invalid AMV DC category");
    previous += SignedValue(*input, bits);
    if (previous < -32768 || previous > 32767) Fail("AMV DC coefficient overflow");
    block[0] = static_cast<int16_t>(previous);
}
int decode_ac_run_length(const Huffman* table, BufferManager* input, int16_t* block) {
    int index = 1;
    while (index < 64) {
        const auto symbol = table->Decode(*input);
        const unsigned bits = symbol & 15;
        if (!bits) {
            if (!symbol) return index;
            if (symbol != 0xf0 || index + 16 > 64) Fail("invalid AMV AC zero run");
            index += 16; continue;
        }
        if (bits > 10) Fail("invalid AMV AC category");
        index += symbol >> 4;
        if (index >= 64) Fail("AMV AC coefficient overflow");
        block[jpeg_natural_order[index++]] = static_cast<int16_t>(SignedValue(*input, bits));
    }
    return index;
}
#define LEFT_SHIFT(a, b) ((JLONG)(a) * (JLONG(1) << (b)))
#define RIGHT_SHIFT(x, shft) ((x) >> (shft))
#define DEQUANTIZE(coef, quantval) (((int)(coef)) * (quantval))
#define PASS1_BITS 1
#define CONST_BITS 13
#define MULTIPLY(var, const) ((var) * (const))
#define ONE ((JLONG)1)
#define FIX_0_298631336 ((JLONG)2446)  /* FIX(0.298631336) */
#define FIX_0_390180644 ((JLONG)3196)  /* FIX(0.390180644) */
#define FIX_0_541196100 ((JLONG)4433)  /* FIX(0.541196100) */
#define FIX_0_765366865 ((JLONG)6270)  /* FIX(0.765366865) */
#define FIX_0_899976223 ((JLONG)7373)  /* FIX(0.899976223) */
#define FIX_1_175875602 ((JLONG)9633)  /* FIX(1.175875602) */
#define FIX_1_501321110 ((JLONG)12299) /* FIX(1.501321110) */
#define FIX_1_847759065 ((JLONG)15137) /* FIX(1.847759065) */
#define FIX_1_961570560 ((JLONG)16069) /* FIX(1.961570560) */
#define FIX_2_053119869 ((JLONG)16819) /* FIX(2.053119869) */
#define FIX_2_562915447 ((JLONG)20995) /* FIX(2.562915447) */
#define FIX_3_072711026 ((JLONG)25172) /* FIX(3.072711026) */
#define DESCALE(x, n) RIGHT_SHIFT((x) + (ONE << ((n)-1)), n)
#define MAXJSAMPLE 255
#define CENTERJSAMPLE 128
#define RANGE_MASK (MAXJSAMPLE * 4 + 3)
static const std::array<uint8_t, 1408> rangeTable = [] {
    std::array<uint8_t, 1408> result{};
    auto* sample = result.data() + 256;
    for (int i = 0; i < 256; ++i) sample[i] = static_cast<uint8_t>(i);
    auto* table = sample + 128;
    std::fill(table + 128, table + 512, 255);
    std::copy(sample, sample + 128, table + 896);
    return result;
}();
static void jpeg_idct_data(int16_t* block, uint8_t* qtbl, uint8_t* retBlock, uint32_t retBlockPitch)
{
    JLONG tmp0, tmp1, tmp2, tmp3;
    JLONG tmp10, tmp11, tmp12, tmp13;
    JLONG z1, z2, z3, z4, z5;
    JCOEFPTR inptr;
    int* quantptr;
    JLONG* wsptr;
    JSAMPROW outptr;
    const JSAMPLE* range_limit = rangeTable.data() + 384;
    int ctr;
    JLONG workspace[DCTSIZE2]; /* buffers data between passes */

    /* Pass 1: process columns from input, store into work array. */
    /* Note results are scaled up by sqrt(8) compared to a true IDCT; */
    /* furthermore, we scale the results by 2**PASS1_BITS. */

    inptr = block;
    int _quantptrData[64];
    for (size_t i = 0; i < 64; i++)
        _quantptrData[i] = (int)qtbl[i];
    quantptr = _quantptrData;
    wsptr = workspace;
    for (ctr = DCTSIZE; ctr > 0; ctr--)
    {
        /* Due to quantization, we will usually find that many of the input
         * coefficients are zero, especially the AC terms.  We can exploit this
         * by short-circuiting the IDCT calculation for any column in which all
         * the AC terms are zero.  In that case each output is equal to the
         * DC coefficient (with scale factor as needed).
         * With typical images and quantization tables, half or more of the
         * column DCT calculations can be simplified this way.
         */

        if (inptr[DCTSIZE * 1] == 0 && inptr[DCTSIZE * 2] == 0 && inptr[DCTSIZE * 3] == 0 &&
            inptr[DCTSIZE * 4] == 0 && inptr[DCTSIZE * 5] == 0 && inptr[DCTSIZE * 6] == 0 &&
            inptr[DCTSIZE * 7] == 0)
        {
            /* AC terms all zero */
            int dcval =
                LEFT_SHIFT(DEQUANTIZE(inptr[DCTSIZE * 0], quantptr[DCTSIZE * 0]), PASS1_BITS);

            wsptr[DCTSIZE * 0] = dcval;
            wsptr[DCTSIZE * 1] = dcval;
            wsptr[DCTSIZE * 2] = dcval;
            wsptr[DCTSIZE * 3] = dcval;
            wsptr[DCTSIZE * 4] = dcval;
            wsptr[DCTSIZE * 5] = dcval;
            wsptr[DCTSIZE * 6] = dcval;
            wsptr[DCTSIZE * 7] = dcval;

            inptr++; /* advance pointers to next column */
            quantptr++;
            wsptr++;
            continue;
        }

        /* Even part: reverse the even part of the forward DCT. */
        /* The rotator is sqrt(2)*c(-6). */

        z2 = DEQUANTIZE(inptr[DCTSIZE * 2], quantptr[DCTSIZE * 2]);
        z3 = DEQUANTIZE(inptr[DCTSIZE * 6], quantptr[DCTSIZE * 6]);

        z1 = MULTIPLY(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY(z2, FIX_0_765366865);

        z2 = DEQUANTIZE(inptr[DCTSIZE * 0], quantptr[DCTSIZE * 0]);
        z3 = DEQUANTIZE(inptr[DCTSIZE * 4], quantptr[DCTSIZE * 4]);

        tmp0 = LEFT_SHIFT(z2 + z3, CONST_BITS);
        tmp1 = LEFT_SHIFT(z2 - z3, CONST_BITS);

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        /* Odd part per figure 8; the matrix is unitary and hence its
         * transpose is its inverse.  i0..i3 are y7,y5,y3,y1 respectively.
         */

        tmp0 = DEQUANTIZE(inptr[DCTSIZE * 7], quantptr[DCTSIZE * 7]);
        tmp1 = DEQUANTIZE(inptr[DCTSIZE * 5], quantptr[DCTSIZE * 5]);
        tmp2 = DEQUANTIZE(inptr[DCTSIZE * 3], quantptr[DCTSIZE * 3]);
        tmp3 = DEQUANTIZE(inptr[DCTSIZE * 1], quantptr[DCTSIZE * 1]);

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY(z3 + z4, FIX_1_175875602); /* sqrt(2) * c3 */

        tmp0 = MULTIPLY(tmp0, FIX_0_298631336); /* sqrt(2) * (-c1+c3+c5-c7) */
        tmp1 = MULTIPLY(tmp1, FIX_2_053119869); /* sqrt(2) * ( c1+c3-c5+c7) */
        tmp2 = MULTIPLY(tmp2, FIX_3_072711026); /* sqrt(2) * ( c1+c3+c5-c7) */
        tmp3 = MULTIPLY(tmp3, FIX_1_501321110); /* sqrt(2) * ( c1+c3-c5-c7) */
        z1 = MULTIPLY(z1, -FIX_0_899976223);    /* sqrt(2) * ( c7-c3) */
        z2 = MULTIPLY(z2, -FIX_2_562915447);    /* sqrt(2) * (-c1-c3) */
        z3 = MULTIPLY(z3, -FIX_1_961570560);    /* sqrt(2) * (-c3-c5) */
        z4 = MULTIPLY(z4, -FIX_0_390180644);    /* sqrt(2) * ( c5-c3) */

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        /* Final output stage: inputs are tmp10..tmp13, tmp0..tmp3 */

        wsptr[DCTSIZE * 0] = (int)DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 7] = (int)DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 1] = (int)DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 6] = (int)DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 2] = (int)DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 5] = (int)DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 3] = (int)DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
        wsptr[DCTSIZE * 4] = (int)DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);

        inptr++; /* advance pointers to next column */
        quantptr++;
        wsptr++;
    }

    /* Pass 2: process rows from work array, store into output array. */
    /* Note that we must descale the results by a factor of 8 == 2**3, */
    /* and also undo the PASS1_BITS scaling. */

    wsptr = workspace;
    for (ctr = 0; ctr < DCTSIZE; ctr++)
    {
        outptr = retBlock + retBlockPitch * ctr;
        /* Rows of zeroes can be exploited in the same way as we did with columns.
         * However, the column calculation has created many nonzero AC terms, so
         * the simplification applies less often (typically 5% to 10% of the time).
         * On machines with very fast multiplication, it's possible that the
         * test takes more time than it's worth.  In that case this section
         * may be commented out.
         */

        if (wsptr[1] == 0 && wsptr[2] == 0 && wsptr[3] == 0 && wsptr[4] == 0 && wsptr[5] == 0 &&
            wsptr[6] == 0 && wsptr[7] == 0)
        {
            /* AC terms all zero */
            JSAMPLE dcval = range_limit[(int)DESCALE((JLONG)wsptr[0], PASS1_BITS + 3) & RANGE_MASK];

            outptr[0] = dcval;
            outptr[1] = dcval;
            outptr[2] = dcval;
            outptr[3] = dcval;
            outptr[4] = dcval;
            outptr[5] = dcval;
            outptr[6] = dcval;
            outptr[7] = dcval;

            wsptr += DCTSIZE; /* advance pointer to next row */
            continue;
        }

        /* Even part: reverse the even part of the forward DCT. */
        /* The rotator is sqrt(2)*c(-6). */

        z2 = (JLONG)wsptr[2];
        z3 = (JLONG)wsptr[6];

        z1 = MULTIPLY(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY(z3, -FIX_1_847759065);
        tmp3 = z1 + MULTIPLY(z2, FIX_0_765366865);

        tmp0 = LEFT_SHIFT((JLONG)wsptr[0] + (JLONG)wsptr[4], CONST_BITS);
        tmp1 = LEFT_SHIFT((JLONG)wsptr[0] - (JLONG)wsptr[4], CONST_BITS);

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        /* Odd part per figure 8; the matrix is unitary and hence its
         * transpose is its inverse.  i0..i3 are y7,y5,y3,y1 respectively.
         */

        tmp0 = (JLONG)wsptr[7];
        tmp1 = (JLONG)wsptr[5];
        tmp2 = (JLONG)wsptr[3];
        tmp3 = (JLONG)wsptr[1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY(z3 + z4, FIX_1_175875602); /* sqrt(2) * c3 */

        tmp0 = MULTIPLY(tmp0, FIX_0_298631336); /* sqrt(2) * (-c1+c3+c5-c7) */
        tmp1 = MULTIPLY(tmp1, FIX_2_053119869); /* sqrt(2) * ( c1+c3-c5+c7) */
        tmp2 = MULTIPLY(tmp2, FIX_3_072711026); /* sqrt(2) * ( c1+c3+c5-c7) */
        tmp3 = MULTIPLY(tmp3, FIX_1_501321110); /* sqrt(2) * ( c1+c3-c5-c7) */
        z1 = MULTIPLY(z1, -FIX_0_899976223);    /* sqrt(2) * ( c7-c3) */
        z2 = MULTIPLY(z2, -FIX_2_562915447);    /* sqrt(2) * (-c1-c3) */
        z3 = MULTIPLY(z3, -FIX_1_961570560);    /* sqrt(2) * (-c3-c5) */
        z4 = MULTIPLY(z4, -FIX_0_390180644);    /* sqrt(2) * ( c5-c3) */

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        /* Final output stage: inputs are tmp10..tmp13, tmp0..tmp3 */

        outptr[0] =
            range_limit[(int)DESCALE(tmp10 + tmp3, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[7] =
            range_limit[(int)DESCALE(tmp10 - tmp3, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[1] =
            range_limit[(int)DESCALE(tmp11 + tmp2, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[6] =
            range_limit[(int)DESCALE(tmp11 - tmp2, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[2] =
            range_limit[(int)DESCALE(tmp12 + tmp1, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[5] =
            range_limit[(int)DESCALE(tmp12 - tmp1, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[3] =
            range_limit[(int)DESCALE(tmp13 + tmp0, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];
        outptr[4] =
            range_limit[(int)DESCALE(tmp13 - tmp0, CONST_BITS + PASS1_BITS + 3) & RANGE_MASK];

        wsptr += DCTSIZE; /* advance pointer to next row */
    }
}

static inline uint8_t clamp_val(int val)
{
    if (val < 0)
        return 0;
    if (val > 255)
        return 255;
    return (uint8_t)val;
}
static inline void fill_8x8_block(uint8_t* dst, int row, int col, int row_stride, int value)
{
    uint64_t pattern = (uint64_t)(uint8_t)value * 0x0101010101010101;
    uint8_t* start = dst + row * row_stride + col;
    for (int i = 0; i < 8; i++)
    {
        memcpy(start + i * row_stride, &pattern, sizeof(pattern));
    }
}
static inline void fill_8x8_block(uint8_t* block, int row, int col, int value)
{
    // 用于8x8块（stride=8）
    uint64_t pattern = (uint64_t)(uint8_t)value * 0x0101010101010101ULL;
    uint8_t* start = block + row * 8 + col;
    for (int i = 0; i < 8; i++)
    {
        memcpy(start + i * 8, &pattern, sizeof(pattern));
    }
}
static void Convert16x16BlockToRGBA(const uint8_t* y_block,
                                    const uint8_t* u_block,
                                    const uint8_t* v_block,
                                    const uint8_t* alpha_block,
                                    int alpha_stride,
                                    uint8_t* rgb_output,
                                    int output_stride,
                                    int block_x,
                                    int block_y)
{
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            int uv_x = x >> 1;
            int uv_y = y >> 1;

            int Y = y_block[y * 16 + x];
            int U = u_block[uv_y * 8 + uv_x];
            int V = v_block[uv_y * 8 + uv_x];

            int R = clamp_val(Y + ((359 * (V - 128)) >> 8));
            int G = clamp_val(Y - ((88 * (U - 128) + 183 * (V - 128)) >> 8));
            int B = clamp_val(Y + ((454 * (U - 128)) >> 8));

            int out_x = block_x + x;
            int out_y = block_y + y;
            uint8_t* pixel = rgb_output + (out_y * output_stride + out_x * 4);

            pixel[0] = R;
            pixel[1] = G;
            pixel[2] = B;
            pixel[3] = alpha_block ? alpha_block[y * alpha_stride + x] : 255;
        }
    }
}
//---------------------------------------------------------------------------
// 解码并直接转换为RGBA
//---------------------------------------------------------------------------
static void DecodeAndConvertToRGBA(struct BufferManager* stream,
                                   uint8_t qtbl[3][64],
                                   uint8_t* rgb_output,
                                   int width,
                                   int height,
                                   const uint8_t* alpha_data,
                                   bool has_alpha)
{
    int16_t idcBuff[64] = {0};
    int decoded;

    // 8*8分块处理, U/V单独算一个小块，2*2Y算一个小块
    for (size_t i = 0; i < height / 16; i++)
    {
        for (size_t j = 0; j < width / 16; j++)
        {
            // 临时存储当前16x16块的YUV数据
            uint8_t y_block[256]; // 16x16 Y
            uint8_t u_block[64];  // 8x8 U
            uint8_t v_block[64];  // 8x8 V
            uint8_t a_block[256]; // 16x16 A

            // --- 1. 解码U块 (1个8x8) ---
            memset(idcBuff, 0, 128);
            decode_dc_run_length(&HuffmanTable[0], stream, idcBuff, stream->decodeUVStatus);
            decoded = decode_ac_run_length(&HuffmanTable[1], stream, idcBuff);
            if (decoded == 1)
            {
                int dequantized = idcBuff[0] * qtbl[1][0];
                int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                int pixel = clamp_val((rounded >> 3) + 128);
                fill_8x8_block(u_block, 0, 0, 8, pixel);
            }
            else
            {
                jpeg_idct_data(idcBuff, qtbl[1], u_block, 8);
            }

            // --- 2. 解码V块 (1个8x8) ---
            memset(idcBuff, 0, 128);
            decode_dc_run_length(&HuffmanTable[0], stream, idcBuff, stream->decodeUVStatus);
            decoded = decode_ac_run_length(&HuffmanTable[1], stream, idcBuff);
            if (decoded == 1)
            {
                int dequantized = idcBuff[0] * qtbl[1][0];
                int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                int pixel = clamp_val((rounded >> 3) + 128);
                fill_8x8_block(v_block, 0, 0, 8, pixel);
            }
            else
            {
                jpeg_idct_data(idcBuff, qtbl[1], v_block, 8);
            }

            // --- 3. 解码4个Y块 (每个8x8) ---
            // Y块1 (左上)
            memset(idcBuff, 0, 128);
            decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
            decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
            if (decoded == 1)
            {
                int dequantized = idcBuff[0] * qtbl[0][0];
                int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                int pixel = clamp_val((rounded >> 3) + 128);
                fill_8x8_block(y_block, 0, 0, 16, pixel);
            }
            else
            {
                jpeg_idct_data(idcBuff, qtbl[0], y_block, 16);
            }

            // Y块2 (右上)
            memset(idcBuff, 0, 128);
            decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
            decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
            if (decoded == 1)
            {
                int dequantized = idcBuff[0] * qtbl[0][0];
                int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                int pixel = clamp_val((rounded >> 3) + 128);
                fill_8x8_block(y_block, 0, 8, 16, pixel);
            }
            else
            {
                jpeg_idct_data(idcBuff, qtbl[0], y_block + 8, 16);
            }

            // Y块3 (左下)
            memset(idcBuff, 0, 128);
            decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
            decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
            if (decoded == 1)
            {
                int dequantized = idcBuff[0] * qtbl[0][0];
                int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                int pixel = clamp_val((rounded >> 3) + 128);
                fill_8x8_block(y_block, 8, 0, 16, pixel);
            }
            else
            {
                jpeg_idct_data(idcBuff, qtbl[0], y_block + 8 * 16, 16);
            }

            // Y块4 (右下)
            memset(idcBuff, 0, 128);
            decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
            decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
            if (decoded == 1)
            {
                int dequantized = idcBuff[0] * qtbl[0][0];
                int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                int pixel = clamp_val((rounded >> 3) + 128);
                fill_8x8_block(y_block, 8, 8, 16, pixel);
            }
            else
            {
                jpeg_idct_data(idcBuff, qtbl[0], y_block + 8 * 16 + 8, 16);
            }

            // yuva420p时增加alpha解码
            if (has_alpha)
            {
                memset(idcBuff, 0, 128);
                decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
                decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
                if (decoded == 1)
                {
                    int dequantized = idcBuff[0] * qtbl[2][0];
                    int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                    int pixel = clamp_val((rounded >> 3) + 128);
                    fill_8x8_block(a_block, 0, 0, 16, pixel);
                }
                else
                {
                    jpeg_idct_data(idcBuff, qtbl[2], a_block, 16);
                }

                memset(idcBuff, 0, 128);
                decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
                decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
                if (decoded == 1)
                {
                    int dequantized = idcBuff[0] * qtbl[2][0];
                    int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                    int pixel = clamp_val((rounded >> 3) + 128);
                    fill_8x8_block(a_block, 0, 8, 16, pixel);
                }
                else
                {
                    jpeg_idct_data(idcBuff, qtbl[2], a_block + 8, 16);
                }

                memset(idcBuff, 0, 128);
                decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
                decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
                if (decoded == 1)
                {
                    int dequantized = idcBuff[0] * qtbl[2][0];
                    int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                    int pixel = clamp_val((rounded >> 3) + 128);
                    fill_8x8_block(a_block, 8, 0, 16, pixel);
                }
                else
                {
                    jpeg_idct_data(idcBuff, qtbl[2], a_block + 8 * 16, 16);
                }

                memset(idcBuff, 0, 128);
                decode_dc_run_length(&HuffmanTable[2], stream, idcBuff, stream->decodeYStatus);
                decoded = decode_ac_run_length(&HuffmanTable[3], stream, idcBuff);
                if (decoded == 1)
                {
                    int dequantized = idcBuff[0] * qtbl[2][0];
                    int rounded = (dequantized < 0) ? dequantized + 7 : dequantized;
                    int pixel = clamp_val((rounded >> 3) + 128);
                    fill_8x8_block(a_block, 8, 8, 16, pixel);
                }
                else
                {
                    jpeg_idct_data(idcBuff, qtbl[2], a_block + 8 * 16 + 8, 16);
                }
            }

            // --- 4. 直接转换为RGBA ---
            //yuva420p/yuv420p通过has_alpha=true/false区分
            int block_x = j * 16;
            int block_y = i * 16;
            const uint8_t* alpha_block =
                has_alpha ? a_block
                          : (alpha_data ? alpha_data + (block_y * width + block_x) : nullptr);

            Convert16x16BlockToRGBA(y_block, u_block, v_block, alpha_block, has_alpha ? 16 : width,
                                    rgb_output, width * 4, block_x, block_y);
        }
    }
}


} // anonymous namespace
void DecodePixels(const uint8_t* data, size_t bytes, uint8_t quant[3][64],
                  uint8_t* rgba, uint32_t width, uint32_t height,
                  const uint8_t* alpha, bool dctAlpha) {
    BufferManager input(data, bytes);
    DecodeAndConvertToRGBA(&input, quant, rgba, width, height, alpha, dctAlpha);
}
}} // namespace krkr::amv
