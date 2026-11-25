/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hot_cues: BEAT 1/2/4 as hot cues A/B/C on USB tracks with a rekordbox ANLZ
 * (see ../patch.py, ../../README.md and docs/cdj850-modding.md section 4b.11).
 *
 *   BEAT SELECT + RELOOP  toggles hot-cue mode (the mod flags byte of
 *                         c850_ui.h, settings record +0x22, saved to flash) and
 *                         flashes HOT CUE ON / HOT CUE OFF on the display.
 *                         BEAT SELECT alone and RELOOP alone stay stock.
 *   pad (BEAT 1/2/4)      set slot: jump to it (CALL path; playing stays playing,
 *                         paused/cued stays cued at the cue); a loop slot arms the
 *                         loop (and plays it when the deck was playing).
 *                         empty slot: record the position (in a playing loop: the
 *                         loop) into the slot and write the ANLZ. With quant_loops
 *                         and quantize on, a point goes to the nearest beat.
 *   hold BEAT 8 + pad     delete the slot and write the ANLZ.
 *   LEDs                  1/2/4 lit = slot set, blinking = empty; 8 lit.
 *
 * Without a loaded USB track whose ANLZ is in the ANLZ cache (CD, non-rekordbox
 * files, no ANLZ) every key is stock.
 *
 * Entry points (redirected pool words, ../patch.py):
 *   c850_ui_keys        0x0C0ED038 (FUN_0C0DD492) inside panel_keys_to_internal_words,
 *                       right after K0..K2 are rebuilt: the shared key filter chain
 *                       (payload/common/c850_ui.c), which calls hot_cues_key_filter.
 *   c850_ui_flash_hook  tail of display_stage_bpm: the HOT CUE ON/OFF flash.
 *   hot_cues_tick       0x0C0EC838 (FUN_0C0EB388), first call of FUN_0C0EC73C (state
 *                       tick, before the key handlers): actions, PLAY, availability.
 *   hot_cues_select     0x0C0D41BC and 0x0C0D3CC8 (cue_select_from_list) in
 *                       cue_call_prev_next's two USB branches: the hot cue instead
 *                       of a memory cue for one call.
 *   hot_cues_wait       0x0C0ED244 (wai_flg) of the playertask loop: beat-loop LED
 *                       inputs faked while playertask sleeps.
 *   hot_cues_anlz_write 0x0C0C195C (FUN_0C0C5960) in the hot-cue setter FUN_0C0C166C:
 *                       empties a slot for a delete, fixes the tag's cue count.
 *   hot_cues_ring_request      0x0C0F54D8 (FUN_0C008C8A), the audio task's cue
 *                       prefill (gated): restores a cached cue, else a short prefill.
 *   hot_cues_play_ring_request 0x0C0F73A8 (FUN_0C008C8A) in player_tick_usb_playback:
 *                       holds the read-ahead while the restore's continuation reads.
 *   hot_cues_play_tick  0x0C014E8C (FUN_0C0F65F0, AudioTask audio state 3, gated) and
 *   hot_cues_usb_tick   0x0C014E90 (player_tick_usb_playback, states 4/5, gated):
 *                       schedule / abort a background cue pre-cache decode.
 *   hot_cues_beba0      0x0C0BE0CC (FUN_0C0BEBA0, mp3ActTask decode dispatch, gated),
 *   hot_cues_pcm_init   0x0C0BF818 (pcm_ring_set_sector) and
 *   hot_cues_pcm_write  0x0C0C056C (pcm_ring_write) in the decoder: run a tagged
 *                       job's decode into the SDRAM cue cache instead of the ring.
 *
 * Integer only (no FPU here; the MAIN routines called use the FPU as they do
 * for stock). Key handling runs on playertask; the cache restore and the
 * pre-cache scheduler on AudioTask, the diverted decode on mp3ActTask.
 */
#include "c850.h"
#include "c850_ui.h"

/* ---- MAIN state and routines (section 4b.11) ---- */
/* Hot-cue mode is C850_UI_FLAG_HOT_CUES of the mod flags byte (c850_ui.h):
 * the last byte of the 32-byte HISTORY name field of the settings record
 * (section 4b.6; no flag bit of the record is free). A name change (factory
 * reset, a new name) clears it: mode off. */
/* FUN_0C0746BC(track): the DB record (+0x1B8) of the loaded track number, as
 * the cue list loader FUN_0C0D23E8 takes it; +4 is the file's path, the key
 * of the ANLZ calls. 0 = none (not USB, no DB track). */
#define HC_DB_RECORD        C850_FN(0x0C0746BC, u32, (int track))
#define HC_LOADED_TRACK     C850_S16(0x0C7F6484)
#define HC_ANLZ_READ        C850_FN(0x0C0C12B2, int, (u32 ctx, u32 key, u32 *buf, int hot))
#define HC_ANLZ_SET         C850_FN(0x0C0C1442, int, (u32 ctx, u32 key, u32 rec, int hot))
#define HC_ANLZ_WRITE       C850_FN(0x0C0C5960, int, (u32 file, u32 ctx, int section, u32 buf))
#define HC_HASH1            C850_FN(0x0C0C3550, u32, (u32 key))
#define HC_HASH2            C850_FN(0x0C0C359A, u32, (u32 key))
#define HC_REC_TO_ENTRY     C850_FN(0x0C01DE1C, int, (u32 rec, u32 *entry))
#define HC_ENTRY_TO_REC     C850_FN(0x0C01DD8C, int, (u32 *entry, u32 rec))
#define HC_SELECT_STOCK     C850_FN(0x0C0D275A, int, (int src, int dir, int x))
#define HC_CUE_LIST_CHANGED C850_FN(0x0C0D5978, void, (void))
#define HC_WAI_FLG          C850_FN(0x0C0C9E7C, int, (int flg, u32 ptn, int mode, u32 *out, int tmo))

/* ANLZ context (*0x0CE649D0): the hot-cue tag of the current file */
#define HC_CTX_MAGIC0       0xBFA14C7Eu                  /* ctx+0, ctx+0x1414 (FUN_0C0C12B2) */
#define HC_CTX_MAGIC1       0x9768A153u
#define HC_CTX_HASH1        0x488                        /* hashes of the file the ctx holds */
#define HC_CTX_HASH2        0x48C
#define HC_CTX_EXTRAS       0xF74                        /* further PCPT after the 3 slots */
#define HC_CTX_TAGLEN       0xF80                        /* PCOB tag length */
#define HC_CTX_NCUES        0xF8A                        /* PCOB number of cues (u16) */
#define HC_CTX_SLOT(n)      (0xF58 + (n) * 0x38)         /* PCPT of hot cue n = 1..3 */
#define HC_SLOT_NUM         0x0E                         /* u16 hot cue number, 0 = empty */
#define HC_PCPT             0x38

/* cue_select_from_list's USB outputs (section 4b.3) */
#define HC_SEL_MODE         C850_U8(0x0CE65859)
#define HC_TARGET           C850_JUMP_TARGET            /* 0x0CE65C0C */
#define HC_TRACK_NO         C850_U16(0x0C7F6488)

#define HC_K1_PADS          (C850_K1_BEAT1 | C850_K1_BEAT2 | C850_K1_BEAT4)
#define HC_K1_BEATS         (HC_K1_PADS | C850_K1_BEAT8)

/* Run-time switch: 0 = every key stock (the mode bit is left alone). */
volatile u32 hot_cues_on = 1;

/* Evidence for tests. */
volatile u32 hot_cues_toggles;       /* BEAT SELECT + RELOOP */
volatile u32 hot_cues_jumps;         /* jumps the CALL path took */
volatile u32 hot_cues_records;       /* slots written by a record */
volatile u32 hot_cues_deletes;       /* slots emptied by a delete */
volatile u32 hot_cues_plays;         /* PLAY presses given after a jump */
volatile u32 hot_cues_failed;        /* actions that did nothing (no cue, write error) */
volatile u32 hot_cues_last_slot;     /* 1..3 */
volatile u32 hot_cues_last_in;       /* frame of the last jump/record */
volatile u32 hot_cues_last_out;      /* its loop out, 0 = none */
volatile u32 hot_cues_snapped;       /* point records moved to a beat (quantize) */
volatile u32 hot_cues_last_pos;      /* play position of the last point record */
volatile s32 hot_cues_last_result;   /* FUN_0C0C1442's result of the last write */
volatile u32 hot_cues_avail;         /* 1 = the loaded track's ANLZ is in the cache */
volatile u32 hot_cues_mask;          /* set slots (bit n-1 = hot cue n) at the last look */
volatile u32 hot_cues_leds;          /* waits with the LED inputs faked */
volatile u32 hot_cues_owned;         /* key presses kept from the stock handlers */

