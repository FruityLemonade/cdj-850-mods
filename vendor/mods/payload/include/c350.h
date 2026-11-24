/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c350.h -- MAIN 1.14 addresses for CDJ-350 payload code (mods/payloadbuild.py).
 *
 * Addresses traced on CDJ-350 MAIN 1.14 (docs/cdj350.md and
 * scripts/ghidra/cdj350-annotations.json). Nothing here is valid for another
 * firmware version.
 *
 * Built with -mrenesas -m2a-nofpu (see c850.h for ABI and context rules).
 */
#ifndef C350_H
#define C350_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;

#define C350_U8(a)  (*(volatile u8 *)(a))
#define C350_S8(a)  (*(volatile s8 *)(a))
#define C350_U16(a) (*(volatile u16 *)(a))
#define C350_S16(a) (*(volatile s16 *)(a))
#define C350_U32(a) (*(volatile u32 *)(a))

#define C350_FN(addr, ret, args) ((ret (*) args)(addr))

/* ---- Player, transport ---- */
#define C350_IS_USB_MODE        C350_U8(0x0CD48D0E)  /* 1 = USB/data mode, 0 = CD */
#define C350_SOURCE_INDEX       C350_U8(0x0CD48D06)  /* active source (0 = disc, 2 = USB) */
#define C350_PLAYER_STATE_BASE  0x0CD48D11           /* + source index */
#define C350_PLAYER_STATE       C350_U8(C350_PLAYER_STATE_BASE + C350_SOURCE_INDEX)
#define C350_STATE_PLAYING      3
#define C350_STATE_LOOPING      4
#define C350_STATE_PAUSED_CDJ   5
#define C350_STATE_CUED         6

/* ---- BPM ---- */
#define C350_BPM_BASE_TENTHS    C350_U16(0x0C9B8E24) /* 1280 = 128.0 BPM */

/* ---- Play position & audio ring (1 frame = 1/75 s = 588 samples) ---- */
#define C350_PLAY_SECTOR        C350_U16(0x0C7E4D10)
#define C350_IN_SECTOR          C350_S16(0x0C7E4D1C)
#define C350_PLAY_FRAME         C350_U32(0x0C7E4D30)
#define C350_CUE_FRAME          C350_U32(0x0C7E4D4C)
#define C350_LOOP_IN_FRAME      C350_U32(0x0C7E4D4C)
#define C350_LOOP_OUT_FRAME     C350_U32(0x0C7E4D74)
#define C350_LOOP_OUT_REQUEST   C350_U32(0x0C7E4D8C) /* from audio cmd 0x1e */
#define C350_TRACK_LENGTH       C350_U32(0x0C7E4DBC)

/* ---- Loops ---- */
#define C350_LOOP_ACTIVE        C350_U8(0x0CD48BD1)
#define C350_LOOP_OUT_CMD       C350_U32(0x0CD48CCC)
#define C350_LOOP_D1C0          C350_U32(0x0CD48574)
#define C350_CUE_TRACK          C350_U16(0x0CD48D5A)
#define C350_TRACKLEN(slot)     C350_U32(0x0CD3D7E0 + ((slot) & 3u) * 0x33Cu)

/* ---- Beat grid ---- */
#define C350_GRID_CTX           0x0C9B8F34
#define C350_GRID_WINDOW_OK     C350_U32(C350_GRID_CTX + 0x70)
#define C350_GRID_BEAT_COUNT    C350_U32(C350_GRID_CTX + 0x74)
#define C350_GRID_WINDOW_INDEX  C350_U16(C350_GRID_CTX + 0x78)
#define C350_GRID_WINDOW_COUNT  C350_U16(C350_GRID_CTX + 0x7A)
#define C350_GRID_BEAT_IN_BAR(i) C350_U16(0x0C9B8FB0 + (i) * 8)
#define C350_GRID_BPM100(i)      C350_U16(0x0C9B8FB2 + (i) * 8)
#define C350_GRID_TIME_MS(i)     C350_U32(0x0C9B8FB4 + (i) * 8)

/* ---- MAIN functions ---- */
#define c350_audio_send_cmd     C350_FN(0x0C0C7850, void, (int, int, u32, int, int, int, int, int))
#define c350_ring_sector_to_frame C350_FN(0x0C0090D6, u32, (int ring_sector))
#define c350_ring_add           C350_FN(0x0C008E38, int, (int sector, int delta))
#define c350_sector_track       C350_FN(0x0C006950, u32, (int sector))
#define c350_next_sector        C350_FN(0x0C006A92, void, (void))
#define c350_state4_keys        C350_FN(0x0C0D28AC, void, (void))

#endif /* C350_H */
