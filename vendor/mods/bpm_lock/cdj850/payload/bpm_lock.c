/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bpm_lock: CDJ-350 BPM Lock with ±100% range on the CDJ-850 (MAIN 1.13).
 *
 * Uses MAIN's dormant BPM-lock dispatcher FUN_0C0D1F14 (state block
 * 0x0CE649AC):
 *   BEAT SELECT + MASTER TEMPO (short)      toggle BPM Lock on/off
 *   BEAT SELECT + MASTER TEMPO (hold 1 s)   enter/update Master BPM edit mode
 *                                           (rotary turns 0.1 BPM, push+turn
 *                                           1.0 BPM, BACK/push/10 s exits)
 *
 * Integer only (no FPU).
 */
#include "c850.h"
#include "c850_ui.h"

#define BL_NOTIFY_STOCK C850_FN(0x0C06E58A, u32, (u32 code, s32 dur))
#define BL_HOLD_MS      1000u

volatile u32 bpm_lock_on = 1;

/* Evidence counters for tests. */
volatile u32 bpm_lock_toggles;
volatile u32 bpm_lock_edits;
volatile u32 bpm_lock_notifies;
volatile u32 bpm_lock_last_code;
volatile u32 bpm_lock_wide_switches;
volatile u32 bpm_lock_display_updates;

static u8 bl_hold;
static u8 bl_hold_fired;
static u32 bl_hold_t0;
static u8 bl_pending_toggle;
static u8 bl_was_locked;
static u8 bl_was_edit;
static u32 bl_last_master;

static u8 bl_have_saved;
static u8 bl_saved_range;
static u8 bl_saved_wide;
static u16 bl_saved_scale;

static char bl_flash_buf[24];

static void bl_fmt_master(u32 tenths)
{
    u8 digits[8];
    u32 whole = tenths / 10u;
    u32 dec = tenths % 10u;
    int k = 0, p = 0;

    bl_flash_buf[p++] = 'M';
    bl_flash_buf[p++] = 'A';
    bl_flash_buf[p++] = 'S';
    bl_flash_buf[p++] = 'T';
    bl_flash_buf[p++] = 'E';
    bl_flash_buf[p++] = 'R';
    bl_flash_buf[p++] = ' ';
    do {
        digits[k++] = (u8)('0' + whole % 10u);
        whole /= 10u;
    } while (whole && k < 8);
    while (k)
        bl_flash_buf[p++] = (char)digits[--k];
    bl_flash_buf[p++] = '.';
    bl_flash_buf[p++] = (char)('0' + dec);
    bl_flash_buf[p] = 0;
}

/* ---- key filter (c850_ui_keys) ---- */

static void bpm_lock_key_filter(const u16 *k, const u16 *p, u16 *own)
{
    if (!bpm_lock_on || c850_ui_flag(C850_UI_FLAG_BPM_LOCK_OFF))
        return;
    if (c850_ui_shift_k1(k, p, own, C850_K1_MASTER_TEMPO)) {
        bl_hold = 1;
        bl_hold_fired = 0;
        bl_hold_t0 = C850_TICK_MS;
        return;
    }
    if (bl_hold) {
        if (k[1] & C850_K1_MASTER_TEMPO) {
            own[1] |= C850_K1_MASTER_TEMPO;
            if (k[0] & C850_K0_BEAT_SELECT)
                own[0] |= C850_K0_BEAT_SELECT;
        } else {
            if (!bl_hold_fired)
                bl_pending_toggle = 1;
            bl_hold = 0;
        }
    }
}
C850_UI_KEY_FILTER(bpm_lock_key_filter);

/* ---- state tick (c850_ui_tick) ---- */

