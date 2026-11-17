/*
 * smooth_move.c - eased rendering of injected moves (see smooth_move.h).
 *
 * Velocity profile
 * ----------------
 * Position follows the smoothstep curve e(t) = 3t^2 - 2t^3. Its velocity
 * 6t(1-t) is zero at both ends and peaks mid-move: the motion accelerates,
 * then decelerates into the target instead of jumping there.
 *
 *     position e(t)          velocity e'(t)
 *     1 |        ___         1.5 |    _--_
 *       |      /             |   /      \
 *       |    /               |  /        \
 *     0 |___/                0 |/          \
 *       0     t     1          0    t     1
 *
 * TUNING
 *   - Duration per move: cmd.smooth(ms) at runtime (0 = off, the default);
 *     upper bound APP_SMOOTH_MAX_MS in app_config.h. 10-50 ms suits short
 *     moves; long moves look better with longer durations.
 *   - Tick rate: APP_SMOOTH_TICK_HZ (8000 = one step per HS microframe).
 *   - Curve shape: regenerate s_ease[] with another monotonic function that
 *     goes from 0 to 32768 (e.g. easeOutQuad 1-(1-t)^2 for a fast start).
 *     Keep 65 entries, first 0, last 32768: exactness relies on e(1) == 1.0.
 */
#include "smooth_move.h"
#include "app_config.h"
#include "hid_merge.h"

#include <stddef.h>

#ifdef UNIT_TEST
#define CRIT_ENTER()  (0U)
#define CRIT_EXIT(m)  ((void)(m))
#else
#include "fsl_common.h"
#define CRIT_ENTER()  DisableGlobalIRQ()
#define CRIT_EXIT(m)  EnableGlobalIRQ(m)
#endif

/* ------------------------------------------------------------------------- */
/* Easing table: smoothstep, Q15 (32768 = 1.0), 64 intervals                 */
/* Generated offline: round(32768 * (t^2 * (3 - 2t))), t = i/64              */
/* ------------------------------------------------------------------------- */
#define EASE_INTERVALS 64U
#define EASE_ONE       32768

static const uint16_t s_ease[EASE_INTERVALS + 1U] = {
    0,     24,    94,    209,   368,   569,   810,   1090,  1408,  1762,  2150,  2571,  3024,
    3507,  4018,  4556,  5120,  5708,  6318,  6949,  7600,  8269,  8954,  9654,  10368, 11094,
    11830, 12575, 13328, 14087, 14850, 15616, 16384, 17152, 17918, 18681, 19440, 20193, 20938,
    21674, 22400, 23114, 23814, 24499, 25168, 25819, 26450, 27060, 27648, 28212, 28750, 29261,
    29744, 30197, 30618, 31006, 31360, 31678, 31958, 32199, 32400, 32559, 32674, 32744, 32768,
};

/* e(t) for t in Q16 (0..65536), linear interpolation between table entries. */
static inline int32_t ease_q15(uint32_t tQ16)
{
    if (tQ16 >= 65536U)
    {
        return EASE_ONE;
    }
    uint32_t idx  = tQ16 >> 10;    /* 64 intervals -> 1024 Q16 units each */
    uint32_t frac = tQ16 & 1023U;
    int32_t a     = s_ease[idx];
    int32_t b     = s_ease[idx + 1U];
    return a + (int32_t)(((b - a) * (int32_t)frac) >> 10);
}

/* round(d * e / 32768), symmetric for negative d */
static inline int32_t scale_q15(int32_t d, int32_t eQ15)
{
    int64_t p = (int64_t)d * eQ15;
    return (int32_t)((p >= 0) ? ((p + 16384) >> 15) : -((-p + 16384) >> 15));
}

/* ------------------------------------------------------------------------- */
/* Segment FIFO (single producer: UART task; single consumer: tick)          */
/* ------------------------------------------------------------------------- */
typedef struct
{
    int32_t dx;
    int32_t dy;
    uint32_t ticks; /* total ticks for this segment (>= 1) */
} segment_t;

