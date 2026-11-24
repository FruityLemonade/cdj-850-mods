/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * auto_standby: AUTO STANDBY timeout selectable in UTILITY (20/40/60/120 MIN),
 * stored via c850_ui in PIONEER/MODSET.DAT (default 20 MIN).
 */
#include "c850_ui.h"

#define AS_STOCK_GET        C850_FN(0x0C08F186, u32, (void))
#define AS_IDLE_SECONDS     C850_U16(0x0C1E10AE)
#define AS_IDLE_MS          C850_U32(0x0C1E10B0)
#define AS_STOCK_LIMIT      0x3840u     /* 14400 s = 4 hours in table[1] at 0x0C18CCDE */

static const u16 as_limits[4] = { 1200u, 2400u, 3600u, 7200u };
static const char *const as_labels[4] = { "20 MIN", "40 MIN", "60 MIN", "120 MIN" };

u32 auto_standby_get_mode(void)
{
    u32 mode = AS_STOCK_GET();
    u16 limit, secs;
    u32 ms;

    if ((mode & 0xFFu) == 0u)
        return 0u;
    limit = as_limits[c850_ui_standby_index() & 3u];
    secs = AS_IDLE_SECONDS;
    ms = AS_IDLE_MS;
    if (secs >= limit || ((u32)secs + 1u >= (u32)limit && ms >= 999u))
        AS_IDLE_SECONDS = (u16)(AS_STOCK_LIMIT - 1u);
    return mode;
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
