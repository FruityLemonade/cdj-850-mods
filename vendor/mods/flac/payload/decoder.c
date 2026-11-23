/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "decoder.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DRFLAC_ASSERT(expression) ((void)0)
#define DRFLAC_MALLOC(size) ((void)size, (void *)0)
#define DRFLAC_REALLOC(ptr, size) ((void)ptr, (void)size, (void *)0)
#define DRFLAC_FREE(ptr) ((void)ptr)
/* Only the calls below are used: with the API static, the rest of dr_flac
 * (and its 16 KB-frame read-all helpers) is left out of the payload. */
#define DRFLAC_API static
/* Rice residuals go to rice_residual() below (the one local change in
 * vendor/dr_flac.h). */
#define DRFLAC_RICE_RESIDUAL_HOOK rice_residual
#pragma GCC diagnostic ignored "-Wunused-function"
#include "vendor/dr_flac.h"

enum { ARENA_BYTES = 256 * 1024, MAX_BLOCK_FRAMES = 4608, MAX_LPC_ORDER = 32 };

static union {
    uint64_t alignment;
    uint8_t bytes[ARENA_BYTES];
} arena;
static size_t used;
static FlacSource input;
static uint32_t offset;
static drflac *decoder;
static uint32_t remaining;
static uint32_t sample_rate;

/* The LPC prediction of one sample: sum(c[i] * s[i]) >> shift over 64 bits,
 * with c the coefficients in reverse order and s the previous order samples,
 * oldest first. Exact for every order and precision, so one routine replaces
 * dr_flac's 32- and 64-bit predictors (the 64-bit one is taken for nearly all
 * 16-bit subframes from flac -5..-8: 16 + 15 + log2(order) > 32 bits).
 *
 * On the SH-2A it is one mac.l per coefficient (32 x 32 + 64 into MACH:MACL,
 * both pointers post-incremented): braf enters a 32-step ladder part-way down
 * for the order, with clrmac in its delay slot. MACH/MACL are callee-saved
 * under -mrenesas, and flac_stack.s keeps them for MAIN too. mac.l saturates
 * when SR.S is set, and the SH-2A has no clrs, so rice_residual() leaves such
 * a partition to dr_flac's own code. */
#if defined(__sh__)
/* Written out (not .rept) so GCC's size estimate of the asm, which counts
 * lines, covers the ladder when it places branches and literal pools. */
#define MAC2 "mac.l @%2+,@%3+\n\tmac.l @%2+,@%3+\n\t"
#define MAC8 MAC2 MAC2 MAC2 MAC2
/* skip = (32 - order) * 2 bytes of the ladder to jump over (braf counts from
 * the end of its delay slot, where the ladder starts). */
static inline drflac_int32 predict(const drflac_int32 *c, const drflac_int32 *s, drflac_uint32 skip,
                                   drflac_uint32 right, drflac_uint32 left)
{
    drflac_uint32 hi, lo;
    /* The memory clobber orders the macs after the stores of earlier samples. */
    __asm__ volatile(
        "braf %4\n\t"
        "clrmac\n\t"
        MAC8 MAC8 MAC8 MAC8
        "sts mach,%0\n\t"
        "sts macl,%1"
        : "=&r"(hi), "=&r"(lo), "+r"(c), "+r"(s)
        : "r"(skip)
        : "mach", "macl", "memory");
    /* (hi:lo) >> shift with right = -shift, left = 31 - shift: exact for 0..31 */
    __asm__("shld %2,%0\n\tshld %3,%1\n\tadd %1,%1\n\tor %1,%0"
            : "+&r"(lo), "+&r"(hi) : "r"(right), "r"(left));
    return (drflac_int32)lo;
}
#undef MAC2
#undef MAC8
#else
static inline drflac_int32 predict(const drflac_int32 *c, const drflac_int32 *s, drflac_uint32 order,
                                   drflac_uint32 shift, drflac_uint32 unused)
{
    (void)unused;
    drflac_int64 sum = 0;
    for (drflac_uint32 i = 0; i < order; ++i)
        sum += (drflac_int64)c[i] * s[i];
    return (drflac_int32)(sum >> shift);
}
#endif

