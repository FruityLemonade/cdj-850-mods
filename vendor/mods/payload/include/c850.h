/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c850.h -- MAIN 1.13 addresses for CDJ-850 payload code (mods/payloadbuild.py).
 *
 * Every address here was traced on CDJ-850 MAIN 1.13 and is documented in
 * docs/cdj850-modding.md: section 4 (player, BPM, auto standby, audio ring)
 * and section 4b (keys, loops, cues, grid, LEDs, settings, matrix text).
 * Tags in the comments: [emu] seen in the emulator, [static] Ghidra only, as
 * in the docs. Nothing here is valid for another firmware version.
 *
 * Calling MAIN: MAIN was built with the Renesas SH compiler. The payload is
 * built with `-mrenesas`, so it follows the same ABI: int/pointer args in
 * r4..r7 then on the stack, result in r0, r8..r14, MACH and MACL preserved
 * by the callee, PR saved by the caller. Pass only ints and pointers; no
 * floats (the payload is built with -m2a-nofpu and must never touch the FPU),
 * no struct arguments or results, no varargs.
 *
 * Context: hook routines run on the stack of the MAIN task that reached the
 * hook (DjControlTask for the player paths). Keep stack frames small and do
 * not block (no RTOS waits) in a hook.
 */
#ifndef C850_H
#define C850_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;

#define C850_U8(a)  (*(volatile u8 *)(a))
#define C850_S8(a)  (*(volatile s8 *)(a))
#define C850_U16(a) (*(volatile u16 *)(a))
#define C850_S16(a) (*(volatile s16 *)(a))
#define C850_U32(a) (*(volatile u32 *)(a))

/* A MAIN function at a fixed address: C850_FN(0x0C0E570A, int, (int beats))(8). */
#define C850_FN(addr, ret, args) ((ret (*) args)(addr))

/* The hook contract (sigpatch.trampoline): regs[0..7] = r0..r7 as the hooked
 * span left them (writes go back to MAIN), sp = r15 as the span left it. */
typedef void c850_hook_fn(u32 *regs, u32 *sp);

/* ---- Player, transport, pitch (section 4) ---- */
#define C850_IS_USB_MODE        C850_U8(0x0CE6D25A)  /* 1 = USB/data mode, 0 = CD */
#define C850_SOURCE_INDEX       C850_U8(0x0CE6D252)  /* active source (2 = USB) */
#define C850_USB_MOUNTED        C850_U8(0x0CE6D147)  /* 1 while a USB stick is mounted [emu] */
#define C850_PLAYER_STATE_BASE  0x0CE6D25D           /* + source index; section 4b states */
#define C850_PLAYER_STATE       C850_U8(C850_PLAYER_STATE_BASE + C850_SOURCE_INDEX)
#define C850_STATE_PLAYING      3
#define C850_STATE_LOOPING      4                    /* USB: a loop plays [emu] */
#define C850_STATE_PAUSED_CDJ   5
#define C850_STATE_CUED         6
#define C850_TEMPO_RANGE        C850_U8(0x0CE6D0E1)  /* 1 6%, 2 10%, 3 16%, 4 WIDE */
#define C850_MASTER_TEMPO       C850_U8(0x0CE6D0E0)
#define C850_JOG_MODE_VINYL     C850_U8(0x0CE6D0DE)
#define C850_DSP_TARGET_SPEED   C850_U32(0x0CE6C940) /* 0x080000 = 1.0x */
#define C850_PITCH              C850_S16(0x0CE6CA50) /* pitch, 1/100 % (FUN_0C0D9B44) [emu] */
#define C850_PITCH_WIDE         C850_U8(0x0CE6CA4C)  /* WIDE scale flag for C850_PITCH */