static u8 hc_queue[3];               /* per pad: 1 trigger, 2 delete */
static u8 hc_had_key;
static u32 hc_h1, hc_h2;             /* hashes of the loaded file (ANLZ cache key) */
static u8 hc_sel;                    /* 1: the next USB cue selection is hc_entry */
static u8 hc_play;                   /* PLAY presses left to give after a jump */
static u8 hc_play_down;              /* our PLAY press is in K0 */
static u8 hc_play_wait;              /* ticks until the next press */
static u8 hc_del;                    /* hot cue being deleted (1..3), 0 = none */
static u8 hc_fixlen;                 /* 1: a hot_cues write; fix the PCOB count */
static u32 hc_entry[9];              /* 0x24-byte cue list entry */
static u32 hc_rec[8];                /* 0x20-byte cue record (FUN_0C0C12B2 / FUN_0C0C1442) */
static u32 hc_buf[0x60];             /* FUN_0C0C12B2 output: count + 10 records */

/* ---- Hot-cue PCM snapshot cache in unused high SDRAM ----
 *
 * payload.ld emits .bss as zeroes into the loadable binary, so the ~1.02 MB
 * snapshot buffer cannot live in .bss. Three 150-sector (2.0 s) forward slots
 * sit at HC_CACHE_BASE..HC_CACHE_END just below 0x0DF6FFFC, leaving >35 KB clear
 * above __payload_end even when all CDJ-850 payload mods (including flac) are
 * built together. Each slot stores 150 x 2352 B PCM followed by 150 x 16 B
 * ring metadata.
 */
extern u8 _payload_end[];

#define HC_CACHE_SLOTS       3u
#define HC_CACHE_SECTORS     150u    /* 2.0 s forward snapshot per slot */
#define HC_CACHE_MIN_SECTORS 26u     /* >25 ensures 120 + N > 145 in audio case 0x73 */
#define HC_SECTOR_LONGWORDS  588u    /* 2352 B / 4 */
#define HC_META_BYTES        16u
#define HC_META_BASE         0x0C750C3Cu
#define HC_SLOT_PCM_BYTES    (HC_CACHE_SECTORS * C850_PCM_SECTOR_BYTES) /* 352,800 B = 0x56220 */
#define HC_SLOT_META_BYTES   (HC_CACHE_SECTORS * HC_META_BYTES)         /*   2,400 B = 0x00960 */
#define HC_SLOT_STRIDE       (HC_SLOT_PCM_BYTES + HC_SLOT_META_BYTES)  /* 355,200 B = 0x56B80 */
#define HC_CACHE_END         0x0DF6FFF0u
#define HC_CACHE_BASE        (HC_CACHE_END - HC_CACHE_SLOTS * HC_SLOT_STRIDE) /* 0x0DE6BD70 */

#define HC_CACHE_EMPTY       0u
#define HC_CACHE_PENDING     1u
#define HC_CACHE_PRECACHE    2u
#define HC_CACHE_READY       3u

#define HC_MEMCPY            C850_FN(0x0C1100B8, void *, (u32 dst, u32 src, u32 len))
#define HC_FRAMES_TO_MSF     C850_FN(0x0C003E56, void, (u32 frame, u32 msf_ptr))

typedef struct {
    u32 h1;
    u32 h2;
    u32 cue_frame;
    u32 out_frame;
    u16 sectors;
    u8  state;
    u8  is_loop;
} hc_cache_slot_t;

volatile u32 hot_cues_precache = 1;  /* 0 = disable snapshot capture/restore/precache */
volatile u32 hot_cues_cache_hits;    /* jumps restored directly from SDRAM cache */
volatile u32 hot_cues_cache_captures;/* cache slots populated */
volatile u32 hot_cues_precaches;     /* background pre-cache decodes completed */
volatile u32 hot_cues_precache_aborts; /* background pre-caches preempted/aborted */
volatile u32 hot_cues_cache_mask;    /* bit n-1 = cache slot n is READY */
volatile u32 hot_cues_cache_last_sectors;

static hc_cache_slot_t hc_cache[HC_CACHE_SLOTS];
static volatile u8 hc_restore_slot;  /* 1..3 armed for audio task case 0x77 */
static volatile u32 hc_restore_frame;
static volatile u32 hc_restore_since;
static volatile u8 hc_stream_inflight; /* 1 while case 0x77 continuation read is running */
static volatile u32 hc_stream_since;
static volatile u8 hc_bg_inflight;   /* 1 while AudioTask dispatched a bg decode */
static volatile u8 hc_bg_cancel;     /* 1 to abort/discard in-flight bg decode */
static volatile u8 hc_bg_divert;     /* 1 inside FUN_0c0beba0 for diverted bg decode */
static volatile u8 hc_bg_slot;       /* 1..3 */
static volatile u16 hc_bg_want;
static volatile u16 hc_bg_sectors;
static volatile u32 hc_bg_frame;
static volatile u32 hc_bg_h1;
static volatile u32 hc_bg_h2;
static volatile u32 hc_bg_since;
static volatile u32 hc_bg_longwords;

static u32 hc_cache_pcm(u32 slot)
{
    return HC_CACHE_BASE + slot * HC_SLOT_STRIDE;
}

static u32 hc_cache_meta(u32 slot)
{
    return HC_CACHE_BASE + slot * HC_SLOT_STRIDE + HC_SLOT_PCM_BYTES;
}

static u32 hc_find(u32 n);

static int hc_cache_budget_ok(void)
{
    return ((u32)_payload_end <= HC_CACHE_BASE);
}

static void hc_cache_update_mask(void)
{
    u32 i, m = 0;

    for (i = 0; i < HC_CACHE_SLOTS; i++)
        if (hc_cache[i].state == HC_CACHE_READY)
            m |= 1u << i;
    hot_cues_cache_mask = m;
}

static void hc_bg_abort(void)
{
    if (hc_bg_inflight) {
        hc_bg_cancel = 1u;
        C850_U8(0x0CE5A9ECu) = 2u;
        C850_U8(0x0C7F670Eu) = 1u;
    }
}

static void hc_cache_invalidate_all(void)
{
    u32 i;

    hc_bg_abort();
    hc_restore_slot = 0;
    hc_stream_inflight = 0;
    for (i = 0; i < HC_CACHE_SLOTS; i++) {
        hc_cache[i].state = HC_CACHE_EMPTY;
        hc_cache[i].sectors = 0;
        hc_cache[i].h1 = 0;
        hc_cache[i].h2 = 0;
        hc_cache[i].cue_frame = 0;
        hc_cache[i].out_frame = 0;
        hc_cache[i].is_loop = 0;
    }
    hc_cache_update_mask();
}

static void hc_cache_set_slot(u32 slot, u32 cue_frame, u32 out_frame, u8 is_loop)
{
    hc_cache_slot_t *s;

    if (slot >= HC_CACHE_SLOTS)
        return;
    s = &hc_cache[slot];
    if (s->state == HC_CACHE_READY && s->h1 == hc_h1 && s->h2 == hc_h2 &&
        s->cue_frame == cue_frame && s->out_frame == out_frame && s->is_loop == is_loop)
        return;
    s->h1 = hc_h1;
    s->h2 = hc_h2;
    s->cue_frame = cue_frame;
    s->out_frame = out_frame;
    s->is_loop = is_loop;
    s->sectors = 0;
    s->state = HC_CACHE_PENDING;
    hc_cache_update_mask();
}

static void hc_cache_clear_slot(u32 slot)
{
    if (slot >= HC_CACHE_SLOTS)
        return;
    if (hc_bg_inflight && hc_bg_slot == slot + 1u)
        hc_bg_abort();
    if (hc_restore_slot == slot + 1u)
        hc_restore_slot = 0;
    hc_cache[slot].state = HC_CACHE_EMPTY;
    hc_cache[slot].sectors = 0;
    hc_cache_update_mask();
}

static int hc_cache_match(u32 slot, u32 cue_frame)
{
    const hc_cache_slot_t *s;

    if (slot >= HC_CACHE_SLOTS)
        return 0;
    s = &hc_cache[slot];
    return (s->state == HC_CACHE_READY && s->h1 == hc_h1 && s->h2 == hc_h2 &&
            s->cue_frame == cue_frame && s->sectors > 0u);
}

