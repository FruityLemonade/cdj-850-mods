/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "decoder.h"
#include "flac_stock.h"
#include <string.h>

/* Stock USB file calls, reached through the per-model address map. */
#define stock_read ((int (*)(void *, int, int, void *))FLAC_STOCK_READ)
#define stock_seek ((int (*)(void *, int, int))FLAC_STOCK_SEEK)
#define stock_tell ((int (*)(void *))FLAC_STOCK_TELL)
#define stock_size ((int (*)(void *, uint32_t *))FLAC_STOCK_SIZE)
#define stock_close ((int (*)(void *))FLAC_STOCK_CLOSE)
extern int flac_stock_classify(unsigned drive, const void *entry, int mode, int flags);
/* flac_stack.s: calls fn(op, arg, value) on the decoder's private stack. */
extern int flac_on_stack(int (*fn)(int, void *, uint32_t), int op, void *arg, uint32_t value);

enum { WAV_HEADER_SIZE = 56, FILE_SLOTS = 8 };
typedef struct {
    void *handle;
    uint32_t compressed_size;
    uint32_t size;
    int raw_offset;
    uint32_t rate; /* 44100 or 48000, from STREAMINFO */
} VirtualFile;

static VirtualFile files[FILE_SLOTS];
static VirtualFile *active;
static uint32_t pcm[FLAC_SECTOR_FRAMES]; /* one sector, WAV byte order */
static uint32_t cached_sector = UINT32_MAX;
static uint32_t next_frame;

/* Emulator diagnostics. Kept in the payload and read through the monitor. */
volatile struct {
    uint32_t opened, virtual_bytes, seeks, failures;
} flac_diagnostics;

static VirtualFile *find(void *handle)
{
    for (unsigned i = 0; i < FILE_SLOTS; ++i)
        if (handle && files[i].handle == handle)
            return &files[i];
    return NULL;
}

static size_t compressed_read(void *context, uint32_t offset, void *dst, size_t size)
{
    VirtualFile *file = context;
    if (file->raw_offset != (int)offset &&
        stock_seek(file->handle, (int)offset, 0) != 0) {
        file->raw_offset = -1;
        return 0;
    }
    int count = stock_read(dst, 1, (int)size, file->handle);
    if (count < 0 || (size_t)count > size) {
        file->raw_offset = -1;
        return 0;
    }
    file->raw_offset = (int)offset + count;
    return (size_t)count;
}

/* The decoder runs on its own stack: dr_flac's open alone takes a 4.4 KB
 * frame, more than the MAIN task stacks can spare. One caller at a time. */
enum { DECODER_OPEN, DECODER_SEEK, DECODER_READ, DECODER_CLOSE };
static volatile int stack_busy;

static int decoder(int op, void *arg, uint32_t value)
{
    switch (op) {
    case DECODER_OPEN: {
        VirtualFile *file = arg;
        FlacSource source = {file, compressed_read, file->compressed_size};
        return (int)flac_open(source);
    }
    case DECODER_SEEK: return flac_seek(value);
    case DECODER_READ: return flac_read_sector(arg);
    default: flac_close(); return 0;
    }
}

static int on_stack(int op, void *arg, uint32_t value)
{
    if (stack_busy) {
        ++flac_diagnostics.failures;
        return op == DECODER_OPEN ? 0 : -1;
    }
    stack_busy = 1;
    int result = flac_on_stack(decoder, op, arg, value);
    stack_busy = 0;
    return result;
}

#define decoder_open(file) ((uint32_t)on_stack(DECODER_OPEN, (file), 0))
#define decoder_seek(frame) on_stack(DECODER_SEEK, NULL, (frame))
#define decoder_read(pcm) on_stack(DECODER_READ, (pcm), 0)
#define decoder_close() ((void)on_stack(DECODER_CLOSE, NULL, 0))

static uint32_t start(VirtualFile *file)
{
    uint32_t frames = decoder_open(file);
    active = frames ? file : NULL;
    next_frame = 0;
    cached_sector = UINT32_MAX;
    return frames;
}

/* Probe only the stock USB audio descriptor. */
int flac_audio_source_size(void *handle, uint32_t *size)
{
    VirtualFile *file = find(handle);
    if (file) {
        *size = file->size;
        return 0;
    }
    int result = stock_size(handle, size);
    if (result != 0 || *size < 42)
        return result;
    uint8_t magic[4];
    int position = stock_tell(handle);
    if (position < 0 || stock_seek(handle, 0, 0) != 0)
        return result;
    int count = stock_read(magic, 1, 4, handle);
    if (stock_seek(handle, position, 0) != 0)
        return -1;
    if (count != 4 || memcmp(magic, "fLaC", 4) != 0)
        return result;
    for (unsigned i = 0; i < FILE_SLOTS; ++i) {
        if (!files[i].handle) {
            file = &files[i];
            break;
        }
    }
    if (!file)
        return -1;
    *file = (VirtualFile){handle, *size, 0, position, 0};
    uint32_t frames = start(file);
    if (!frames || frames > (INT32_MAX - WAV_HEADER_SIZE) / 4) {
        file->handle = NULL;
        active = NULL;
        decoder_close();
        ++flac_diagnostics.failures;
        return -1;
    }
    file->rate = flac_sample_rate();
    file->size = WAV_HEADER_SIZE + frames * 4;
    *size = file->size;
    ++flac_diagnostics.opened;
    return result;
}