/* ---- BPM words (section 4, mods/bpm999) ---- */
#define C850_BPM_BASE_TENTHS    C850_U16(0x0C9D23EC) /* 1280 = 128.0 BPM */
#define C850_BPM_WHOLE_PITCHED  C850_U16(0x0C9D23F0)
#define C850_BPM_STAGING        C850_U16(0x0C9D0AE0) /* TX page 1 bytes 8..9 */
#define C850_PITCH_STAGING      C850_U16(0x0C9D0AE2) /* TX page 1 bytes 10..11 */
#define C850_PITCH_SCALE        C850_U16(0x0CE6CA4E) /* 300=+-6, 200=+-10/WIDE, 320=+-16 */
#define C850_STAGING_S18        C850_U8(0x0C9D0AF0)  /* sign (0x02|0x01) + MT (0x80) */
#define C850_STAGING_S19        C850_U8(0x0C9D0AF1)  /* 0x10=2 dec, 0x20=1 dec (WIDE), 0x40=BPM */
#define C850_STAGING_S1B        C850_U8(0x0C9D0AF3)  /* 0x04=WIDE, 0x20=16, 0x40=10, 0x80=6 */

/* ---- BPM Lock state (FUN_0C0D1F14) ---- */
#define C850_BPMLOCK_ACTIVE     C850_U8(0x0CE649AC)  /* 1 = locked to Master BPM */
#define C850_BPMLOCK_EDIT       C850_U8(0x0CE649AD)  /* 1 = editing Master BPM */
#define C850_BPMLOCK_SET        C850_U8(0x0CE649AE)  /* 1 = Master BPM initialized */
#define C850_BPMLOCK_MASTER     C850_U32(0x0CE649B0) /* Master BPM in tenths (e.g. 1280) */
#define C850_BPMLOCK_SAVED_WIDE C850_U8(0x0CE649B8)
#define C850_BPMLOCK_SAVED_SCALE C850_U16(0x0CE649BA)
#define C850_BPMLOCK_PITCH      C850_S16(0x0CE649BC) /* locked pitch in 1/100 % */
#define C850_BPMLOCK_DIR        C850_U8(0x0CE649BE)  /* catch-up direction (1/2/3) */
#define C850_BPMLOCK_PENDING    C850_U8(0x0CE649BF)  /* catch-up armed */
#define C850_BPMLOCK_TRACK_BPM  C850_U32(0x0CE649C4) /* track base tenths snapshot */
#define C850_BPMLOCK_TIMER      C850_U32(0x0C1E0C68) /* 10s Master edit timeout */

static inline s32 c850_effective_pitch(void)
{
    return C850_BPMLOCK_ACTIVE == 1 ? (s32)C850_BPMLOCK_PITCH : (s32)C850_PITCH;
}

/* ---- INFO screen & DB records ---- */
#define C850_DISPLAY_MODE       C850_U32(0x0C9D0AF8) /* 10 = loaded title, 11 = INFO list */
#define C850_INFO_CURSOR        C850_U8(0x0CE6D1E4)  /* 0..1 = title, 2.. = INFO list */
#define C850_INFO_COUNT         C850_U8(0x0CE6D1E5)
#define C850_INFO_TRACK         C850_S16(0x0C9D1D96)
#define C850_DB_RECORDS         0x0C9D44D4u
#define C850_DB_RECORD_SIZE     0x864u
#define C850_DB_RECORD_ID(rec)  C850_U32((rec) + 0x10)
#define C850_DB_RECORD_DRIVE(rec) C850_U16((rec) + 0x16)
#define C850_DB_RECORD_TITLE(rec) ((rec) + 0x38u)
#define C850_DB_USB_DRIVE       3u
#define C850_DB_LOAD_SORT_MENU  0x0C0750D8u
#define C850_DB_UNMOUNT         0x0C04F2B0u
#define c850_db_record_of_track C850_FN(0x0C073F7C, u32, (int track))
#define C850_ROW_SOURCE(r)      (0x0C9D196Fu + (r) * 0x100u)