static void hc_cache_sync_from_buf(void)
{
    u32 n, rec;

    for (n = 1u; n <= HC_CACHE_SLOTS; n++) {
        rec = hc_find(n);
        if (!rec) {
            hc_cache_clear_slot(n - 1u);
            continue;
        }
        if (HC_REC_TO_ENTRY(rec, hc_entry) == 1) {
            u8 loop = (hc_entry[0] & 1u) ? 1u : 0u;
            hc_cache_set_slot(n - 1u, hc_entry[3], loop ? hc_entry[4] : 0u, loop);
        }
    }
}

/* The loop out in effect (docs 4b.10, as loop_resize follows it): a resize
 * (BEAT in a loop, loop_resize's CALL) writes the requested out 0x0C7F64DC and
 * the feeder loops to it, while the out record 0x0C7F64B0 keeps the old out
 * until EXIT/RELOOP; a new loop sets 0x0C7F64B0 and leaves 0x0C7F64DC stale.
 * hc_track_out() runs every player cycle (hot_cues_tick). */
static u32 hc_dc_seen, hc_b0_seen;
static u8 hc_dc_fresh;

static void hc_track_out(void)
{
    u32 dc = C850_LOOP_OUT_REQUEST, b0 = C850_LOOP_OUT_FRAME;

    if (b0 != hc_b0_seen)
        hc_dc_fresh = 0;
    else if (dc != hc_dc_seen)
        hc_dc_fresh = 1;
    hc_dc_seen = dc;
    hc_b0_seen = b0;
}

u32 hot_cues_effective_out(u32 in)
{
    hc_track_out();
    if (hc_dc_fresh && C850_LOOP_OUT_REQUEST > in)
        return C850_LOOP_OUT_REQUEST;
    return C850_LOOP_OUT_FRAME;
}

int hot_cues_mode(void)
{
    if (!hot_cues_on)
        return 0;
    return c850_ui_hot_cues_active();
}

static const char *hc_utility_value(void)
{
    if (!hot_cues_on)
        return 0;
    return c850_ui_flag(C850_UI_FLAG_HOT_CUES_OFF) ? "OFF" : "ON";
}

static void hc_utility_select(void)
{
    int off = !c850_ui_flag(C850_UI_FLAG_HOT_CUES_OFF);

    c850_ui_set_flag(C850_UI_FLAG_HOT_CUES_OFF, off);
    if (off)
        c850_ui_set_hot_cues_armed(0);
}

static const c850_ui_utility_item hc_utility_item = {
    75u, "HOT CUES", hc_utility_value, hc_utility_select,
};
C850_UI_UTILITY_ITEM(hc_utility_item);

#define HC_PLAY_TRIES 16
#define HC_PLAY_GAP   10

static u32 hc_key(void)
{
    return HC_DB_RECORD(HC_LOADED_TRACK);
}

static int hc_ctx_ok(u32 ctx)
{
    return ctx && (ctx & 3) == 0 && C850_U32(ctx) == HC_CTX_MAGIC0 && C850_U32(ctx + 0x1414) == HC_CTX_MAGIC1;
}

/* The ANLZ cache holds the loaded file (cheap, no call): LEDs and keys. */
static u32 hc_ctx_current(void)
{
    u32 ctx = C850_ANLZ_CTX;

    if (!hot_cues_avail || !hc_ctx_ok(ctx) || C850_U32(ctx + HC_CTX_HASH1) != hc_h1 ||
        C850_U32(ctx + HC_CTX_HASH2) != hc_h2)
        return 0;
    return ctx;
}

/* Set slots of the cached file: bit n-1 = hot cue n. */
u32 hot_cues_slot_mask(u32 ctx)
{
    u32 n, m = 0;

    for (n = 1; n <= 3; n++)
        if (C850_U16(ctx + HC_CTX_SLOT(n) + HC_SLOT_NUM) != 0)
            m |= 1u << (n - 1);
    return m;
}

/* Read the hot cues of the loaded file into hc_buf (FUN_0C0C12B2 loads the
 * file into the cache when needed); 1 = the cache now holds that file. A file
 * without ANLZ leaves the cache on another file (FUN_0C0C12B2 still returns 0). */
static int hc_read(u32 key)
{
    u32 ctx = C850_ANLZ_CTX;

    if (!key || !hc_ctx_ok(ctx))
        return 0;
    if (HC_ANLZ_READ(ctx, key + 4, hc_buf, 1) != 0)
        return 0;
    hc_h1 = HC_HASH1(key + 4);
    hc_h2 = HC_HASH2(key + 4);
    if (C850_U32(ctx + HC_CTX_HASH1) != hc_h1 || C850_U32(ctx + HC_CTX_HASH2) != hc_h2)
        return 0;
    return 1;
}

/* Record of hot cue n in hc_buf, or 0. */
static u32 hc_find(u32 n)
{
    u32 k, cnt = hc_buf[0];

    for (k = 0; k < cnt && k < 10; k++) {
        u32 rec = (u32)&hc_buf[1 + 8 * k];
        if (C850_U16(rec) == n)
            return rec;
    }
    return 0;
}

static int hc_state_ok(u32 st)
{
    return st >= 3 && st <= 6;
}

/* ---- keys: pool 0x0C0ED038, inside panel_keys_to_internal_words ---- */

static void hc_toggle_mode(void)
{
    int on;

    if (c850_ui_flag(C850_UI_FLAG_HOT_CUES_OFF))
        return;
    on = !c850_ui_hot_cues_armed();
    c850_ui_set_hot_cues_armed(on);
    hot_cues_toggles++;
    c850_ui_flash(on ? "HOT CUE ON" : "HOT CUE OFF");
}

/* Called by c850_ui_keys (the shared filter on pool 0x0C0ED038) with the
 * true held words k, the new presses p, and the keys taken so far. */
static void hot_cues_key_filter(const u16 *k, const u16 *p, u16 *own)
{
    u16 p1 = p[1];
    int i;

    if (!hot_cues_on || c850_ui_flag(C850_UI_FLAG_HOT_CUES_OFF))
        return;
    if (c850_ui_shift(k, p, own, C850_K0_RELOOP)) {
        hc_toggle_mode();
        hot_cues_owned++;
    }
    if ((p1 & HC_K1_BEATS) && hot_cues_mode() && C850_IS_USB_MODE == 1 && hc_ctx_current()) {
        own[1] |= p1 & HC_K1_BEATS;
        hot_cues_owned++;
        for (i = 0; i < 3; i++)
            if (p1 & (C850_K1_BEAT1 << i))
                hc_queue[i] = (k[1] & C850_K1_BEAT8) ? 2 : 1;
    }
}
C850_UI_KEY_FILTER(hot_cues_key_filter);

/* ---- actions (from hot_cues_tick) ---- */

/* Pools 0x0C0D41BC and 0x0C0D3CC8 (cue_select_from_list, (src, 1) and
 * (src, 2, next)) in cue_call_prev_next's USB branches (0x0CE6D254 0/1 or not):
 * for one call the hot cue in hc_entry is the selection. The fields are those
 * the stock routine copies for src 2/3 (0x0C0D27CA..), entry words E0..E8. */
int hot_cues_select(int src, int dir, int x)
{
    u32 *e = hc_entry, loop, b12;

    if (!hc_sel)
        return HC_SELECT_STOCK(src, dir, x);
    hc_sel = 0;
    loop = e[0] & 1u;
    b12 = (e[0] & 0x1000u) != 0;
    if (HC_SEL_MODE == 3) {
        C850_U16(HC_TARGET + 4) = HC_TRACK_NO;
        C850_U32(HC_TARGET + 0xC) = e[3];
        C850_U32(HC_TARGET + 0x14) = e[5];
        C850_U32(HC_TARGET + 0x1C) = e[7];
        if (!loop) {
            C850_U32(HC_TARGET + 0x10) = 0;
        } else {
            C850_U32(HC_TARGET + 0x10) = e[4];
            C850_U32(HC_TARGET + 0x18) = e[6];
            C850_U32(HC_TARGET + 0x20) = e[8];
        }
        C850_U8(HC_TARGET + 1) = (u8)loop;
        C850_U8(HC_TARGET + 2) = (u8)loop;
        C850_U8(HC_TARGET + 3) = (u8)b12;
        C850_U8(HC_TARGET + 0) = 1;
    } else {
        C850_U32(0x0CE5A9F8) = e[5];
        C850_U16(0x0CE65C04) = HC_TRACK_NO;
        C850_CUE_FRAME = e[3];
        C850_U32(0x0CE5A9FC) = e[7];
        C850_U32(0x0CE64570) = b12;
        C850_U32(0x0CE64574) = 1;
        C850_U16(HC_TARGET + 4) = HC_TRACK_NO;
        C850_U32(HC_TARGET + 0xC) = e[3];
        C850_U32(HC_TARGET + 0x14) = e[5];
        C850_U32(HC_TARGET + 0x1C) = e[7];
        if (!loop) {
            C850_U32(0x0C7F66F4) = 0;
            C850_LOOP_ACTIVE = 0;
            C850_U8(0x0CE6D0EE) = 0;
            C850_U32(HC_TARGET + 0x10) = 0;
        } else {
            C850_U32(0x0C7F66F4) = e[4];
            C850_U32(0x0C7F6758) = e[6];
            C850_U32(0x0C7F675C) = e[8];
            C850_LOOP_ACTIVE = 1;
            C850_U8(0x0CE6D0EE) = 1;
            C850_U32(HC_TARGET + 0x10) = e[4];
            C850_U32(HC_TARGET + 0x18) = e[6];
            C850_U32(HC_TARGET + 0x20) = e[8];
        }
        C850_U8(HC_TARGET + 1) = (u8)loop;
        C850_U8(HC_TARGET + 2) = (u8)loop;
        C850_U8(HC_TARGET + 3) = (u8)b12;
    }
    HC_CUE_LIST_CHANGED();
    return 0;
}