#define QUEUE_LEN 16U /* power of two */

typedef struct
{
    segment_t q[QUEUE_LEN];
    volatile uint32_t head; /* next write (producer) */
    volatile uint32_t tail; /* next read  (consumer) */
    /* active segment, owned by the tick context */
    bool active;
    segment_t cur;
    uint32_t k;     /* ticks done */
    int32_t sentX;  /* position already emitted */
    int32_t sentY;
    volatile uint32_t durationMs;
} smooth_state_t;

static smooth_state_t s_s;

static uint32_t ticks_for(uint32_t ms)
{
    uint32_t t = (ms * APP_SMOOTH_TICK_HZ) / 1000U;
    return (t == 0U) ? 1U : t;
}

void smooth_move_init(void)
{
    uint32_t m = CRIT_ENTER();
    s_s.head = s_s.tail = 0U;
    s_s.active          = false;
    s_s.durationMs      = 0U; /* OFF by default */
    CRIT_EXIT(m);
}

uint32_t smooth_move_get_duration(void)
{
    return s_s.durationMs;
}

bool smooth_move_busy(void)
{
    return s_s.active || (s_s.head != s_s.tail);
}

void smooth_move_start(int32_t dx, int32_t dy)
{
    uint32_t ms = s_s.durationMs;

    if (ms == 0U)
    {
        hid_merge_inject_move(dx, dy); /* smoothing off: unchanged behaviour */
        return;
    }
    if ((dx == 0) && (dy == 0))
    {
        return;
    }

    uint32_t m = CRIT_ENTER();
    uint32_t used = s_s.head - s_s.tail;
    if (used < QUEUE_LEN)
    {
        segment_t *seg = &s_s.q[s_s.head & (QUEUE_LEN - 1U)];
        seg->dx        = dx;
        seg->dy        = dy;
        seg->ticks     = ticks_for(ms);
        s_s.head++;
    }
    else
    {
        /* Queue full: extend the newest queued segment so no motion is lost.
         * (It has not started yet - the tick only reads at tail.) */
        segment_t *last = &s_s.q[(s_s.head - 1U) & (QUEUE_LEN - 1U)];
        last->dx += dx;
        last->dy += dy;
    }
    CRIT_EXIT(m);
}

void smooth_move_cancel(void)
{
    /* The tick runs in the PIT ISR; the lock keeps it from running while the
     * queue and the active segment are cleared. */
    uint32_t m = CRIT_ENTER();
    s_s.tail   = s_s.head;
    s_s.active = false;
    CRIT_EXIT(m);
}

APP_HOT void smooth_move_tick(int32_t *outDx, int32_t *outDy)
{
    int32_t sx = 0;
    int32_t sy = 0;

    if (!s_s.active)
    {
        uint32_t m = CRIT_ENTER();
        if (s_s.head != s_s.tail)
        {
            s_s.cur    = s_s.q[s_s.tail & (QUEUE_LEN - 1U)];
            s_s.tail++;
            s_s.active = true;
            s_s.k      = 0U;
            s_s.sentX  = 0;
            s_s.sentY  = 0;
        }
        CRIT_EXIT(m);
    }

    if (s_s.active)
    {
        s_s.k++;
        /* t = k/N in Q16; at k == N this is exactly 65536 -> e = 1.0 */
        uint32_t t = (uint32_t)(((uint64_t)s_s.k << 16) / s_s.cur.ticks);
        int32_t e  = ease_q15(t);
        int32_t px = scale_q15(s_s.cur.dx, e);
        int32_t py = scale_q15(s_s.cur.dy, e);

        sx        = px - s_s.sentX;
        sy        = py - s_s.sentY;
        s_s.sentX = px;
        s_s.sentY = py;

        if (s_s.k >= s_s.cur.ticks)
        {
            s_s.active = false; /* px/py == dx/dy here */
        }
        if ((sx != 0) || (sy != 0))
        {
            hid_merge_inject_move(sx, sy);
        }
    }

    if (outDx != NULL)
    {
        *outDx = sx;
    }
    if (outDy != NULL)
    {
        *outDy = sy;
    }
}