/* Leading zeros of a byte (8 for 0). */
#define R2(n) n, n
#define R4(n) R2(n), R2(n)
#define R8(n) R4(n), R4(n)
#define R16(n) R8(n), R8(n)
#define R32(n) R16(n), R16(n)
#define R64(n) R32(n), R32(n)
static const unsigned char clz8[256] = {8, 7, R2(6), R4(5), R8(4), R16(3), R32(2), R64(1), R64(0), R64(0)};

/* One Rice partition. A code whose unary run ends in the top byte of the bit
 * cache and whose low bits are in the same cache word is read in place, with
 * the cache held in locals for the whole partition; any other code (a cache
 * refill, a long run) goes through dr_flac's own reader, so its cache, refill
 * and CRC-16 state move exactly as in dr_flac. One sample per iteration
 * instead of four (the SH-2A has 16 registers and the unrolled loop spilled),
 * and predict() in place of dr_flac's per-order switch. */
static drflac_bool32 rice_residual(drflac_bs *bs, drflac_uint32 bitsPerSample, drflac_uint32 count,
                                   drflac_uint8 riceParam, drflac_uint32 order, drflac_int32 shift,
                                   drflac_uint32 precision, const drflac_int32 *coefficients,
                                   drflac_int32 *out)
{
    drflac_bool32 plain = order == 0 || order > MAX_LPC_ORDER || shift < 0 || shift > 31 ||
                          riceParam > 30;
#if defined(__sh__)
    drflac_uint32 sr;
    __asm__ volatile("stc sr,%0" : "=r"(sr));
    plain |= (sr & 2) != 0;
#endif
    if (plain)
        return drflac__decode_samples_with_residual__rice__scalar(
            bs, bitsPerSample, count, riceParam, order, shift, precision, coefficients, out);
    drflac_int32 reversed[MAX_LPC_ORDER];
    for (unsigned i = 0; i < order; ++i)
        reversed[i] = coefficients[order - 1 - i];
    const drflac_uint32 mask = riceParam ? ~(drflac_uint32)0 >> (32 - riceParam) : 0;
    enum { CACHE_BITS = sizeof(bs->cache) * 8 };
    const drflac_uint32 width = riceParam + 1u, limit = CACHE_BITS - width;
#if defined(__sh__)
    const drflac_uint32 a = (MAX_LPC_ORDER - order) * 2u, b = 0u - (drflac_uint32)shift,
                        d = 31u - (drflac_uint32)shift;
#else
    const drflac_uint32 a = order, b = (drflac_uint32)shift, d = 0;
#endif
    drflac_cache_t cache = bs->cache;
    drflac_uint32 consumed = bs->consumedBits;
    for (drflac_int32 *end = out + count; out < end; ++out) {
        drflac_uint32 zeros = clz8[(drflac_uint32)(cache >> (CACHE_BITS - 8))], part;
        if (zeros < 8 && consumed + zeros <= limit) {
            /* part keeps the run's stop bit above the low bits, as dr_flac's does */
            cache <<= zeros;
            part = (drflac_uint32)(cache >> (CACHE_BITS - width));
            cache <<= width;
            consumed += zeros + width;
        } else if (zeros < 8 && bs->nextL2Line < DRFLAC_CACHE_L2_LINE_COUNT(bs)) {
            /* The low bits run into the next cache word, which is buffered:
             * dr_flac's straddle path, with its CRC-16 update. */
            cache <<= zeros;
            consumed += zeros;
            drflac_uint32 low = consumed - limit;
            part = (drflac_uint32)(cache >> (CACHE_BITS - width));
#ifndef DR_FLAC_NO_CRC
            drflac__update_crc16(bs);
#endif
            cache = drflac__be2host__cache_line(bs->cacheL2[bs->nextL2Line++]);
#ifndef DR_FLAC_NO_CRC
            bs->crc16Cache = cache;
#endif
            part |= (drflac_uint32)(cache >> (CACHE_BITS - low));
            cache <<= low;
            consumed = low;
        } else {
            bs->cache = cache;
            bs->consumedBits = consumed;
            if (!drflac__read_rice_parts_x1(bs, riceParam, &zeros, &part))
                return DRFLAC_FALSE;
            cache = bs->cache;
            consumed = bs->consumedBits;
        }
        drflac_uint32 value = (part & mask) | zeros << riceParam;
        value = (value >> 1) ^ (0u - (value & 1u));
        *out = (drflac_int32)value + predict(reversed, out - order, a, b, d);
    }
    bs->cache = cache;
    bs->consumedBits = consumed;
    return DRFLAC_TRUE;
}
#undef R2
#undef R4
#undef R8
#undef R16
#undef R32
#undef R64