/* ---- instant jump: an in-ring seek of the IRQ7 feeder (docs 4b.11, 12b) ----
 *
 * A point cue whose audio is still in the PCM ring (played sectors kept behind
 * the play sector, or sectors read ahead of it) is reached by moving the
 * feeder's read pointer and play sector there, as loop_roll lands and as a
 * stock loop wrap seeks: no CALL, no ring restart, no prefill. The ring
 * counters follow by themselves: the ISR and the audio task work them out
 * from the play sector, the write sector 0x0C7F645E and the behind start
 * 0x0C7F6460 (FUN_0C008634 / FUN_0C00866E), and the forward path trims the
 * behind start once more than 1000 sectors lie behind. Only forward playback
 * at the stock block alignment, outside loops; otherwise the CALL path. */
#define HC_READ_PTR         C850_U32(0x0C7F6470)   /* next block the feeder plays */
#define HC_PLAY_SECTOR      C850_S16(0x0C7F645C)
#define HC_WRITE_SECTOR     C850_S16(0x0C7F645E)
#define HC_BEHIND_START     C850_S16(0x0C7F6460)
#define HC_SEG_SOURCE       C850_U32(0x0C7F677C)   /* -1 while no loop segments */
#define HC_BLOCK_BYTES      0x150u
#define HC_AHEAD_SPARE      38u                    /* sectors (0.5 s) that must follow the landing */
#define HC_BEHIND_SPARE     4u                     /* sectors kept clear of the behind start */
#define HC_CHECK_STEP       16u                    /* sector table checked every 16 sectors */

volatile u32 hot_cues_instant = 1;   /* 0 = every jump takes the CALL path */
volatile u32 hot_cues_instants;      /* jumps that landed in the ring */
volatile u32 hot_cues_instant_misses; /* point jumps while playing that had to take CALL */
volatile u32 hot_cues_instant_sector;
volatile u32 hot_cues_instant_from;  /* frame the last instant jump left */
volatile u32 hot_cues_instant_tick;  /* 1 ms tick of the last instant jump */

static u32 hc_ring(s32 v)
{
    while (v < 0)
        v += (s32)C850_PCM_RING_SLOTS;
    while (v >= (s32)C850_PCM_RING_SLOTS)
        v -= (s32)C850_PCM_RING_SLOTS;
    return (u32)v;
}

static u32 hc_irq_off(void)
{
    u32 sr, m;

    __asm__ volatile("stc sr,%0" : "=r"(sr));
    m = sr | 0xF0u;
    __asm__ volatile("ldc %0,sr" : : "r"(m) : "memory");
    return sr;
}

static void hc_irq_restore(u32 sr)
{
    __asm__ volatile("ldc %0,sr" : : "r"(sr) : "memory");
}

/* The ring sector holding frame f, the run between it and the play sector
 * checked in the sector table; 0 when that audio is not buffered. */
static int hc_ring_sector(u32 f, u32 *out)
{
    u32 cur = (u32)(u16)HC_PLAY_SECTOR, fcur, lo, flo, n, k, s;

    if (cur >= C850_PCM_RING_SLOTS)
        return 0;
    fcur = c850_ring_sector_to_frame((int)cur);
    if (f >= fcur) {
        n = f - fcur;
        if (n + HC_AHEAD_SPARE > hc_ring((s32)HC_WRITE_SECTOR - (s32)cur))
            return 0;
        s = hc_ring((s32)(cur + n));
        lo = cur;
        flo = fcur;
    } else {
        n = fcur - f;
        if (n + HC_BEHIND_SPARE > hc_ring((s32)cur - (s32)HC_BEHIND_START))
            return 0;
        s = hc_ring((s32)cur - (s32)n);
        lo = s;
        flo = f;
    }
    for (k = 0;; k += HC_CHECK_STEP) {
        if (k > n)
            k = n;
        if (c850_ring_sector_to_frame((int)hc_ring((s32)(lo + k))) != flo + k)
            return 0;
        if (k == n)
            break;
    }
    *out = s;
    return 1;
}

/* Land on frame f while playing; 1 = done. */
static int hc_jump_in_ring(u32 f)
{
    u32 s, sr, ptr;

    if (!hot_cues_instant || C850_LOOP_ACTIVE != 0 || HC_SEG_SOURCE != 0xFFFFFFFFu)
        return 0;
    ptr = HC_READ_PTR;
    if (ptr < C850_PCM_RING_BASE || (ptr - C850_PCM_RING_BASE) % HC_BLOCK_BYTES != 0)
        return 0;                     /* reverse or a ramp: not the stock forward alignment */
    if (!hc_ring_sector(f, &s))
        return 0;
    hot_cues_instant_from = C850_PLAY_FRAME;
    sr = hc_irq_off();
    HC_READ_PTR = C850_PCM_RING_BASE + s * C850_PCM_SECTOR_BYTES;
    HC_PLAY_SECTOR = (s16)s;
    hc_irq_restore(sr);
    hot_cues_instant_sector = s;
    hot_cues_instant_tick = C850_TICK_MS;
    hot_cues_instants++;
    return 1;
}

static void hc_copy_ring_to_slot(u32 slot, u32 start_sec, u32 n)
{
    u32 first = C850_PCM_RING_SLOTS - start_sec;
    u32 dst_pcm = hc_cache_pcm(slot);
    u32 dst_meta = hc_cache_meta(slot);

    if (first > n)
        first = n;
    HC_MEMCPY(dst_pcm, C850_PCM_RING_BASE + start_sec * C850_PCM_SECTOR_BYTES,
              first * C850_PCM_SECTOR_BYTES);
    HC_MEMCPY(dst_meta, HC_META_BASE + start_sec * HC_META_BYTES, first * HC_META_BYTES);
    if (n > first) {
        u32 rest = n - first;
        HC_MEMCPY(dst_pcm + first * C850_PCM_SECTOR_BYTES, C850_PCM_RING_BASE,
                  rest * C850_PCM_SECTOR_BYTES);
        HC_MEMCPY(dst_meta + first * HC_META_BYTES, HC_META_BASE, rest * HC_META_BYTES);
    }
}

static int hc_ring_find_span(u32 f, u32 want, u32 *out_slot)
{
    u32 cur = (u32)(u16)HC_PLAY_SECTOR;
    u32 write_s = (u32)(u16)HC_WRITE_SECTOR;
    u32 behind_s = (u32)(u16)HC_BEHIND_START;
    u32 ahead, behind, fcur, start_s, k;

    if (cur >= C850_PCM_RING_SLOTS || write_s >= C850_PCM_RING_SLOTS ||
        behind_s >= C850_PCM_RING_SLOTS || want == 0u || want > HC_CACHE_SECTORS)
        return 0;
    ahead = hc_ring((s32)write_s - (s32)cur);
    behind = hc_ring((s32)cur - (s32)behind_s);
    if (ahead + behind < want)
        return 0;
    fcur = c850_ring_sector_to_frame((int)cur);
    if (fcur == 0xFFFFFFFFu)
        return 0;
    if (f >= fcur) {
        u32 d = f - fcur;
        if (d + want > ahead)
            return 0;
        start_s = hc_ring((s32)(cur + d));
    } else {
        u32 d = fcur - f;
        if (d > behind || d + ahead < want)
            return 0;
        start_s = hc_ring((s32)cur - (s32)d);
    }
    for (k = 0;; k += HC_CHECK_STEP) {
        if (k >= want)
            k = want - 1u;
        if (c850_ring_sector_to_frame((int)hc_ring((s32)(start_s + k))) != f + k)
            return 0;
        if (k == want - 1u)
            break;
    }
    *out_slot = start_s;
    return 1;
}