/* ------------------------------------------------------------------------- */
/* Tick source: PIT channel 0                                                */
/* ------------------------------------------------------------------------- */
#ifndef UNIT_TEST
#include "fsl_pit.h"
#include "fsl_clock.h"

static bool s_hwReady;

static void tick_timer_run(bool on)
{
    if (!s_hwReady)
    {
        return;
    }
    if (on)
    {
        PIT_StartTimer(PIT, kPIT_Chnl_0);
    }
    else
    {
        PIT_StopTimer(PIT, kPIT_Chnl_0);
    }
}

void smooth_move_hw_init(void)
{
    pit_config_t cfg;

    /* PERCLK = OSC 24 MHz / 1 (PERCLK_CLK_SEL: 0 = IPG, 1 = OSC). PERCLK also
     * clocks the GPTs; nothing else in this project uses them. */
    CLOCK_SetMux(kCLOCK_PerclkMux, 1U);
    CLOCK_SetDiv(kCLOCK_PerclkDiv, 0U);

    PIT_GetDefaultConfig(&cfg);
    PIT_Init(PIT, &cfg);
    /* 24 MHz / 8000 Hz = 3000 counts. PIT counts down to 0, so LDVAL = N-1. */
    PIT_SetTimerPeriod(PIT, kPIT_Chnl_0, (CLOCK_GetOscFreq() / APP_SMOOTH_TICK_HZ) - 1U);
    PIT_EnableInterrupts(PIT, kPIT_Chnl_0, kPIT_TimerInterruptEnable);

    /* Below both USB IRQs so the IN endpoint is always re-primed first, but
     * still >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY because the tick
     * reaches USB_DeviceHidSend() (OSA critical sections) via hid_merge. */
    NVIC_SetPriority(PIT_IRQn, APP_NVIC_PRIO_SMOOTH_TICK);
    EnableIRQ(PIT_IRQn);

    s_hwReady = true;
    tick_timer_run(s_s.durationMs != 0U);
}

APP_HOT void PIT_IRQHandler(void)
{
    PIT_ClearStatusFlags(PIT, kPIT_Chnl_0, kPIT_TimerFlag);
    smooth_move_tick(NULL, NULL); /* ~100 cycles when active, ~20 when idle */
    SDK_ISR_EXIT_BARRIER;
}
#else
void smooth_move_hw_init(void)
{
}
static void tick_timer_run(bool on)
{
    (void)on;
}
#endif

void smooth_move_set_duration(uint32_t ms)
{
    if (ms > APP_SMOOTH_MAX_MS)
    {
        ms = APP_SMOOTH_MAX_MS;
    }
    if (ms != 0U)
    {
        s_s.durationMs = ms; /* applies to moves queued from now on */
        tick_timer_run(true);
        return;
    }

    /* Turning off: stop the tick, then hand any remaining motion to
     * hid_merge in one go so nothing is lost. */
    tick_timer_run(false);
    uint32_t m    = CRIT_ENTER();
    int32_t restX = 0;
    int32_t restY = 0;
    if (s_s.active)
    {
        restX = s_s.cur.dx - s_s.sentX;
        restY = s_s.cur.dy - s_s.sentY;
        s_s.active = false;
    }
    while (s_s.head != s_s.tail)
    {
        segment_t *seg = &s_s.q[s_s.tail & (QUEUE_LEN - 1U)];
        restX += seg->dx;
        restY += seg->dy;
        s_s.tail++;
    }
    s_s.durationMs = 0U;
    CRIT_EXIT(m);

    if ((restX != 0) || (restY != 0))
    {
        hid_merge_inject_move(restX, restY);
    }
}