static void *allocate(size_t size, void *context)
{
    (void)context;
    if (size > ARENA_BYTES - used)
        return NULL;
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned > ARENA_BYTES - used)
        return NULL;
    void *result = arena.bytes + used;
    used += aligned;
    return result;
}

static void release(void *address, void *context)
{
    (void)address;
    (void)context;
}

static size_t read_bytes(void *context, void *dst, size_t size)
{
    (void)context;
    if (size > input.size - offset)
        size = input.size - offset;
    size_t actual = input.read_at(input.context, offset, dst, size);
    if (actual > size)
        return 0;
    offset += (uint32_t)actual;
    return actual;
}

static drflac_bool32 seek_bytes(void *context, int distance, drflac_seek_origin origin)
{
    (void)context;
    uint32_t base = 0;
    switch (origin) {
    case DRFLAC_SEEK_SET: break;
    case DRFLAC_SEEK_CUR: base = offset; break;
    case DRFLAC_SEEK_END: base = input.size; break;
    default: return DRFLAC_FALSE;
    }
    if (distance < 0) {
        uint32_t back = 0u - (uint32_t)distance;
        if (back > base)
            return DRFLAC_FALSE;
        offset = base - back;
    } else {
        if ((uint32_t)distance > input.size - base)
            return DRFLAC_FALSE;
        offset = base + (uint32_t)distance;
    }
    return DRFLAC_TRUE;
}

static drflac_bool32 tell_bytes(void *context, drflac_int64 *position)
{
    (void)context;
    *position = offset;
    return DRFLAC_TRUE;
}

void flac_close(void)
{
    if (decoder != NULL)
        drflac_close(decoder);
    decoder = NULL;
    used = 0;
    remaining = 0;
    sample_rate = 0;
}

uint32_t flac_open(FlacSource source)
{
    uint8_t info[26];
    flac_close();
    if (!source.read_at || source.size < 42 || source.size > INT32_MAX ||
        source.read_at(source.context, 0, info, sizeof(info)) != sizeof(info))
        return 0;

    /* Validate native STREAMINFO before allocating or parsing frame data. */
    unsigned max_block = (unsigned)info[10] << 8 | info[11];
    unsigned rate = (unsigned)info[18] << 12 | (unsigned)info[19] << 4 | info[20] >> 4;
    unsigned channels = ((info[20] >> 1) & 7u) + 1u;
    unsigned bits = ((info[20] & 1u) << 4 | info[21] >> 4) + 1u;
    if (memcmp(info, "fLaC", 4) || (info[4] & 0x7f) ||
        info[5] || info[6] || info[7] != 34 || max_block < 16 ||
        max_block > MAX_BLOCK_FRAMES || (rate != 44100 && rate != 48000) ||
        channels != 2 || (bits != 16 && bits != 24) ||
        (info[21] & 0xf))
        return 0;

    input = source;
    offset = 0;
    const drflac_allocation_callbacks memory = {NULL, allocate, NULL, release};
    decoder = drflac_open(read_bytes, seek_bytes, tell_bytes, NULL, &memory);
    if (!decoder || !decoder->totalPCMFrameCount || decoder->totalPCMFrameCount > UINT32_MAX) {
        flac_close();
        return 0;
    }
    remaining = (uint32_t)decoder->totalPCMFrameCount;
    sample_rate = rate;
    return remaining;
}

uint32_t flac_sample_rate(void)
{
    return decoder ? sample_rate : 0;
}

int flac_seek(uint32_t frame)
{
    if (!decoder || frame >= decoder->totalPCMFrameCount)
        return -1;
    if (decoder->totalPCMFrameCount - remaining == frame)
        return 0;
    if (!drflac_seek_to_pcm_frame(decoder, frame)) {
        flac_close();
        return -1;
    }
    remaining = (uint32_t)decoder->totalPCMFrameCount - frame;
    return 0;
}