static void bl_apply_wide_if_needed(void)
{
    s32 p = C850_BPMLOCK_PITCH;

    if (p >= -1600 && p <= 1600)
        return;
    if (C850_PITCH_WIDE == 1 && C850_TEMPO_RANGE == 4 && C850_PITCH_SCALE == 200)
        return;
    /* Temporarily clear ACTIVE while recomputing fader pitch under WIDE so
     * FUN_0C0D1E84 (called from tempo_fader_to_pitch) does not unlock. */
    C850_BPMLOCK_ACTIVE = 0;
    C850_TEMPO_RANGE = 4;
    C850_PITCH_WIDE = 1;
    C850_PITCH_SCALE = 200;
    c850_tempo_fader_to_pitch();
    C850_BPMLOCK_ACTIVE = 1;
    c850_bpm_lock_recalc();
    bpm_lock_wide_switches++;
}

static void bl_restore_saved_range(void)
{
    if (!bl_have_saved)
        return;
    C850_TEMPO_RANGE = bl_saved_range;
    C850_PITCH_WIDE = bl_saved_wide;
    C850_PITCH_SCALE = bl_saved_scale;
    c850_tempo_fader_to_pitch();
    bl_have_saved = 0;
}

void bpm_lock_tick(void)
{
    if (!bpm_lock_on) {
        bl_hold = 0;
        bl_pending_toggle = 0;
        return;
    }
    if (c850_ui_flag(C850_UI_FLAG_BPM_LOCK_OFF)) {
        bl_hold = 0;
        bl_pending_toggle = 0;
        if (C850_BPMLOCK_EDIT == 1)
            c850_bpm_lock_cmd(3, 0, 0);
        if (C850_BPMLOCK_ACTIVE == 1) {
            c850_bpm_lock_cmd(7, 0, 0);
            bl_restore_saved_range();
        }
        bl_was_locked = C850_BPMLOCK_ACTIVE;
        bl_was_edit = C850_BPMLOCK_EDIT;
        return;
    }

    /* 1-second hold triggers Master BPM edit mode. */
    if (bl_hold && !bl_hold_fired && (u32)(C850_TICK_MS - bl_hold_t0) >= BL_HOLD_MS) {
        bl_hold_fired = 1;
        c850_bpm_lock_cmd(1, 0, 0);
        bpm_lock_edits++;
        if (C850_BPMLOCK_EDIT == 1) {
            bl_last_master = C850_BPMLOCK_MASTER;
            bl_fmt_master(bl_last_master);
            c850_ui_flash(bl_flash_buf);
        }
    }

    /* Short release toggles BPM Lock (or confirms Master edit if already locked). */
    if (bl_pending_toggle) {
        u8 was_active = C850_BPMLOCK_ACTIVE;
        u8 pre_range = C850_TEMPO_RANGE;
        u8 pre_wide = C850_PITCH_WIDE;
        u16 pre_scale = C850_PITCH_SCALE;

        bl_pending_toggle = 0;
        c850_bpm_lock_cmd(0, 0, 0);
        bpm_lock_toggles++;

        if (!was_active && C850_BPMLOCK_ACTIVE == 1) {
            bl_saved_range = pre_range;
            bl_saved_wide = pre_wide;
            bl_saved_scale = pre_scale;
            bl_have_saved = 1;
            bl_apply_wide_if_needed();
        } else if (was_active && C850_BPMLOCK_ACTIVE == 0) {
            bl_restore_saved_range();
        } else if (was_active && C850_BPMLOCK_ACTIVE == 1) {
            bl_apply_wide_if_needed();
        }
    }

    /* If lock disengaged automatically (catch-up or track change), do not force-restore
     * range on catch-up so the physical fader stays continuous; just clear saved flag. */
    if (bl_was_locked && C850_BPMLOCK_ACTIVE == 0)
        bl_have_saved = 0;
    bl_was_locked = C850_BPMLOCK_ACTIVE;

    /* Keep WIDE engaged if rotary Master BPM editing moves locked pitch beyond ±16%. */
    if (C850_BPMLOCK_ACTIVE == 1)
        bl_apply_wide_if_needed();

    /* Live flash while rotating Master BPM in edit mode. */
    if (C850_BPMLOCK_EDIT == 1) {
        u32 m = C850_BPMLOCK_MASTER;
        if (!bl_was_edit || m != bl_last_master) {
            bl_last_master = m;
            bl_fmt_master(m);
            c850_ui_flash(bl_flash_buf);
        }
    }
    bl_was_edit = C850_BPMLOCK_EDIT;
}
C850_UI_TICK_HOOK(bpm_lock_tick);