int flac_file_classify(unsigned drive, const void *entry, int mode, int flags)
{
    int type = flac_stock_classify(drive, entry, mode, flags);
    if (type != -1 || ((*(const uint8_t *const *)entry)[0x0b] & 0x1e))
        return type;
    const uint16_t *name = (const uint16_t *)((const uint8_t *)entry + 0x12);
    size_t len = 0;
    while (len < 256 && name[len])
        ++len;
    if (len >= 5 && len < 256 && name[len - 5] == '.' &&
        (name[len - 4] | 0x20) == 'f' && (name[len - 3] | 0x20) == 'l' &&
        (name[len - 2] | 0x20) == 'a' && (name[len - 1] | 0x20) == 'c')
        return 11;
    return type;
}

static void write_le32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

/* The stock WAV path resamples other rates to 44.1 kHz for the DSP. */
static void wav_header(uint8_t *header, uint32_t size, uint32_t rate)
{
    static const uint8_t template[WAV_HEADER_SIZE] = {
        'R','I','F','F',0,0,0,0,'W','A','V','E',
        'f','m','t',' ',16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,
        'J','U','N','K',4,0,0,0,0,0,0,0,
        'd','a','t','a',0,0,0,0
    };
    memcpy(header, template, WAV_HEADER_SIZE);
    write_le32(header + 4, size - 8);
    write_le32(header + 24, rate);
    write_le32(header + 28, rate * 4);
    write_le32(header + 52, size - WAV_HEADER_SIZE);
}

static void *current_audio_file(void)
{
    /* The stock open dispatcher records 0 for USB and 1 for optical media. */
    if (*(const volatile int *)FLAC_AUDIO_SOURCE != 0)
        return NULL;
    return *(void *volatile *)FLAC_AUDIO_HANDLE;
}

static int virtual_read(VirtualFile *file, void *dst, uint32_t count, uint32_t position)
{
    if (position >= file->size)
        return 0;
    uint32_t start_position = position;
    uint32_t available = file->size - position;
    uint32_t total = count < available ? count : available;
    uint8_t *out = dst;
    while (total) {
        uint32_t take = total;
        if (position < WAV_HEADER_SIZE) {
            uint8_t header[WAV_HEADER_SIZE];
            wav_header(header, file->size, file->rate);
            if (take > WAV_HEADER_SIZE - position)
                take = WAV_HEADER_SIZE - position;
            memcpy(out, header + position, take);
        } else {
            if (active != file && !start(file))
                break;
            uint32_t sector = (position - WAV_HEADER_SIZE) / FLAC_SECTOR_BYTES;
            uint32_t within = (position - WAV_HEADER_SIZE) % FLAC_SECTOR_BYTES;
            if (cached_sector != sector) {
                uint32_t frame = sector * FLAC_SECTOR_FRAMES;
                if (next_frame != frame) {
                    ++flac_diagnostics.seeks;
                    if (decoder_seek(frame))
                        break;
                }
                int decoded = decoder_read(pcm);
                if (decoded <= 0)
                    break;
                cached_sector = sector;
                next_frame = frame + (uint32_t)decoded;
            }
            if (take > FLAC_SECTOR_BYTES - within)
                take = FLAC_SECTOR_BYTES - within;
            memcpy(out, (const uint8_t *)pcm + within, take);
        }
        out += take;
        position += take;
        total -= take;
    }
    if (total) {
        ++flac_diagnostics.failures;
        active = NULL;
        decoder_close();
    }
    uint32_t bytes = position - start_position;
    flac_diagnostics.virtual_bytes += bytes;
    return total && !bytes ? -1 : (int)bytes;
}

int flac_wav_header_read(int track, uint32_t offset, uint32_t count,
                    uint32_t *read_count, uint8_t **result)
{
    VirtualFile *file = find(current_audio_file());
    if (!file) {
        typedef int (*StockRead)(int, uint32_t, uint32_t, uint32_t *, uint8_t **);
        return ((StockRead)FLAC_STOCK_HEADER)(track, offset, count, read_count, result);
    }
    static uint8_t header_buffer[0x40];
    if (count > sizeof(header_buffer) || offset >= WAV_HEADER_SIZE) {
        *read_count = 0;
        return -1;
    }
    memset(header_buffer, 0, sizeof(header_buffer));
    int bytes = virtual_read(file, header_buffer, count, offset);
    if (bytes < 0) {
        *read_count = 0;
        return -1;
    }
    *read_count = (uint32_t)bytes;
    *result = header_buffer;
    return 0;
}

int flac_wav_pcm_read(int track, uint32_t offset, uint32_t count,
                 uint32_t *read_count, void *destination)
{
    VirtualFile *file = find(current_audio_file());
    if (!file) {
        typedef int (*StockRead)(int, uint32_t, uint32_t, uint32_t *, void *);
        return ((StockRead)FLAC_STOCK_PCM)(track, offset, count, read_count, destination);
    }
    int bytes = virtual_read(file, destination, count, offset);
    if (bytes < 0)
        return -1;
    *read_count = (uint32_t)bytes;
    return 0;
}

int flac_file_close(void *handle)
{
    VirtualFile *file = find(handle);
    if (file) {
        if (active == file) {
            decoder_close();
            active = NULL;
        }
        file->handle = NULL;
    }
    return stock_close(handle);
}

int flac_audio_open(int source, int list, int track, uint32_t *size, char *extension)
{
    typedef int (*OpenAudio)(int, int, int, uint32_t *, char *);
    int file = ((OpenAudio)FLAC_STOCK_OPEN)(source, list, track, size, extension);
    if (source == 1 && file >= 0 && memcmp(extension, "FLAC", 4) == 0) {
        extension[0] = 'W';
        extension[1] = 'A';
        extension[2] = 'V';
        extension[3] = '\0';
    }
    return file;
}