/* Left and right 16-bit samples as one stereo frame of the WAV's data, in
 * file (little-endian) byte order. */
static inline uint32_t wav_frame(uint32_t left, uint32_t right)
{
#if defined(__sh__)
    /* swap.b swaps the low two bytes; xtrct joins left's low half above right's */
    __asm__("swap.b %0,%0\n\tswap.b %1,%1\n\tshll16 %1\n\txtrct %0,%1" : "+&r"(left), "+&r"(right));
    return right;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (left & 0xffu) << 24 | (left & 0xff00u) << 8 | (right & 0xffu) << 8 | (right >> 8 & 0xffu);
#else
    return (left & 0xffffu) | right << 16;
#endif
}

/* n frames of the current FLAC frame from first on, as dr_flac's s16 reader
 * computes them (wasted bits, then the stereo decorrelation, then the top 16
 * of 32 bits), written straight to out. */
static int convert(uint32_t *out, unsigned first, unsigned n)
{
    const drflac_frame *frame = &decoder->currentFLACFrame;
    const drflac_uint32 *a = (const drflac_uint32 *)frame->subframes[0].samples.pS32 + first;
    const drflac_uint32 *b = (const drflac_uint32 *)frame->subframes[1].samples.pS32 + first;
    unsigned unused = 32u - frame->header.bitsPerSample;
    unsigned w0 = frame->subframes[0].wastedBitsPerSample;
    unsigned w1 = frame->subframes[1].wastedBitsPerSample;
    if (drflac__get_channel_count_from_channel_assignment(frame->header.channelAssignment) != 2 ||
        unused == 0 || unused > 31 || unused + w0 > 31 || unused + w1 > 31)
        return -1;
    switch (frame->header.channelAssignment) {
    case DRFLAC_CHANNEL_ASSIGNMENT_LEFT_SIDE:
        for (unsigned i = 0; i < n; ++i) {
            uint32_t left = a[i] << (unused + w0);
            out[i] = wav_frame(left >> 16, (left - (b[i] << (unused + w1))) >> 16);
        }
        break;
    case DRFLAC_CHANNEL_ASSIGNMENT_RIGHT_SIDE:
        for (unsigned i = 0; i < n; ++i) {
            uint32_t right = b[i] << (unused + w1);
            out[i] = wav_frame((right + (a[i] << (unused + w0))) >> 16, right >> 16);
        }
        break;
    case DRFLAC_CHANNEL_ASSIGNMENT_MID_SIDE:
        for (unsigned i = 0; i < n; ++i) {
            uint32_t side = b[i] << w1;
            uint32_t mid = (a[i] << w0) << 1 | (side & 1u);
            out[i] = wav_frame((mid + side) << (unused - 1) >> 16, (mid - side) << (unused - 1) >> 16);
        }
        break;
    default:
        for (unsigned i = 0; i < n; ++i)
            out[i] = wav_frame(a[i] << (unused + w0) >> 16, b[i] << (unused + w1) >> 16);
        break;
    }
    return 0;
}

int flac_read_sector(uint32_t *pcm)
{
    if (!decoder)
        return -1;
    unsigned take = remaining < FLAC_SECTOR_FRAMES ? remaining : FLAC_SECTOR_FRAMES;
    if (!take)
        return 0;
    for (unsigned done = 0; done < take;) {
        drflac_frame *frame = &decoder->currentFLACFrame;
        if (frame->pcmFramesRemaining == 0) {
            if (!drflac__read_and_decode_next_flac_frame(decoder)) {
                flac_close();
                return -1;
            }
            continue;
        }
        unsigned n = take - done;
        if (n > frame->pcmFramesRemaining)
            n = frame->pcmFramesRemaining;
        unsigned first = frame->header.blockSizeInPCMFrames - frame->pcmFramesRemaining;
        if (convert(pcm + done, first, n)) {
            flac_close();
            return -1;
        }
        done += n;
        decoder->currentPCMFrame += n;
        frame->pcmFramesRemaining -= n;
    }
    memset(pcm + take, 0, (FLAC_SECTOR_FRAMES - take) * 4);
    remaining -= take;
    return (int)take;
}
