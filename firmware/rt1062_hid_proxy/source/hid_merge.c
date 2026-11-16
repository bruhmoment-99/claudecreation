/*
 * hid_merge.c - Merges the physical mouse stream with UART injections and
 * implements button state locking.
 *
 * Model
 * -----
 *   physButtons : last button byte reported by the physical mouse
 *   injButtons  : buttons held down by cmd.left/right/middle(1)
 *   logical     : physButtons | injButtons     <- what the PC should see
 *   lastSent    : button byte of the last report handed to USB1
 *   accX/Y/W    : motion not yet delivered (physical + injected deltas)
 *
 * A report is emitted only when there is motion to deliver OR the logical
 * button byte differs from lastSent. Consequences:
 *   - cmd.left(1) while the physical left button is already down: logical does
 *     not change -> nothing is sent (no duplicate "press").
 *   - physical release while cmd.left(1) is active: logical stays pressed ->
 *     nothing is sent; the PC keeps seeing the button held.
 *
 * Motion is *accumulated*, never overwritten: if the USB1 IN endpoint is still
 * busy when a new physical report arrives, its deltas are added to the
 * accumulator and go out in the next report. Values exceeding the report range
 * are split across consecutive reports (the remainder stays in the
 * accumulator), so nothing is lost or repeated.
 */
#include "hid_merge.h"
#include "app_config.h"

#include <stddef.h>

#ifdef UNIT_TEST
#define CRIT_ENTER()  (0U)
#define CRIT_EXIT(m)  ((void)(m))
#else
#include "fsl_common.h"
/*
 * PRIMASK-based lock: safe in thread and ISR context alike, and also masks the
 * USB1 ISR itself (which calls hid_merge_on_tx_complete). Held for < 1 us.
 */
#define CRIT_ENTER()  DisableGlobalIRQ()
#define CRIT_EXIT(m)  EnableGlobalIRQ(m)
#endif

#define ACC_LIMIT     (1L << 24) /* saturate accumulators far below INT32_MAX */

typedef struct
{
    volatile bool ready;   /* USB1 configured                     */
    volatile bool txBusy;  /* an IN transfer is queued on USB1    */
    uint8_t physButtons;
    uint8_t injButtons;
    uint8_t lastSent;
    bool forceButtons;     /* resend buttons even if unchanged    */
    int32_t accX;
    int32_t accY;
    int32_t accW;
} merge_state_t;

static merge_state_t s_m;

static inline int32_t sat_add(int32_t acc, int32_t v)
{
    int64_t r = (int64_t)acc + v;
    if (r > ACC_LIMIT)
    {
        r = ACC_LIMIT;
    }
    else if (r < -ACC_LIMIT)
    {
        r = -ACC_LIMIT;
    }
    return (int32_t)r;
}

static inline int32_t clamp(int32_t v, int32_t lim)
{
    return (v > lim) ? lim : ((v < -lim) ? -lim : v);
}

/*
 * Build and send one report if (a) USB1 is ready, (b) the IN endpoint is idle,
 * and (c) there is something worth sending. Safe to call from any context.
 */
static APP_HOT void try_send(void)
{
    hid_mouse_report_t r;
    uint32_t m = CRIT_ENTER();

    if (!s_m.ready || s_m.txBusy)
    {
        CRIT_EXIT(m);
        return;
    }

    uint8_t logical = (uint8_t)(s_m.physButtons | s_m.injButtons);
    bool motion     = (s_m.accX != 0) || (s_m.accY != 0) || (s_m.accW != 0);

    if (!motion && (logical == s_m.lastSent) && !s_m.forceButtons)
    {
        CRIT_EXIT(m); /* nothing changed -> stay silent (button locking) */
        return;
    }

    int32_t x = clamp(s_m.accX, 32767);
    int32_t y = clamp(s_m.accY, 32767);
    int32_t w = clamp(s_m.accW, 127);
    s_m.accX -= x; /* step 4: consume what we deliver, keep remainder */
    s_m.accY -= y;
    s_m.accW -= w;

    r.buttons = logical;
    r.x       = (int16_t)x;
    r.y       = (int16_t)y;
    r.wheel   = (int8_t)w;

    s_m.lastSent     = logical;
    s_m.forceButtons = false;
    s_m.txBusy       = true; /* claim the endpoint while still locked */
    CRIT_EXIT(m);

    if (!usb_device_send_report(&r))
    {
        /* Not queued (bus reset in progress, not configured ...). Put the
         * motion back and force the button byte out next time. */
        m          = CRIT_ENTER();
        s_m.txBusy = false;
        if (s_m.ready)
        {
            s_m.accX         = sat_add(s_m.accX, x);
            s_m.accY         = sat_add(s_m.accY, y);
            s_m.accW         = sat_add(s_m.accW, w);
            s_m.forceButtons = true;
        }
        CRIT_EXIT(m);
    }
}