/* ---- notify redirect (pools 0x0C0D1DA4 / 0x0C0D1F34 -> 0x0C06E58A) ---- */

u32 bpm_lock_notify(u32 code, s32 dur)
{
    u32 c = code & 0xFFu;

    if (!bpm_lock_on || c850_ui_flag(C850_UI_FLAG_BPM_LOCK_OFF))
        return BL_NOTIFY_STOCK(code, dur);

    bpm_lock_notifies++;
    bpm_lock_last_code = c;
    switch (c) {
    case 1:
        c850_ui_flash("MASTER 130.0");
        return c;
    case 2:
        c850_ui_flash("NO MASTER BPM");
        return c;
    case 3:
        c850_ui_flash("NO BPM");
        return c;
    case 4:
    case 0x13:
        c850_ui_flash("OUT OF RANGE");
        return c;
    case 0x0D:
        c850_ui_flash("BPM LOCK ON");
        return c;
    case 0x0E:
        c850_ui_flash("BPM LOCK OFF");
        return c;
    case 0x0F:
        bl_fmt_master(C850_BPMLOCK_MASTER);
        c850_ui_flash(bl_flash_buf);
        return c;
    default:
        return BL_NOTIFY_STOCK(code, dur);
    }
}

static const char *bl_utility_value(void)
{
    if (!bpm_lock_on)
        return 0;
    return c850_ui_flag(C850_UI_FLAG_BPM_LOCK_OFF) ? "OFF" : "ON";
}

static void bl_utility_select(void)
{
    c850_ui_set_flag(C850_UI_FLAG_BPM_LOCK_OFF,
                     !c850_ui_flag(C850_UI_FLAG_BPM_LOCK_OFF));
}

static const c850_ui_utility_item bl_utility_item = {
    40u, "BPM LOCK", bl_utility_value, bl_utility_select,
};
C850_UI_UTILITY_ITEM(bl_utility_item);

/* ---- display hook (tail of display_stage_bpm) ---- */

void bpm_lock_display_hook(u32 *regs, u32 *sp)
{
    (void)regs;
    (void)sp;
    if (!bpm_lock_on || c850_ui_flag(C850_UI_FLAG_BPM_LOCK_OFF))
        return;
    if (C850_BPMLOCK_EDIT == 1 && C850_BPMLOCK_MASTER > 0u) {
        u32 bpm = (C850_BPMLOCK_MASTER + 5u) / 10u;
        if (bpm > 999u)
            bpm = 999u;
        C850_BPM_STAGING = (u16)bpm;
        bpm_lock_display_updates++;
        return;
    }
    if (C850_BPMLOCK_ACTIVE == 1) {
        s32 p = C850_BPMLOCK_PITCH;
        u32 mag = (u32)(p < 0 ? -p : p);
        u32 base = C850_BPM_BASE_TENTHS;
        u8 s18 = C850_STAGING_S18;
        u8 s19 = C850_STAGING_S19;

        if (base != 0u && C850_BPM_STAGING != 0xFFFFu) {
            u32 bpm = (base * (u32)(10000 + p) + 50000u) / 100000u;
            if (bpm > 999u)
                bpm = 999u;
            C850_BPM_STAGING = (u16)bpm;
        }
        if (C850_PITCH_WIDE == 1) {
            mag /= 10u;
            s19 = (u8)((s19 & ~0x10u) | 0x20u);
        } else {
            s19 = (u8)((s19 & ~0x20u) | 0x10u);
        }
        C850_STAGING_S19 = s19;
        C850_PITCH_STAGING = (u16)mag;
        if (p > 0)
            s18 = (u8)((s18 & ~0x03u) | 0x03u);
        else if (p < 0)
            s18 = (u8)((s18 & ~0x03u) | 0x02u);
        else
            s18 = (u8)(s18 & ~0x03u);
        C850_STAGING_S18 = s18;
        bpm_lock_display_updates++;
    }
}
