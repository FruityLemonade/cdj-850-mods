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

/* ---- BPM, pitch ---- */
#define C350_BPM_BASE_TENTHS    C350_U16(0x0C9B8E24) /* 1280 = 128.0 BPM */
#define C350_PITCH              C350_S16(0x0CD4853E) /* 1/100 %: fader top on +-10 = 1000 [emu] */
#define C350_MASTER_TEMPO       C350_U8(0x0CD48BC4)  /* 1 = MASTER TEMPO on [emu] */

/* ---- Track records (the DB record of a track handle, 0x35C bytes) ---- */
#define c350_record_of_track    C350_FN(0x0C06BD38, u32, (int track))
#define C350_REC_ID(rec)        C350_U32((rec) + 0x10)   /* rekordbox track id */
#define C350_REC_BPM_OK(rec)    C350_U32((rec) + 0x18)
#define C350_REC_BPM100(rec)    C350_U32((rec) + 0x1C)
#define C350_REC_TITLE(rec)     ((rec) + 0x38)           /* ASCII, NUL-terminated */
#define C350_REC_ARTIST(rec)    ((rec) + 0x98)
#define C350_REC_ALBUM(rec)     ((rec) + 0xC8)
#define c350_track_title        C350_FN(0x0C06C3C4, u32, (int track))
#define c350_track_artist       C350_FN(0x0C06C3DC, u32, (int track))
#define c350_track_album        C350_FN(0x0C06C3F8, u32, (int track))

/* ---- INFO view (track view, rotary pages of row 2) ---- */
#define C350_INFO_PAGE          C350_U8(0x0CD48BE2)  /* 0 artist, 1 album, 2 phase meter */
#define c350_info_page_scroll   C350_FN(0x0C0D664A, void, (u32 dir, u32 steps))
#define c350_display_request    C350_FN(0x0C06777A, u32, (int))

/* ---- rekordbox DB server (request 0x2002 track metadata) ---- */
#define c350_db_track_row       C350_FN(0x0C0719A4, int, (u32 id, u32 row))
#define C350_DB_ROW_RATING(row) C350_U8((row) + 0x1C)    /* 0..5 */
#define C350_DB_ROW_KEY(row)    C350_U32((row) + 0x2C)   /* key id */
#define c350_db_key_name        C350_FN(0x0C07D14C, int, (u32 key, u32 *text))  /* UTF-16 */

/* ---- Play position & audio ring (1 frame = 1/75 s = 588 samples) ---- */
#define C350_PLAY_SECTOR        C350_U16(0x0C7E4D10)
#define C350_IN_SECTOR          C350_S16(0x0C7E4D1C)
#define C350_PLAY_FRAME         C350_U32(0x0C7E4D30)
#define C350_CUE_FRAME          C350_U32(0x0C7E4D4C)
#define C350_LOOP_IN_FRAME      C350_U32(0x0C7E4D4C)
#define C350_LOOP_OUT_FRAME     C350_U32(0x0C7E4D74)  /* copy of the out record (0x0C00A7E0) */
#define C350_LOOP_OUT_REQUEST   C350_U32(0x0C7E4D8C) /* from audio cmd 0x1e */
#define C350_TRACK_LENGTH       C350_U32(0x0C7E4DBC)

/* ---- PCM ring and the IRQ feeder (docs/cdj350.md, "Block-precise loops") ----
 * Same layout as the CDJ-850's: 2400 sectors (frames) of 0x930 bytes, played
 * in blocks of 84 stereo samples (0x150 bytes, 7 per sector) by the feeder
 * ISR 0x0C06E118, which advances the read pointer and recomputes the read
 * sector before its loop-out test. */
#define C350_RING_READ_INDEX    C350_U16(0x0C7E4D10)  /* = C350_PLAY_SECTOR */
#define C350_RING_WRITE_INDEX   C350_U16(0x0C7E4D12)  /* forward fill */
#define C350_RING_OLDEST        C350_U16(0x0C7E4D14)  /* oldest sector kept (<= 1000 behind) */
#define C350_PCM_READ_PTR       C350_U32(0x0C7E4D24)  /* next block the feeder plays */
#define C350_PCM_RING_BASE      0x0C1D7710u
#define C350_PCM_RING_SLOTS     2400u
#define C350_PCM_SECTOR_BYTES   0x930u
#define C350_PCM_BLOCK_BYTES    0x150u
#define C350_BLOCKS_PER_FRAME   7u
#define C350_AUDIO_CD_MODE      C350_U8(0x0C7E4F78)   /* 1 = CD (MSF sector table), 0 = USB data [emu] */
#define C350_AUDIO_ALT_FILL     C350_U8(0x0C7E4F1C)   /* 0 = the forward-fill branch of the loop-out test */
#define C350_AUDIO_LOOP_PLAYING C350_U8(0x0C7E4F24)   /* set by the out-record builder 0x0C00A7E0 */
#define C350_TICK_MS            C350_U32(0x0CD2DCC0)  /* +1 per 1 ms tick */

/* ---- Loops ---- */
#define C350_LOOP_IN_SECTOR     C350_U16(0x0C7E4D5C)  /* in record +0x10: its ring sector */
#define C350_LOOP_OUT_RECORD    C350_U32(0x0C7E4D60)  /* applied out frame the feeder tests (+0x10 sector) */
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
#define c350_beat_grid_frame(i) ((C350_GRID_TIME_MS(i) * 75u + 500u) / 1000u)

#endif /* C350_H */