void hid_merge_init(void)
{
    uint32_t m = CRIT_ENTER();
    s_m.ready        = false;
    s_m.txBusy       = false;
    s_m.physButtons  = 0U;
    s_m.injButtons   = 0U;
    s_m.lastSent     = 0U;
    s_m.forceButtons = false;
    s_m.accX = s_m.accY = s_m.accW = 0;
    CRIT_EXIT(m);
}

void hid_merge_set_device_ready(bool ready)
{
    uint32_t m = CRIT_ENTER();
    s_m.ready  = ready;
    s_m.txBusy = false; /* any queued transfer was cancelled by the reset   */
    s_m.accX = s_m.accY = s_m.accW = 0; /* stale motion is meaningless now */
    s_m.lastSent     = 0U;               /* PC assumes all-released state   */
    s_m.forceButtons = false;
    CRIT_EXIT(m);

    if (ready)
    {
        try_send(); /* re-announce any held buttons */
    }
}

/*
 * Data-flow steps 1..4 from the spec, executed on every physical report:
 *  1. add physical deltas to the pending injection deltas (accumulator)
 *  2. logical buttons = physical | injected        (inside try_send)
 *  3. send merged report to USB1                    (inside try_send)
 *  4. clear the delivered motion                    (inside try_send)
 */
APP_HOT void hid_merge_physical(uint8_t buttons, bool buttonsValid, int32_t dx, int32_t dy, int32_t wheel)
{
    uint32_t m = CRIT_ENTER();
    if (buttonsValid)
    {
        s_m.physButtons = buttons;
    }
    if (s_m.ready)
    {
        s_m.accX = sat_add(s_m.accX, dx);
        s_m.accY = sat_add(s_m.accY, dy);
        s_m.accW = sat_add(s_m.accW, wheel);
    }
    CRIT_EXIT(m);
    try_send();
}

void hid_merge_physical_disconnected(void)
{
    uint32_t m      = CRIT_ENTER();
    s_m.physButtons = 0U;
    CRIT_EXIT(m);
    try_send(); /* emits a release if a physical-only button was held */
}

void hid_merge_inject_move(int32_t dx, int32_t dy)
{
    uint32_t m = CRIT_ENTER();
    if (s_m.ready)
    {
        s_m.accX = sat_add(s_m.accX, dx);
        s_m.accY = sat_add(s_m.accY, dy);
    }
    CRIT_EXIT(m);
#if APP_INJECT_SEND_IMMEDIATE
    try_send();
#endif
}

void hid_merge_inject_wheel(int32_t delta)
{
    uint32_t m = CRIT_ENTER();
    if (s_m.ready)
    {
        s_m.accW = sat_add(s_m.accW, delta);
    }
    CRIT_EXIT(m);
#if APP_INJECT_SEND_IMMEDIATE
    try_send();
#endif
}

void hid_merge_inject_button(uint8_t mask, bool pressed)
{
    uint32_t m = CRIT_ENTER();
    if (pressed)
    {
        s_m.injButtons |= mask;
    }
    else
    {
        s_m.injButtons &= (uint8_t)~mask;
    }
    CRIT_EXIT(m);
    /* Button changes are always pushed immediately; try_send() itself
     * suppresses the report when the logical state did not change. */
    try_send();
}

void hid_merge_set_injected_buttons(uint8_t mask)
{
    uint32_t m     = CRIT_ENTER();
    s_m.injButtons = mask;
    CRIT_EXIT(m);
    /* Same locking rule: only reported if (physical | injected) changes. */
    try_send();
}

uint8_t hid_merge_get_logical_buttons(void)
{
    uint32_t m = CRIT_ENTER();
    uint8_t v  = (uint8_t)(s_m.physButtons | s_m.injButtons);
    CRIT_EXIT(m);
    return v;
}

APP_HOT void hid_merge_on_tx_complete(void)
{
    uint32_t m = CRIT_ENTER();
    s_m.txBusy = false;
    CRIT_EXIT(m);
    try_send(); /* drain anything that accumulated while busy */
}