/* ---- BROWSE ---- */
#define C850_NAV_VIEW           C850_U8(0x0CE6D0FD)
#define C850_NAV_BROWSE         1u
#define C850_BROWSE_CACHE_TITLE_OFFSET 0x14u  /* UTF-16 title after a cache entry's track ID */
#define C850_BROWSE_MENU_LEVEL  C850_U8(0x0C1E108C)
#define C850_DB_MENU_CACHE      0x0C0BBABEu
#define C850_DB_TRACK_QUERY     0x0C07C330u
#define C850_DB_SORT_CURSOR     0x0C11D224u
#define C850_DB_FINISH_RESULTS  0x0C0BABACu
#define C850_DB_RESULTS_COUNT(results) C850_U16((results) + 2)
#define C850_DB_RESULTS_SORT(results)  C850_U8((results) + 4)
#define C850_DB_RESULTS_INDEX(results) C850_U32((results) + 0x2C)

/* ---- Keys (section 4b.1) ---- */
#define C850_K0                 C850_U16(0x0CE6CA08) /* held keys, internal words */
#define C850_K1                 C850_U16(0x0CE6CA0A)
#define C850_K2                 C850_U16(0x0CE6CA0C)
#define C850_K0_PREV            C850_U16(0x0CE6CA22) /* latched each player cycle */
#define C850_K1_PREV            C850_U16(0x0CE6CA24)
#define C850_K2_PREV            C850_U16(0x0CE6CA26)
#define C850_NEW_KEYS           C850_U8(0x0CE6D132)
#define C850_UI_LOCK            C850_U8(0x0CE6D151)
#define C850_BEAT_FRACTION_MODE C850_U8(0x0CE6D154)  /* BEAT SELECT toggles it [emu] */
/* K0 bits */
#define C850_K0_REV         0x8000
#define C850_K0_CUE         0x4000
#define C850_K0_PLAY        0x2000
#define C850_K0_LOOP_IN     0x1000
#define C850_K0_LOOP_OUT    0x0800
#define C850_K0_RELOOP      0x0400
#define C850_K0_BEAT_SELECT 0x0040
#define C850_K0_SEARCH_REV  0x0020
#define C850_K0_SEARCH_FWD  0x0010
#define C850_K0_TRACK_NEXT  0x0008
#define C850_K0_TRACK_PREV  0x0004
/* K1 bits */
#define C850_K1_TAG_TRACK   0x8000
#define C850_K1_INFO        0x2000
#define C850_K1_TAG_LIST    0x1000
#define C850_K1_BROWSE      0x0800
#define C850_K1_VINYL       0x0400
#define C850_K1_TEMPO       0x0200
#define C850_K1_MASTER_TEMPO 0x0100
#define C850_K1_BEAT8       0x0080
#define C850_K1_BEAT4       0x0040
#define C850_K1_BEAT2       0x0020
#define C850_K1_BEAT1       0x0010
#define C850_K1_BACK        0x0008
#define C850_K1_EJECT       0x0004
/* K2 bits */
#define C850_K2_TIME_MODE   0x2000
#define C850_K2_MENU        0x1000
#define C850_K2_MEMORY      0x0800
#define C850_K2_DELETE      0x0400
#define C850_K2_CALL_NEXT   0x0200
#define C850_K2_CALL_PREV   0x0100

/* ---- Play position & PCM ring (section 4b units: 1 frame = 1/75 s = 588 samples) ---- */
#define C850_PLAY_FRAME         C850_U32(0x0C7F647C) /* current play frame [emu] */
#define C850_RING_READ_INDEX    C850_U16(0x0C7F645C)
#define C850_RING_WRITE_INDEX   C850_U16(0x0C7F645E)
#define C850_PCM_RING_BASE      0x0C1E8E5Cu          /* 2400 x 0x930 B stereo 16-bit BE PCM */
#define C850_PCM_RING_SLOTS     2400u
#define C850_PCM_SECTOR_BYTES   0x930u               /* 2352 B = 588 stereo 16-bit frames */
#define C850_PCM_READ_PTR       C850_U32(0x0C7F6470) /* next block the IRQ7 feeder plays */
#define C850_PCM_BLOCK_BYTES    0x150u               /* one DSPDREQ: 84 stereo samples, 1.9 ms */
#define C850_BLOCKS_PER_FRAME   7u                   /* 0x930 = 7 x 0x150 */

