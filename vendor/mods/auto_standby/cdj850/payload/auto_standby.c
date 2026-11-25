/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * auto_standby: AUTO STANDBY timeout selectable in UTILITY (20/40/60/120 MIN),
 * stored via c850_ui in PIONEER/MODSET.DAT (default 20 MIN).
 *
 * Stock idle gate FUN_0C0EF7D6 only lets auto_standby_seconds count on the
 * USB source while no stick is mounted (0x0CE6D147 == 0); with a stick in it
 * clears the counter on every tick, so the deck never went to standby from
 * USB. While on USB with a stick mounted and the player idle, the payload
 * keeps its own millisecond count and makes the stock request at the limit;
 * the stock key-activity reset (FUN_0C0EFAEC, pool 0x0C0EFB84) clears it.
 */
#include "c850_ui.h"

#define AS_STOCK_GET        C850_FN(0x0C08F186, u32, (void))
#define AS_REQUEST          C850_FN(0x0C0EFB10, u32, (u32 state))
#define AS_COMMIT           C850_FN(0x0C0EFABA, u32, (void))
#define AS_IDLE_SECONDS     C850_U16(0x0C1E10AE)
#define AS_IDLE_MS          C850_U32(0x0C1E10B0)
#define AS_COUNT_ENABLE     C850_U8(0x0C1E10AD)  /* stock: 0 = no countdown */
#define AS_USB_STATE        C850_U8(C850_PLAYER_STATE_BASE + 2u)
#define AS_SOURCE_USB       2u
#define AS_STOCK_LIMIT      0x3840u     /* 14400 s = 4 hours in table[1] at 0x0C18CCDE */

static const u16 as_limits[4] = { 1200u, 2400u, 3600u, 7200u };
static const char *const as_labels[4] = { "20 MIN", "40 MIN", "60 MIN", "120 MIN" };

volatile u32 auto_standby_usb_ms;       /* idle ms on USB with a stick mounted */
volatile u32 auto_standby_usb_requests; /* standby requests made from that count */

static u32 as_usb_idle(void)
{
    u8 st;

    if (C850_SOURCE_INDEX != AS_SOURCE_USB || C850_USB_MOUNTED == 0u)
        return 0u;
    st = AS_USB_STATE;
    /* browse / nothing loaded, paused, cued; not loading (2), playing (3),
     * looping (4) or the search / scratch states 7..9 */
    return st == 0u || st == C850_STATE_PAUSED_CDJ || st == C850_STATE_CUED ||
           st == 10u || st == 12u || st == 14u;
}

/* Pool 0x0C0EF998: read once per 1 ms tick by auto_standby_seconds. */
u32 auto_standby_get_mode(void)
{
    u32 mode = AS_STOCK_GET();
    u16 limit, secs;
    u32 ms;

    if ((mode & 0xFFu) == 0u || AS_COUNT_ENABLE == 0u) {
        auto_standby_usb_ms = 0u;
        return mode;
    }
    limit = as_limits[c850_ui_standby_index() & 3u];
    if (as_usb_idle()) {
        /* the stock gate fails here and clears its own counter after us */
        ms = auto_standby_usb_ms + 1u;
        if (ms >= (u32)limit * 1000u) {
            ms = 0u;
            auto_standby_usb_requests++;
            AS_REQUEST(2u);
            AS_COMMIT();
        }
        auto_standby_usb_ms = ms;
        return mode;
    }
    auto_standby_usb_ms = 0u;
    secs = AS_IDLE_SECONDS;
    ms = AS_IDLE_MS;
    if (secs >= limit || ((u32)secs + 1u >= (u32)limit && ms >= 999u))
        AS_IDLE_SECONDS = (u16)(AS_STOCK_LIMIT - 1u);
    return mode;
}

/* Pool 0x0C0EFB84: read by the stock key-activity reset FUN_0C0EFAEC. */
u32 auto_standby_activity(void)
{
    auto_standby_usb_ms = 0u;
    return AS_STOCK_GET();
}

static const char *as_utility_value(void)
{
    return as_labels[c850_ui_standby_index() & 3u];
}

static void as_utility_select(void)
{
    u32 next = (c850_ui_standby_index() + 1u) & 3u;
    c850_ui_set_flags(C850_UI_FLAG_STANDBY_MASK, next << C850_UI_FLAG_STANDBY_SHIFT);
}

static const c850_ui_utility_item as_utility_item = {
    10u, "STANDBY TIME", as_utility_value, as_utility_select,
};
C850_UI_UTILITY_ITEM(as_utility_item);