__attribute__((noinline))
static void hc_cache_poll_capture(void)
{
    u32 i, track_len, st = C850_PLAYER_STATE;

    if (!hot_cues_precache || !hot_cues_on || !hot_cues_avail || C850_IS_USB_MODE != 1)
        return;
    if (!hc_cache_budget_ok())
        return;
    track_len = C850_TRACK_LENGTH;
    if (track_len == 0u || C850_AUDIO_STATE == 0u)
        return;
    if (st != C850_STATE_PLAYING && st != C850_STATE_LOOPING &&
        st != C850_STATE_PAUSED_CDJ && st != C850_STATE_CUED)
        return;
    for (i = 0; i < HC_CACHE_SLOTS; i++) {
        hc_cache_slot_t *s = &hc_cache[i];
        u32 want, start_s;

        if (s->state != HC_CACHE_PENDING || s->h1 != hc_h1 || s->h2 != hc_h2)
            continue;
        if (s->cue_frame >= track_len)
            continue;
        want = track_len - s->cue_frame;
        if (want > HC_CACHE_SECTORS)
            want = HC_CACHE_SECTORS;
        if (want <= 25u && s->cue_frame + want < track_len)
            continue;
        if (hc_ring_find_span(s->cue_frame, want, &start_s)) {
            hc_copy_ring_to_slot(i, start_s, want);
            s->sectors = (u16)want;
            s->state = HC_CACHE_READY;
            hot_cues_cache_captures++;
            hc_cache_update_mask();
        }
    }
}

/* ---- short prefill & instant cache restore: pools 0x0C0F54D8 & 0x0C0F73A8 ----
 *
 * A USB CALL restarts the ring; the audio task then asks the reader for 400
 * sectors (5.33 s) from slot 120 (0x0C0F533A..0x0C0F534E) and takes PLAY only
 * once they are in. The pool word that call reads (once, at 0x0C0F534C) lies in
 * the boot-critical closure; it is redirected through the service-mode gate
 * (GATED_BOOT_POOLS), so in an update mode the stock request runs untouched.
 *
 * On cache hit (slot READY in SDRAM matching track & cue frame): copy N sectors
 * into slots 120..120+N-1, advance pcm_write_sector/behind_start, schedule the
 * background continuation read from cue_frame+N into slot 120+N, and jump the
 * audio task substate to case 0x73 (instant snapshot ready). On cache miss for
 * a hot-cue CALL, shorten count to HC_PREFILL_MIN (180). Runs on AudioTask.
 */
#define HC_RING_REQUEST     C850_FN(0x0C008C8A, void, (int kind, int p2, u32 max, u32 pos, u32 count, \
                                                     u32 start, int p7))
#define HC_PREFILL_STOCK    400u
#define HC_PREFILL_SLOT     120u
#define HC_PREFILL_ARM_MS   2000u
/* The audio task arms playback (play sector = slot 120, then the 120 sectors
 * before the cue as a kind-5 request, then ready) only once about 150 sectors
 * of the prefill are in: in the emulator 160 sectors became ready and 140 left
 * the deck in state 6 for good. Counts below HC_PREFILL_MIN are raised to it. */
#define HC_PREFILL_MIN      180u

volatile u32 hot_cues_prefill = HC_PREFILL_MIN;  /* sectors (2.4 s) for a hot-cue CALL; 0 or >= 400 = stock */
volatile u32 hot_cues_prefills;      /* prefill requests shortened */
volatile u32 hot_cues_prefill_last;  /* count of the last request through the word */
volatile u32 hot_cues_prefill_tick;  /* 1 ms tick of the last shortened request */
static volatile u8 hc_prefill_armed;
static volatile u32 hc_prefill_since;

static void hc_prefill_arm(void)
{
    hc_prefill_since = C850_TICK_MS;
    hc_prefill_armed = 1;
}

static void hc_clear_ring_sentinels(u32 from_sec, u32 count)
{
    u32 k;

    for (k = 0; k < count && from_sec + k < C850_PCM_RING_SLOTS; k++)
        C850_U32(HC_META_BASE + (from_sec + k) * HC_META_BYTES) = 0xFFFFFFFFu;
}

static int hc_try_cache_restore(int p2, u32 max, u32 pos, int p7)
{
    u32 slot = hc_restore_slot, n, cont, track_len, req_cnt;
    const hc_cache_slot_t *s;

    hc_restore_slot = 0;
    if (slot < 1u || slot > HC_CACHE_SLOTS)
        return 0;
    if (!hot_cues_on || !hot_cues_precache || !hc_cache_budget_ok())
        return 0;
    if (C850_TICK_MS - hc_restore_since > HC_PREFILL_ARM_MS)
        return 0;
    if (C850_U32(0x0C7F66F0u) != hc_restore_frame)
        return 0;
    s = &hc_cache[slot - 1u];
    if (!hc_cache_match(slot - 1u, hc_restore_frame))
        return 0;
    n = s->sectors;
    track_len = C850_TRACK_LENGTH;
    cont = s->cue_frame + n;
    if (n < HC_CACHE_MIN_SECTORS && (track_len == 0u || cont < track_len))
        return 0;

    HC_MEMCPY(C850_PCM_RING_BASE + HC_PREFILL_SLOT * C850_PCM_SECTOR_BYTES,
              hc_cache_pcm(slot - 1u), n * C850_PCM_SECTOR_BYTES);
    HC_MEMCPY(HC_META_BASE + HC_PREFILL_SLOT * HC_META_BYTES,
              hc_cache_meta(slot - 1u), n * HC_META_BYTES);
    hc_clear_ring_sentinels(0u, HC_PREFILL_SLOT);
    hc_clear_ring_sentinels(HC_PREFILL_SLOT + n, 32u);
    if (track_len != 0u && cont >= track_len && HC_PREFILL_SLOT + n < C850_PCM_RING_SLOTS)
        C850_U32(HC_META_BASE + (HC_PREFILL_SLOT + n) * HC_META_BYTES + 8u) = 0x00AA0000u;

    HC_WRITE_SECTOR = (s16)(HC_PREFILL_SLOT + n);
    HC_BEHIND_START = (s16)HC_PREFILL_SLOT;
    C850_U32(0x0C7F6744u) = cont - 1u;

    if (track_len != 0u && cont < track_len) {
        req_cnt = C850_PCM_RING_SLOTS - (HC_PREFILL_SLOT + n);
        if (req_cnt > HC_PREFILL_STOCK)
            req_cnt = HC_PREFILL_STOCK;
        HC_FRAMES_TO_MSF(cont, pos);
        HC_RING_REQUEST(4, p2, max, pos, req_cnt, HC_PREFILL_SLOT + n, p7);
        hc_stream_inflight = 1;
        hc_stream_since = C850_TICK_MS;
    } else {
        req_cnt = 0;
        hc_stream_inflight = 0;
        C850_U8(0x0C7F663Du) = 0u;
        C850_U8(0x0C7F65FEu) = 0u;
    }
    C850_U8(0x0C7F6625u) = 0x72u;     /* caller at 0x0C0F544E steps to case 0x73 */
    hot_cues_prefill_last = req_cnt;
    hot_cues_cache_last_sectors = n;
    hot_cues_cache_hits++;
    return 1;
}

void hot_cues_ring_request(int kind, int p2, u32 max, u32 pos, u32 count, u32 start, int p7)
{
    u32 n = hot_cues_prefill;

    if (kind == 4 && count == HC_PREFILL_STOCK && start == HC_PREFILL_SLOT &&
        hc_try_cache_restore(p2, max, pos, p7)) {
        hc_prefill_armed = 0;
        return;
    }
    hc_stream_inflight = 0;
    if (hc_prefill_armed && kind == 4 && count == HC_PREFILL_STOCK && start == HC_PREFILL_SLOT) {
        hc_prefill_armed = 0;
        if (hot_cues_on && n != 0 && n < HC_PREFILL_STOCK) {
            count = n < HC_PREFILL_MIN ? HC_PREFILL_MIN : n;
            hot_cues_prefills++;
            hot_cues_prefill_tick = C850_TICK_MS;
        }
    }
    hot_cues_prefill_last = count;
    HC_RING_REQUEST(kind, p2, max, pos, count, start, p7);
}

/* Pool 0x0C0F73A8 inside player_tick_usb_playback (case 0x10): skip sending a
 * duplicate read-ahead request while the case-0x77 continuation read is still
 * running in mp3ActTask; caller steps into case 0x11 and waits for done. */