/* ---- Forward loop-out test of the IRQ7 feeder (payload/common/c850_loop_out.c) ----
 * Pool 0x0C07730C -> c850_loop_out_isr: loop_roll_out_hook, quant_loops_out_hook,
 * then isr_loop_out_reached (0x0C0158D4). Interrupt context. */
int c850_loop_out_isr(int sector, u32 out, int z);
u32 c850_read_pos(void);               /* block the read pointer names (frame * 7 + block) */
int c850_ring_jump(u32 pos_block);     /* 1 = read pointer moved there (in the ring) */

/* ---- Waveform staging & ANLZ buffers (section 5, FUN_0C066402, FUN_0C0C2402) ---- */
#define C850_WAVEFORM_SEQUENCE  C850_U8(0x0C9D1184)  /* 0x70 byte 1 sequence */
#define C850_WAVEFORM_CHANGED   C850_U32(0x0C9D1188) /* set 1 to bump sequence and resend */
#define C850_WAVEFORM_SAMPLES   0x0C9D1199u          /* 100 bytes sent by panel_tx_waveform */
#define C850_WAVEFORM_PWAV      0x0CE65862u          /* 400 x 2 B ANLZ PWAV (h 0..31, w 0..7) */
#define C850_WAVEFORM_OVERVIEW  0x0CE65B82u          /* 100 B overview (0..15) */

/* ---- Loops (section 4b.2) ---- */
#define C850_LOOP_ACTIVE        C850_U8(0x0CE6D0ED)
#define C850_CUE_OR_LOOP_EXISTS C850_U8(0x0CE6D0EC)
#define C850_AUTO_LOOP_BEATS    C850_U8(0x0C1E1088)  /* 1/2/4/8, 0 = none */
#define C850_LOOP_IN_FRAME      C850_U32(0x0C7F6498)
#define C850_LOOP_OUT_FRAME     C850_U32(0x0C7F64B0) /* applied */
#define C850_LOOP_OUT_REQUEST   C850_U32(0x0C7F64DC) /* from audio cmd 6 */
#define C850_TRACK_LENGTH       C850_U32(0x0C7F650C) /* frames */
#define C850_LOOP_D1C0          C850_U32(0x0CE6D1C0)
#define C850_AUDIO_LOOP_PLAYING C850_U8(0x0C7F6674)

/* ---- Cues (section 4b.3) ---- */
#define C850_MEMCUE_LIST        0x0CE649D4           /* 0x24-byte entries, end flags 0xFFFFFFFF */
#define C850_MEMCUE_COUNT       C850_U16(0x0CE65850)
#define C850_ANLZ_CTX           C850_U32(0x0CE649D0) /* hot cue A/B/C PCPT at +0xF90/+0xFC8/+0x1000 [emu] */
#define C850_JUMP_TARGET        0x0CE65C0C           /* 0x24-byte record cue_select_from_list fills */
#define C850_CUE_FRAME          C850_U32(0x0CE6D158)

/* ---- Beat grid (section 4b.4) ---- */
#define C850_GRID_CTX           0x0C9D24FC
#define C850_GRID_BEAT_COUNT    C850_U32(C850_GRID_CTX + 0x74)
#define C850_GRID_WINDOW_INDEX  C850_U16(C850_GRID_CTX + 0x78) /* window w holds beats w*500.. */
#define C850_GRID_WINDOW_COUNT  C850_U16(C850_GRID_CTX + 0x7A)
/* entry i of the current window: {u16 beat_in_bar, u16 bpm*100, u32 time_ms} */
#define C850_GRID_BEAT_IN_BAR(i) C850_U16(0x0C9D2578 + (i) * 8)
#define C850_GRID_BPM100(i)      C850_U16(0x0C9D257A + (i) * 8)
#define C850_GRID_TIME_MS(i)     C850_U32(0x0C9D257C + (i) * 8) /* ctx+0x80 + i*8 (grid_find_beat) */