void hot_cues_play_ring_request(int kind, int p2, u32 max, u32 pos, u32 count, u32 start, int p7)
{
    if (hc_stream_inflight) {
        if (C850_U8(0x0C7F663Du) != 0u) {
            C850_U8(0x0C7F663Du) = 0u;
            hc_stream_inflight = 0;
        } else if (C850_TICK_MS - hc_stream_since > 5000u) {
            hc_stream_inflight = 0;
        } else {
            return;
        }
    }
    HC_RING_REQUEST(kind, p2, max, pos, count, start, p7);
}

/* ---- Background pre-caching on AudioTask + mp3ActTask ----
 *
 * AudioTask (pools 0x0C014E8C for audio state 3, 0x0C014E90 for 4/5)
 * schedules one PENDING hot-cue slot whenever playback is steady in USB mode
 * with safe ahead runway (>= 600 sectors playing, >= 500 paused/cued) and
 * the reader is idle (0x0C7F65FE, 0x0C7F663D clear). It fills the reader's
 * job struct 0x0C7F6538 as FUN_0C008C8A does (kind 4, p2 = 0xFE marks it,
 * MSF of the cue, count, start = the write sector), keeping +0x14, the track
 * slot the last stock request left (FUN_0C0BEEC0 copies it to 0x0CE5A9E4 and
 * 0x0C0BECAA refuses a job whose slot does not fit 0x0CE5A9E0), and sets
 * 0x0C7F670E so the next stock read-ahead re-seeks (kind 6) to the ring's
 * write frame. mp3ActTask runs the stock kind-4 seek, then hot_cues_pcm_init
 * switches the decode pass to kind 10 / mode 2 so FUN_0C0C0346 leaves
 * 0x0C750C3C and 0x0C7F645E alone while hot_cues_pcm_write copies PCM +
 * metadata into the slot at HC_CACHE_BASE. A jump, a seek, reverse, a new
 * track or runway < 300 sectors cancels the job (0x0CE5A9EC = 2).
 */
#define HC_PLAYER_PLAY_TICK C850_FN(0x0C0F65F0u, u32, (u32 deck))
#define HC_PLAYER_USB_TICK  C850_FN(0x0C0F6E6Cu, u32, (u32 deck))
#define HC_BEBA0            C850_FN(0x0C0BEBA0u, u32, (u32 msg))
#define HC_PCM_INIT         C850_FN(0x0C0EFB8Eu, void, (u16 sector))
#define HC_PCM_WRITE        C850_FN(0x0C0EFB9Au, void, (u32 src, u32 n_longwords))
#define HC_VBR_FIND         C850_FN(0x0C08F51Au, u32, (int track_idx, int p2, u32 frame, \
                                                       u32 *out_a, u32 *out_pair, u32 *out_b, int p7))

__attribute__((noinline))
static void hc_bg_step(u16 deck)
{
    u8 usb_st = C850_U8(0x0C7F6626u), usb_sub = C850_U8(0x0C7F6625u);
    u8 ast = C850_AUDIO_STATE;
    u32 pst = C850_PLAYER_STATE;
    u16 ahead = C850_U16(0x0C7F6686u);
    int steady = (ast == 3u && usb_st == 0u && usb_sub == 0x11u) ||
                 ((ast == 4u || ast == 5u) &&
                  ((usb_st == 0u && usb_sub == 0x10u) || (usb_st == 1u && usb_sub == 0x20u)));
    u32 i, track_len, pos;

    if (hc_bg_inflight) {
        int abort = 0;
        if (!hot_cues_on || !hot_cues_precache || !hot_cues_avail || C850_IS_USB_MODE != 1)
            abort = 1;
        else if (hc_restore_slot || hc_stream_inflight || hc_prefill_armed || hc_play)
            abort = 1;
        else if (hc_bg_h1 != hc_h1 || hc_bg_h2 != hc_h2)
            abort = 1;
        else if (!steady)
            abort = 1;
        else if (ast == 3u && ahead < 300u)
            abort = 1;
        else if (C850_U8(0x0C7F666Cu) != 0u)
            abort = 1;
        else if (C850_TICK_MS - hc_bg_since > 5000u)
            abort = 1;
        if (abort)
            hc_bg_abort();
        return;
    }
    if (!hot_cues_on || !hot_cues_precache || !hot_cues_avail || C850_IS_USB_MODE != 1)
        return;
    if (!hc_cache_budget_ok())
        return;
    track_len = C850_TRACK_LENGTH;
    if (track_len == 0u)
        return;
    if (hc_restore_slot || hc_stream_inflight || hc_prefill_armed || hc_play)
        return;
    if (!steady)
        return;
    if (pst != C850_STATE_PLAYING && pst != C850_STATE_LOOPING &&
        pst != C850_STATE_PAUSED_CDJ && pst != C850_STATE_CUED)
        return;
    if (C850_U8(0x0C7F666Cu) != 0u)
        return;
    if (C850_U8(0x0C7F65FEu) != 0u || C850_U8(0x0C7F663Du) != 0u ||
        C850_U8(0x0CE5A9ECu) == 5u)
        return;
    if (ast == 3u ? (ahead < 600u) : (ahead < 500u))
        return;
    if (C850_U16(0x0C7F654Cu) == 0u)  /* no stock request has named the track yet */
        return;

    for (i = 0; i < HC_CACHE_SLOTS; i++) {
        hc_cache_slot_t *s = &hc_cache[i];
        u32 want, start_s;

        if (s->state != HC_CACHE_PENDING || s->h1 != hc_h1 || s->h2 != hc_h2)
            continue;
        if (s->cue_frame >= track_len)
            continue;
        want = track_len - s->cue_frame;
        if (want > HC_CACHE_SECTORS)
            want = HC_CACHE_SECTORS;
        if (want < HC_CACHE_MIN_SECTORS && s->cue_frame + want < track_len)
            continue;
        if (hc_ring_find_span(s->cue_frame, want, &start_s)) {
            hc_copy_ring_to_slot(i, start_s, want);
            s->sectors = (u16)want;
            s->state = HC_CACHE_READY;
            hot_cues_cache_captures++;
            hc_cache_update_mask();
            return;
        }
        hc_bg_slot = (u8)(i + 1u);
        hc_bg_frame = s->cue_frame;
        hc_bg_h1 = s->h1;
        hc_bg_h2 = s->h2;
        hc_bg_want = (u16)want;
        hc_bg_sectors = 0u;
        hc_bg_longwords = 0u;
        hc_bg_cancel = 0u;
        hc_bg_since = C850_TICK_MS;
        hc_bg_inflight = 1u;

        pos = s->cue_frame;
        C850_U8(0x0C7F6549u) = 4u;
        C850_U8(0x0C7F654Au) = 0xFEu; /* background pre-cache marker */
        /* +0x14 is track_length_slot(track number), the loaded track's slot
         * (FUN_0C0BEEC0 copies it to 0x0CE5A9E4; 0x0C0BECAA refuses the job when
         * it does not fit 0x0CE5A9E0): keep the one the last stock request for
         * this track left in the job struct. */
        C850_U16(0x0C7F654Eu) = (u16)(pos / 4500u);
        C850_U16(0x0C7F6550u) = (u16)((pos / 75u) % 60u);
        C850_U16(0x0C7F6552u) = (u16)(pos % 75u);
        C850_U16(0x0C7F6554u) = (u16)want;
        C850_U16(0x0C7F6556u) = (u16)HC_WRITE_SECTOR;
        C850_U8(0x0C7F6558u) = (u8)deck;
        C850_U8(0x0C7F670Eu) = 1u;
        C850_U8(0x0C7F65FEu) = 1u;
        return;
    }
}

u32 hot_cues_play_tick(u32 deck)
{
    u32 r = HC_PLAYER_PLAY_TICK(deck);

    hc_bg_step((u16)deck);
    return r;
}

u32 hot_cues_usb_tick(u32 deck)
{
    u32 r = HC_PLAYER_USB_TICK(deck);

    hc_bg_step((u16)deck);
    return r;
}

u32 hot_cues_beba0(u32 msg)
{
    u32 r = 0u;

    if (C850_U8(msg + 0x11u) == 4u && C850_U8(msg + 0x12u) == 0xFEu && hc_bg_inflight) {
        u32 slot = (u32)hc_bg_slot - 1u;
        if (!hc_bg_cancel && slot < HC_CACHE_SLOTS) {
            hc_bg_divert = 1u;
            r = HC_BEBA0(msg);
            hc_bg_divert = 0u;
            if (!hc_bg_cancel && C850_U8(0x0CE5A9ECu) != 2u && hc_bg_sectors > 0u) {
                hc_cache_slot_t *s = &hc_cache[slot];
                u32 track_len = C850_TRACK_LENGTH;
                if (s->state == HC_CACHE_PENDING && s->h1 == hc_bg_h1 &&
                    s->h2 == hc_bg_h2 && s->cue_frame == hc_bg_frame &&
                    (hc_bg_sectors >= HC_CACHE_MIN_SECTORS ||
                     (track_len != 0u && hc_bg_frame + hc_bg_sectors >= track_len))) {
                    s->sectors = hc_bg_sectors;
                    s->state = HC_CACHE_READY;
                    hot_cues_cache_captures++;
                    hot_cues_precaches++;
                    hc_cache_update_mask();
                }
            } else if (hc_bg_cancel || C850_U8(0x0CE5A9ECu) == 2u) {
                hot_cues_precache_aborts++;
            }
        } else {
            hot_cues_precache_aborts++;
        }
        hc_bg_divert = 0u;
        hc_bg_cancel = 0u;
        hc_bg_inflight = 0u;
        return r;
    }
    hc_bg_divert = 0u;
    return HC_BEBA0(msg);
}

void hot_cues_pcm_init(u16 sector)
{
    if (hc_bg_divert) {
        C850_U8(0x0CE5AC70u) = 10u;
        C850_U8(0x0CE5AC71u) = 2u;
        C850_U16(0x0CE5AC76u) = 3000u;
        C850_U16(0x0CE5AC44u) = 0u;
        sector = 0u;
    }
    HC_PCM_INIT(sector);
}

__attribute__((noinline))
static void hc_bg_write_meta(u32 slot, u32 sec)
{
    u32 dst = hc_cache_meta(slot) + sec * HC_META_BYTES;
    u32 frame = hc_bg_frame + sec;
    u16 track_idx = C850_U16(0x0CE5AC60u);
    u32 tinfo = 0x0CE59B50u + (u32)track_idx * 0x3A4u;

    C850_U32(dst + 0u) = frame;
    C850_U32(dst + 8u) = (u32)C850_U16(0x0CE5A9E4u);
    if (C850_U8(tinfo + 0x3A1u) & 0x80u) {
        u32 a = 0u, b = 0u, pair[2] = {0u, 0u};
        C850_U8(0x0CE6451Cu) = 1u;
        if (HC_VBR_FIND((int)(s16)track_idx, 0, frame, &a, pair, &b, 0x0Eu) == 0u) {
            C850_U32(dst + 4u) = pair[1];
            C850_U32(dst + 12u) = pair[0] & 0x7FFFFFFFu;
        } else {
            C850_U32(dst + 4u) = 0u;
            C850_U32(dst + 12u) = 0xFFFFFFFFu;
        }
        C850_U8(0x0CE6451Cu) = 0u;
    } else {
        C850_U32(dst + 4u) = 0u;
        C850_U32(dst + 12u) = 0xFFFFFFFFu;
    }
}

void hot_cues_pcm_write(u32 src, u32 n_longwords)
{
    u32 slot, max_lw, avail_lw, cp, done_sec;

    if (!hc_bg_divert) {
        HC_PCM_WRITE(src, n_longwords);
        return;
    }
    slot = (u32)hc_bg_slot - 1u;
    if (slot >= HC_CACHE_SLOTS || hc_bg_cancel)
        return;
    max_lw = (u32)hc_bg_want * HC_SECTOR_LONGWORDS;
    if (hc_bg_longwords < max_lw) {
        avail_lw = max_lw - hc_bg_longwords;
        cp = n_longwords < avail_lw ? n_longwords : avail_lw;
        if (cp > 0u) {
            HC_MEMCPY(hc_cache_pcm(slot) + hc_bg_longwords * 4u, src, cp * 4u);
            hc_bg_longwords += cp;
        }
    }
    done_sec = hc_bg_longwords / HC_SECTOR_LONGWORDS;
    if (done_sec > (u32)hc_bg_want)
        done_sec = (u32)hc_bg_want;
    while ((u32)hc_bg_sectors < done_sec) {
        hc_bg_write_meta(slot, (u32)hc_bg_sectors);
        hc_bg_sectors++;
    }
}

static void hc_jump(u32 n, u32 rec, u32 st)
{
    int in_cache;

    if (HC_REC_TO_ENTRY(rec, hc_entry) != 1) {
        hot_cues_failed++;
        return;
    }
    if (!(hc_entry[0] & 1u) &&
        (st == C850_STATE_PLAYING || st == C850_STATE_PAUSED_CDJ || st == C850_STATE_CUED) &&
        C850_PLAYER_STATE == st) {
        if (hc_jump_in_ring(hc_entry[3])) {
            hot_cues_jumps++;
            hot_cues_last_slot = n;
            hot_cues_last_in = hc_entry[3];
            hot_cues_last_out = 0;
            if (st != C850_STATE_PLAYING) {
                C850_PLAY_FRAME = hc_entry[3];
                hc_play = HC_PLAY_TRIES;
                hc_play_wait = 1;
            }
            return;
        }
        in_cache = hot_cues_precache && hc_cache_match(n - 1u, hc_entry[3]);
        if (st == C850_STATE_PLAYING && !in_cache)
            hot_cues_instant_misses++;
    } else {
        in_cache = hot_cues_precache && hc_cache_match(n - 1u, hc_entry[3]);
    }
    if (in_cache) {
        hc_restore_slot = (u8)n;
        hc_restore_frame = hc_entry[3];
        hc_restore_since = C850_TICK_MS;
    } else {
        hc_restore_slot = 0;
    }
    hc_bg_abort();
    hc_prefill_arm();
    hc_sel = 1;
    c850_cue_call_prev_next(1);
    if (hc_sel) {                     /* the CALL path's gates said no */
        hc_sel = 0;
        hc_restore_slot = 0;
        hot_cues_failed++;
        return;
    }
    hot_cues_jumps++;
    hot_cues_last_slot = n;
    hot_cues_last_in = hc_entry[3];
    hot_cues_last_out = (hc_entry[0] & 1u) ? hc_entry[4] : 0;
    /* Wait in hot_cues_tick until c850_cue_call_prev_next's seek settles, then auto-play. */
    hc_play = HC_PLAY_TRIES;
    hc_play_wait = 1;
}

/* Write hot cue n (rec) through the stock ANLZ hot-cue setter. */
static int hc_write(u32 key, u32 rec)
{
    int r;

    hc_fixlen = 1;
    r = HC_ANLZ_SET(C850_ANLZ_CTX, key + 4, rec, 1);
    hc_fixlen = 0;
    hot_cues_last_result = r;
    return r;
}

/* Quantize for a new point cue: the nearest beat, exactly as LOOP IN snaps
 * with mods/quant_loops (its strong quant_loops_in_point: quantize on, a USB
 * track whose grid gave the deck's tempo, the beat in the grid window in RAM;
 * otherwise the position itself). Built without quant_loops, this weak default
 * keeps the play position, as stock MEMORY stores a memory cue. */
u32 quant_loops_in_point(u32 pos) __attribute__((weak, noinline));
u32 quant_loops_in_point(u32 pos)
{
    return pos;
}

/* Frame a new point cue at play position pos is recorded at. */
u32 hot_cues_point(u32 pos)
{
    u32 q = quant_loops_in_point(pos);

    hot_cues_last_pos = pos;
    if (q != pos)
        hot_cues_snapped++;
    return q;
}

/* The cue list entry for a new hot cue n: a point at the play position (the
 * nearest beat with quantize), or the playing loop as it plays (a loop
 * quant_loops made is on the grid already). Fields as memory_cue_add's USB
 * branch builds them. */
static void hc_new_entry(u32 n, u32 st, u32 *e)
{
    u32 in = C850_PLAY_FRAME, out = 0, loop = 0, k;

    if (st == C850_STATE_LOOPING && C850_LOOP_ACTIVE == 1) {
        in = C850_LOOP_IN_FRAME;
        out = hot_cues_effective_out(in);
        loop = out > in;
        if (!loop)
            out = 0;
    }
    if (!loop)
        in = hot_cues_point(in);
    for (k = 0; k < 9; k++)
        e[k] = 0;
    e[0] = (n << 16) | loop;
    e[1] = HC_TRACK_NO;
    e[2] = loop ? C850_U16(0x0C7F64B6) : 0;
    e[3] = in;
    e[4] = out;
    e[5] = in;
    e[6] = out;
    e[7] = 0;                         /* seek hints: 0 as rekordbox writes them */
    e[8] = loop ? C850_U32(0x0C7F675C) : 0;
}