/* ---- Panel LEDs and status staging (section 4b.5) ---- */
#define C850_STATUS_STAGING     0x0C9D0AD8           /* S, copied into TX pages 1/2 */
#define C850_FRAME_DIGIT        C850_U8(0x0C9D0ADE)  /* S[6], page 1 byte 7 */
#define C850_LEDS_PAGE2_B5      C850_U8(0x0C9D0AE9)  /* LOOP IN 0x20, LOOP OUT 0x40 */
#define C850_LEDS_PAGE2_B6      C850_U8(0x0C9D0AEA)  /* bit 0 RELOOP */
#define C850_LEDS_PAGE2_B7      C850_U8(0x0C9D0AEB)  /* bits 0..3 beat-loop 1/2/4/8 */

/* ---- Settings record (section 4b.6) ---- */
#define C850_SETTINGS           0x0C9DA23C           /* 0x58 bytes, flash log at 0x3F8000; never set
                                                        +0 bit 4, +1 bits 4..6, +0x43 bits 4..7 (log end) */

/* ---- Track INFO metadata (section 4b.8) ---- */
#define C850_DB_TRACK_INFO      0x0C054A36u        /* server handler for request 0x2002 */
#define C850_DB_USB_INFO        0x0301u            /* drive 3, rekordbox DB kind 1 */
#define C850_DB_USB_INFO_RATING(db) C850_U8((db) + 0x2946u) /* USB INFO field 6 enabled */
#define C850_INFO_RATING_TEXT(rec) ((rec) + 0x5A8)  /* INFO 0x0A, up to five panel glyphs */
#define C850_INFO_RATING_MAX    5
#define C850_INFO_RATING_FILLED 0x04               /* filled star, U+2605 [static] */

/* ---- Dot-matrix text rows (section 4b.7) ---- */
/* row record: [tag, attr0, attr1, attr2, len, text[len]] */
#define C850_MATRIX_ROW(r)      (0x0C9D0B18 + (r) * 0x105)
#define C850_MATRIX_DIRTY       C850_U8(0x0C9D1254)

/* ---- Auto standby (section 4) ---- */
#define C850_STANDBY_SECONDS    C850_U16(0x0C1E10AE)
#define C850_STANDBY_TICKS      C850_U32(0x0C1E10B0)
#define C850_STANDBY_STATE      C850_U8(0x0C1E10AC)

/* ---- MAIN functions (section 4b; signatures as traced) ---- */
/* auto_beat_loop_set(beats): beats = 1/2/4/8 [emu] */
#define c850_auto_beat_loop_set        C850_FN(0x0C0E570A, void, (int beats))
/* beat_loop_out_from_bpm(beats, &bpm, tracklen): 1 = enough audio buffered */
#define c850_beat_loop_out_from_bpm    C850_FN(0x0C0E5590, int, (int beats, int *bpm, u32 tracklen))
/* audio_send_cmd(cmd, flag, a, b, c, d, e, src); args 5..8 go on the stack */
#define c850_audio_send_cmd            C850_FN(0x0C0D7464, void, (int, int, u32, int, int, int, int, int))
/* frames <-> grid: grid_find_beat(ctx, time_ms, mode): index in the current window, -1 error */
#define c850_grid_find_beat            C850_FN(0x0C0012E4, int, (u32 ctx, u32 time_ms, int mode))
#define c850_grid_page_window          C850_FN(0x0C00119E, int, (u32 ctx, u32 file_id))
#define c850_ring_sector_to_frame      C850_FN(0x0C00851E, u32, (int ring_sector))
#define c850_sector_to_track           C850_FN(0x0C00586C, u32, (int ring_sector))
/* cue_call_prev_next(next): 0 = CALL prev, 1 = CALL next (USB branch) */
#define c850_cue_call_prev_next        C850_FN(0x0C0D3B1A, void, (int next))
/* cue_select_from_list(src, 1): fills C850_JUMP_TARGET and C850_CUE_FRAME */
#define c850_cue_select_from_list      C850_FN(0x0C0D275A, int, (int src, int dir))
/* player_keys_latch_prev(): copies K0..K2 to the _PREV words */
#define c850_player_keys_latch_prev    C850_FN(0x0C0E49CC, void, (void))
/* power/standby state machine, called by DjControlTask at 0x0C0EE644 (section 4) */
#define c850_power_state_tick          C850_FN(0x0C0EF8EC, u32, (void))
/* the 1 ms tick's power/standby part: calls the auto-standby seconds counter
 * FUN_0C0EF844; called from the 1 ms tick routine 0x0C0B92CC through its pool
 * word 0x0C0B9310 (section 8.4, payload_demo) */