static void hc_record(u32 n, u32 key, u32 st)
{
    u32 e[9];

    hc_new_entry(n, st, e);
    if (HC_ENTRY_TO_REC(e, (u32)hc_rec) != 1 || hc_write(key, (u32)hc_rec) != 0) {
        hot_cues_failed++;
        return;
    }
    hot_cues_records++;
    hot_cues_last_slot = n;
    hot_cues_last_in = e[3];
    hot_cues_last_out = e[4];
    if (hot_cues_precache)
        hc_cache_set_slot(n - 1u, e[3], (e[0] & 1u) ? e[4] : 0u, (e[0] & 1u) ? 1u : 0u);
}

static void hc_delete(u32 n, u32 key, u32 rec)
{
    u32 k;

    for (k = 0; k < 8; k++)
        hc_rec[k] = C850_U32(rec + 4 * k);
    hc_del = (u8)n;
    if (hc_write(key, (u32)hc_rec) != 0)
        hot_cues_failed++;
    else {
        hot_cues_deletes++;
        hc_cache_clear_slot(n - 1u);
    }
    hc_del = 0;
    hot_cues_last_slot = n;
}

__attribute__((noinline))
static void hc_action(u32 n, u32 kind)
{
    u32 key = hc_key(), st = C850_PLAYER_STATE, rec;

    if (!hc_state_ok(st) || C850_IS_USB_MODE != 1 || !hc_read(key)) {
        hot_cues_failed++;
        return;
    }
    rec = hc_find(n);
    if (kind == 2) {
        if (rec)
            hc_delete(n, key, rec);
    } else if (rec) {
        hc_jump(n, rec, st);
    } else {
        hc_record(n, key, st);
    }
    hot_cues_mask = hot_cues_slot_mask(C850_ANLZ_CTX);
}

/* Availability: looked up again whenever the loaded track (number, path
 * hash) changes. */
static u32 hc_seen, hc_seen_track;

__attribute__((noinline))
static void hc_refresh(void)
{
    u32 key = hc_key(), h;

    if (!key || C850_IS_USB_MODE != 1) {
        if (hc_had_key)
            hc_cache_invalidate_all();
        hc_had_key = 0;
        hot_cues_avail = 0;
        return;
    }
    h = HC_HASH1(key + 4);
    if (!hc_had_key || h != hc_seen || (u32)HC_LOADED_TRACK != hc_seen_track) {
        hc_cache_invalidate_all();
        hc_had_key = 1;
        hc_seen = h;
        hc_seen_track = (u32)HC_LOADED_TRACK;
        hot_cues_avail = hc_read(key);
        if (hot_cues_avail) {
            hot_cues_mask = hot_cues_slot_mask(C850_ANLZ_CTX);
            if (hot_cues_precache)
                hc_cache_sync_from_buf();
        }
    }
}

/* Pool 0x0C0EC838 (FUN_0C0EB388): first call of the state tick FUN_0C0EC73C,
 * before UI lock and every key handler of this player cycle. */
void hot_cues_tick(void)
{
    int i;

    hc_refresh();
    hc_track_out();
    hc_cache_poll_capture();
    if (hc_prefill_armed && C850_TICK_MS - hc_prefill_since > HC_PREFILL_ARM_MS)
        hc_prefill_armed = 0;         /* the CALL made no prefill request */
    if (hc_restore_slot && C850_TICK_MS - hc_restore_since > HC_PREFILL_ARM_MS)
        hc_restore_slot = 0;
    /* After a jump that was playing: PLAY presses for the state-6 handler,
     * as after a stock CALL, until the deck plays (the handler ignores a
     * press while the cue is still being prepared). Press one tick, release
     * the next, wait, again; at most HC_PLAY_TRIES presses. */
    if (hc_play_down) {
        hc_play_down = 0;
        if (!(c850_ui_kt[0] & C850_K0_PLAY))
            C850_K0 &= ~C850_K0_PLAY;
        hc_play_wait = HC_PLAY_GAP;
    } else if (hc_play) {
        u32 st = C850_PLAYER_STATE;
        if (st == C850_STATE_PLAYING || st == C850_STATE_LOOPING) {
            hc_play = 0;              /* playing */
        } else if ((st == C850_STATE_CUED || st == C850_STATE_PAUSED_CDJ) &&
                   !(c850_ui_kt[0] & C850_K0_PLAY) &&
                   (hc_play_wait == 0 || --hc_play_wait == 0)) {
            C850_K0 |= C850_K0_PLAY;
            hc_play_down = 1;
            hc_play--;
            hot_cues_plays++;
        }
    }
    for (i = 0; i < 3; i++) {
        if (hc_queue[i]) {
            u32 kind = hc_queue[i];
            hc_queue[i] = 0;
            hc_action((u32)i + 1u, kind);
            break;                    /* one action per cycle */
        }
    }
}
C850_UI_TICK_HOOK(hot_cues_tick);

/* ---- ANLZ write: pool 0x0C0C195C, the section write of FUN_0C0C166C ---- */

/* The stock hot-cue setter fills slot n and counts the set slots into the
 * PCOB header before this write. For a delete the slot is emptied here; for
 * every hot_cues write the count and the tag length also include the further
 * PCPT entries the writer appends (ctx+0xF74), so the tag stays consistent. */
int hot_cues_anlz_write(u32 file, u32 ctx, int section, u32 buf)
{
    if (section == 3 && (hc_fixlen || hc_del)) {
        u32 n, cnt = 0, k;

        if (hc_del >= 1 && hc_del <= 3) {
            u32 slot = ctx + HC_CTX_SLOT(hc_del);
            for (k = 0; k < HC_PCPT; k += 4)
                C850_U32(slot + k) = 0;
        }
        for (n = 1; n <= 3; n++)
            if (C850_U16(ctx + HC_CTX_SLOT(n) + HC_SLOT_NUM) != 0)
                cnt++;
        cnt += C850_U32(ctx + HC_CTX_EXTRAS);
        C850_U16(ctx + HC_CTX_NCUES) = (u16)cnt;
        C850_U32(ctx + HC_CTX_TAGLEN) = cnt * HC_PCPT + 0x18;
    }
    return HC_ANLZ_WRITE(file, ctx, section, buf);
}

/* ---- LEDs: pool 0x0C0ED244, the wai_flg of the playertask loop ---- */

/*
 * The beat-loop LEDs are written by display_stage_navigation_leds (DisplayTask,
 * boot-critical, not patched) from D154 (fraction mode: low nibble 0 instead
 * of 0xF), 0x0C1E1088 (bits ORed in, RELOOP LED on, blinking) and the blink
 * timers. playertask (priority 2) is the only task that reads or writes D154
 * and 0x0C1E1088 apart from that stage (DisplayTask, priority 3), so the two
 * bytes hold the LED pattern while playertask waits here and their real
 * values while it runs. Pattern: D154 = 0 (all four lit), 0x0C1E1088 = the
 * empty slots (blinking).
 */
static u8 hc_fk, hc_fk_beats, hc_sv_beats, hc_sv_d154;

/* twai_flg(flgid, waiptn, wfmode, p_flgptn, tmout): the timeout is the stack argument
 * the loop pushes (0x0C0ED1A6) and pops (0x0C0ED1BC). */
int hot_cues_wait(int flg, u32 ptn, int mode, u32 *out, int tmo)
{
    int r;
    u32 ctx;

    if (hot_cues_on && hot_cues_mode() && C850_IS_USB_MODE == 1 && (ctx = hc_ctx_current()) != 0) {
        u32 m = hot_cues_slot_mask(ctx);
        hot_cues_mask = m;
        hc_sv_beats = C850_AUTO_LOOP_BEATS;
        hc_sv_d154 = C850_BEAT_FRACTION_MODE;
        hc_fk_beats = (u8)((~m & 7u) | 8u);
        C850_AUTO_LOOP_BEATS = hc_fk_beats;
        C850_BEAT_FRACTION_MODE = 0;
        hc_fk = 1;
        hot_cues_leds++;
    }
    r = HC_WAI_FLG(flg, ptn, mode, out, tmo);
    if (hc_fk) {
        hc_fk = 0;
        /* keep a value another task wrote meanwhile (source change) */
        if (C850_AUTO_LOOP_BEATS == hc_fk_beats)
            C850_AUTO_LOOP_BEATS = hc_sv_beats;
        if (C850_BEAT_FRACTION_MODE == 0)
            C850_BEAT_FRACTION_MODE = hc_sv_d154;
    }
    return r;
}