#define c850_power_tick_1ms            C850_FN(0x0C0EF8C8, void, (void))
#define C850_TICK_MS                   C850_U32(0x0CE49F54) /* +1 per 1 ms tick */
/* BPM Lock dispatcher, recalc, and pitch fader converter */
#define c850_bpm_lock_cmd              C850_FN(0x0C0D1F14, u32, (u32 cmd, u32 a, u32 b))
#define c850_bpm_lock_recalc           C850_FN(0x0C0D1CE6, u32, (void))
#define c850_tempo_fader_to_pitch      C850_FN(0x0C0D9B44, u32, (void))

/* ---- Service-mode gate (docs/cdj850-modding.md, "Service mode gate") ----
 * Nonzero while the deck is in a power-on firmware-update mode (USB/CD
 * SELECT + RELOOP), or before MAIN has looked at the power-on keys at all.
 * The framework already enforces it for every entry: each hook trampoline
 * skips its hooks and each PoolRedirect goes through a generated gate
 * (payloadbuild.py, __gate_<symbol>) that jumps to the stock function. Payload
 * code does not need to test it itself; this is for code that wants to. */
#define C850_UPDATE_MODE        C850_U8(0x0C1E109A)  /* 1/2: RELOOP held at power-on */
#define C850_KEYS_PENDING       C850_U8(0x0C1E10AA)  /* 1 until the power-on keys are read */
static inline int c850_service_mode(void) { return C850_UPDATE_MODE | C850_KEYS_PENDING; }

/* ---- Shared state-4 tick (docs/cdj850-modding.md sections 4b.9, 4b.10) ----
 * keys_state4_looping (0x0C0E0F34) runs once per player cycle while a USB
 * loop plays (pool word 0x0C0D7B98, read only by the tail jump at
 * 0x0C0D79A6). One pool word can point at one routine, so every mod that
 * wants that tick redirects the word to the same symbol, c850_loop_tick (the
 * second redirect then finds it 'present'). Each such mod ships a source
 * payload/loop_tick.c that only expands C850_LOOP_TICK_DEFINE: weak, identical
 * definitions of the tick and of a no-op for every hook, so the link keeps
 * one tick, and a mod's own (strong) hook replaces its no-op. The tick calls
 * the hooks in this order, then the stock handler. Hooks: loop_resize_state4
 * (mods/loop_resize), quant_loops_loop_tick (mods/quant_loops: block loops). */
#define C850_STATE4_KEYS        C850_FN(0x0C0E0F34, void, (void))
#define C850_LOOP_TICK_DEFINE                                                   \
    void loop_resize_state4(void) __attribute__((weak));                        \
    void loop_resize_state4(void) {}                                            \
    void quant_loops_loop_tick(void) __attribute__((weak));                     \
    void quant_loops_loop_tick(void) {}                                         \
    void c850_loop_tick(void) __attribute__((weak));                            \
    void c850_loop_tick(void)                                                   \
    {                                                                           \
        loop_resize_state4();                                                   \
        quant_loops_loop_tick();                                                \
        C850_STATE4_KEYS();                                                     \
    }

#endif /* C850_H */
