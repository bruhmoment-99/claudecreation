/*
 * PC unit tests for the hardware-independent modules:
 *   hid_merge.c          (button locking, accumulation, splitting)
 *   hid_report_parser.c  (dynamic report layout discovery)
 *   uart_parser.c        (command grammar + response protocol)
 *   app_status.c         (status LED patterns, cmd.status() text)
 *
 * Build & run:  make -C test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_status.h"
#include "hid_merge.h"
#include "hid_report_parser.h"
#include "smooth_move.h"
#include "uart_parser.h"

static int s_fail;
#define CHECK(c)                                                         \
    do                                                                   \
    {                                                                    \
        if (!(c))                                                        \
        {                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            s_fail++;                                                    \
        }                                                                \
    } while (0)

/* ---- USB1 stub ---------------------------------------------------------- */
static hid_mouse_report_t s_sent[64];
static int s_nSent;
static long s_sumX, s_sumY; /* total motion delivered to "the PC" */

bool usb_device_send_report(const hid_mouse_report_t *r)
{
    s_sent[s_nSent++ % 64] = *r;
    s_sumX += r->x;
    s_sumY += r->y;
    return true;
}

static void reset_usb(void)
{
    s_nSent = 0;
    s_sumX = s_sumY = 0;
    hid_merge_init();
    smooth_move_init();
    hid_merge_set_device_ready(true);
}

/* ---- UART output capture ------------------------------------------------ */
static char s_out[512];
static size_t s_outLen;
static void cap_write(const char *d, size_t n)
{
    if (s_outLen + n >= sizeof(s_out))
    {
        abort();
    }
    memcpy(&s_out[s_outLen], d, n);
    s_outLen += n;
    s_out[s_outLen] = 0;
}
static int s_sleepCalls;
static uint32_t s_sleptMs;
static char s_outAtSleep[512]; /* output already written when sleep began */
static void cap_sleep(uint32_t ms)
{
    s_sleepCalls++;
    s_sleptMs = ms;
    memcpy(s_outAtSleep, s_out, s_outLen + 1);
}
static uint32_t s_nowMs;
static uint32_t cap_now(void)
{
    return s_nowMs;
}
static const cmd_io_t s_io = {cap_write, cap_sleep, cap_now};

static const char *run_cmd(const char *line)
{
    s_outLen = 0;
    s_out[0] = 0;
    cmd_execute_line(line, strlen(line), &s_io);
    return s_out;
}

/* ========================================================================= */
static void test_button_locking(void)
{
    printf("button locking\n");
    reset_usb();

    /* physical left press -> one report */
    hid_merge_physical(HID_BTN_LEFT, true, 0, 0, 0);
    CHECK(s_nSent == 1 && s_sent[0].buttons == HID_BTN_LEFT);
    hid_merge_on_tx_complete();

    /* injected left press while physical already pressed -> NO report */
    hid_merge_inject_button(HID_BTN_LEFT, true);
    CHECK(s_nSent == 1);

    /* physical release while injected held -> logical unchanged -> NO report */
    hid_merge_physical(0, true, 0, 0, 0);
    CHECK(s_nSent == 1);
    CHECK(hid_merge_get_logical_buttons() == HID_BTN_LEFT);

    /* injected release -> logical changes -> release report */
    hid_merge_inject_button(HID_BTN_LEFT, false);
    CHECK(s_nSent == 2 && s_sent[1].buttons == 0);
    hid_merge_on_tx_complete();

    /* injected right press with physical idle -> press report */
    hid_merge_inject_button(HID_BTN_RIGHT, true);
    CHECK(s_nSent == 3 && s_sent[2].buttons == HID_BTN_RIGHT);
    hid_merge_on_tx_complete();

    /* duplicate zero-motion physical reports are suppressed */
    hid_merge_physical(0, true, 0, 0, 0);
    CHECK(s_nSent == 3);
}

static void test_motion_merge(void)
{
    printf("motion merge / accumulation\n");
    reset_usb();

    /* endpoint busy: physical + injected deltas accumulate */
    hid_merge_physical(0, true, 5, 5, 0); /* sent, tx now busy */
    CHECK(s_nSent == 1 && s_sent[0].x == 5);
    hid_merge_inject_move(100, -50);
    hid_merge_physical(0, true, 3, 2, 1);
    CHECK(s_nSent == 1); /* still busy */
    hid_merge_on_tx_complete();
    CHECK(s_nSent == 2 && s_sent[1].x == 103 && s_sent[1].y == -48 && s_sent[1].wheel == 1);
    hid_merge_on_tx_complete();
    CHECK(s_nSent == 2); /* accumulator cleared: nothing repeats */

    /* large move split across reports, nothing lost */
    hid_merge_inject_move(70000, -40000);
    CHECK(s_sent[2].x == 32767 && s_sent[2].y == -32767);
    hid_merge_on_tx_complete();
    CHECK(s_sent[3].x == 32767 && s_sent[3].y == -7233);
    hid_merge_on_tx_complete();
    CHECK(s_sent[4].x == 4466 && s_sent[4].y == 0);
    hid_merge_on_tx_complete();
    CHECK(s_nSent == 5);

    /* wheel clamp to +-127 */
    hid_merge_inject_wheel(-200);
    CHECK(s_sent[5].wheel == -127);
    hid_merge_on_tx_complete();
    CHECK(s_sent[6].wheel == -73);
    hid_merge_on_tx_complete();

    /* not configured: motion discarded */
    hid_merge_set_device_ready(false);
    int before = s_nSent;
    hid_merge_inject_move(10, 10);
    CHECK(s_nSent == before);
    hid_merge_set_device_ready(true);
    CHECK(s_nSent == before);
}

/* ========================================================================= */
static void test_parser_simple(void)
{
    printf("report parser: classic 3-byte + wheel mouse\n");
    static const uint8_t d[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
        0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
        0x81, 0x06, 0xC0, 0xC0};
    hid_mouse_layout_t l;
    CHECK(hid_parse_mouse_layout(d, sizeof(d), &l));
    CHECK(!l.usesReportId && l.buttons.mask == 0x07);
    CHECK(l.x.bitOffset == 8 && l.x.bitSize == 8 && l.x.isSigned);
    CHECK(l.y.bitOffset == 16 && l.wheel.bitOffset == 24);

    uint8_t r[] = {0x05, 0xFE, 0x03, 0xFF};
    hid_mouse_sample_t s;
    CHECK(hid_extract_mouse(&l, r, sizeof(r), &s));
    CHECK(s.buttons == 0x05 && s.x == -2 && s.y == 3 && s.wheel == -1 && s.buttonsValid);
}

static void test_parser_gaming(void)
{
    printf("report parser: report ID, 16 buttons, 12-bit packed X/Y, extra keyboard collection\n");
    static const uint8_t d[] = {
        /* keyboard collection first (report ID 1) - must be ignored */
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
        0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0,
        /* mouse collection (report ID 2) */
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
        0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31,
        0x81, 0x06,
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
        0xC0, 0xC0};
    hid_mouse_layout_t l;
    CHECK(hid_parse_mouse_layout(d, sizeof(d), &l));
    CHECK(l.usesReportId && l.buttons.reportId == 2 && l.buttons.mask == 0xFF);
    CHECK(l.x.reportId == 2 && l.x.bitOffset == 16 && l.x.bitSize == 12);
    CHECK(l.y.bitOffset == 28 && l.wheel.bitOffset == 40);

    /* X = -3 (0xFFD), Y = +2047 (0x7FF), buttons 1+2, wheel +1 */
    uint8_t r[] = {0x02, 0x03, 0x00, 0xFD, 0xFF, 0x7F, 0x01};
    hid_mouse_sample_t s;
    CHECK(hid_extract_mouse(&l, r, sizeof(r), &s));
    CHECK(s.buttons == 0x03 && s.x == -3 && s.y == 2047 && s.wheel == 1);

    /* keyboard report (ID 1) -> no mouse fields */
    uint8_t k[] = {0x01, 0x02};
    CHECK(!hid_extract_mouse(&l, k, sizeof(k), &s));
}

static void test_parser_reject(void)
{
    printf("report parser: keyboard-only descriptor rejected\n");
    static const uint8_t d[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
                                0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
    hid_mouse_layout_t l;
    CHECK(!hid_parse_mouse_layout(d, sizeof(d), &l));
}

/* ========================================================================= */
static void test_commands(void)
{
    printf("command parser: original commands\n");
    reset_usb();

    CHECK(strcmp(run_cmd("cmd.move(10,-5)"), "cmd.move(10,-5)\r\n>>> ") == 0);
    CHECK(s_nSent == 1 && s_sent[0].x == 10 && s_sent[0].y == -5);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.move( 3 , 4 )"), "cmd.move( 3 , 4 )\r\n>>> ") == 0);
    hid_merge_on_tx_complete();

    CHECK(strcmp(run_cmd("cmd.left()"), "cmd.left()\r\n0\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.left(1)"), "cmd.left(1)\r\n>>> ") == 0);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.left()"), "cmd.left()\r\n1\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.left(0)"), "cmd.left(0)\r\n>>> ") == 0);
    hid_merge_on_tx_complete();

    hid_merge_physical(HID_BTN_MIDDLE, true, 0, 0, 0);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.middle()"), "cmd.middle()\r\n1\r\n>>> ") == 0);
    hid_merge_physical(0, true, 0, 0, 0);
    hid_merge_on_tx_complete();

    CHECK(strcmp(run_cmd("cmd.wheel(-2)"), "cmd.wheel(-2)\r\n>>> ") == 0);
    hid_merge_on_tx_complete();
}

static void test_side_buttons(void)
{
    printf("command parser: side buttons + locking\n");
    reset_usb();

    CHECK(strcmp(run_cmd("cmd.side1(1)"), "cmd.side1(1)\r\n>>> ") == 0);
    CHECK(s_nSent == 1 && s_sent[0].buttons == HID_BTN_SIDE1); /* bit 3 = button 4 */
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.side2(1)"), "cmd.side2(1)\r\n>>> ") == 0);
    CHECK(s_nSent == 2 && s_sent[1].buttons == (HID_BTN_SIDE1 | HID_BTN_SIDE2));
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.side2()"), "cmd.side2()\r\n1\r\n>>> ") == 0);

    /* physical side1 already held + injected side1 -> no duplicate report */
    run_cmd("cmd.release()");
    hid_merge_on_tx_complete();
    hid_merge_physical(HID_BTN_SIDE1, true, 0, 0, 0);
    hid_merge_on_tx_complete();
    int n = s_nSent;
    CHECK(strcmp(run_cmd("cmd.side1(1)"), "cmd.side1(1)\r\n>>> ") == 0);
    CHECK(s_nSent == n);
    CHECK(strcmp(run_cmd("cmd.side1(0)"), "cmd.side1(0)\r\n>>> ") == 0);
    CHECK(s_nSent == n); /* physical still holds it */
    hid_merge_physical(0, true, 0, 0, 0);
    CHECK(s_nSent == n + 1 && s_sent[n].buttons == 0);
    hid_merge_on_tx_complete();
}

static void test_state_commands(void)
{
    printf("command parser: buttons(mask) / release()\n");
    reset_usb();

    CHECK(strcmp(run_cmd("cmd.buttons(19)"), "cmd.buttons(19)\r\n>>> ") == 0); /* L+R+side2 */
    CHECK(s_nSent == 1 && s_sent[0].buttons == 19);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.buttons()"), "cmd.buttons()\r\n19\r\n>>> ") == 0);

    /* same mask again -> logical unchanged -> no report */
    run_cmd("cmd.buttons(19)");
    CHECK(s_nSent == 1);

    /* physical left held; release() clears injected only */
    hid_merge_physical(HID_BTN_LEFT, true, 0, 0, 0);
    CHECK(s_nSent == 1); /* left already logically down */
    CHECK(strcmp(run_cmd("cmd.release()"), "cmd.release()\r\n>>> ") == 0);
    CHECK(s_nSent == 2 && s_sent[1].buttons == HID_BTN_LEFT);
    hid_merge_on_tx_complete();

    /* release with nothing injected -> no report */
    run_cmd("cmd.release()");
    CHECK(s_nSent == 2);

    CHECK(strcmp(run_cmd("cmd.buttons(255)"), "cmd.buttons(255)\r\n>>> ") == 0);
    CHECK(s_sent[2].buttons == 255);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.buttons(0)"), "cmd.buttons(0)\r\n>>> ") == 0);
    CHECK(s_sent[3].buttons == HID_BTN_LEFT);
    hid_merge_on_tx_complete();
}

static void test_utility_commands(void)
{
    printf("command parser: sleep / version / info\n");
    reset_usb();

    CHECK(strcmp(run_cmd("cmd.version()"), "cmd.version()\r\n1.0.0\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.info()"), "cmd.info()\r\nHID-Proxy-RT1062\r\n>>> ") == 0);

    /* sleep: prompt must be fully written BEFORE the sleep starts */
    s_sleepCalls = 0;
    CHECK(strcmp(run_cmd("cmd.sleep(250)"), "cmd.sleep(250)\r\n>>> ") == 0);
    CHECK(s_sleepCalls == 1 && s_sleptMs == 250);
    CHECK(strcmp(s_outAtSleep, "cmd.sleep(250)\r\n>>> ") == 0);

    s_sleepCalls = 0;
    run_cmd("cmd.sleep(0)");
    CHECK(s_sleepCalls == 0);

    /* streaming: commands after a sleep run after it, in order */
    cmd_parser_t p;
    cmd_parser_init(&p, &s_io);
    s_outLen     = 0;
    s_sleepCalls = 0;
    const char *in = "cmd.left(1)\r\ncmd.sleep(5)\r\ncmd.left(0)\r\n";
    cmd_parser_feed(&p, (const uint8_t *)in, strlen(in));
    CHECK(s_sleepCalls == 1);
    CHECK(strcmp(s_outAtSleep, "cmd.left(1)\r\n>>> cmd.sleep(5)\r\n>>> ") == 0);
    CHECK(strcmp(s_out, "cmd.left(1)\r\n>>> cmd.sleep(5)\r\n>>> cmd.left(0)\r\n>>> ") == 0);
}

static void test_silent_ignore(void)
{
    printf("command parser: invalid input is silently ignored\n");
    reset_usb();

    static const char *bad[] = {
        "cmd.left(2)",    "cmd.left(1,1)",  "cmd.side1(-1)",   "cmd.move(1)",       "cmd.move(1,2,3)",
        "cmd.move(a,2)",  "cmd.jump(1)",    "hello",           "cmd.buttons(256)",  "cmd.buttons(-1)",
        "cmd.release(1)", "cmd.sleep()",    "cmd.sleep(-5)",   "cmd.sleep(999999999)", "cmd.version(1)",
        "cmd.info(x)",    "cmd.left",       "cmd.left(1",      "cmd.left((1))",     "CMD.left(1)",
        "cmd.left(1)x",   "cmd.wheel()",    "cmd.move(,)",     "cmd.move(1,)",      "",
    };
    s_sleepCalls = 0;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        s_outLen = 0;
        s_out[0] = 0;
        int ok   = cmd_execute_line(bad[i], strlen(bad[i]), &s_io);
        if (ok != 0 || s_outLen != 0)
        {
            printf("  not ignored: \"%s\"\n", bad[i]);
        }
        CHECK(ok == 0);
        CHECK(s_outLen == 0);
    }
    CHECK(s_nSent == 0);      /* and no side effects */
    CHECK(s_sleepCalls == 0);
    CHECK(hid_merge_get_logical_buttons() == 0);

    /* streaming: overlong line and garbage produce no output at all */
    cmd_parser_t p;
    cmd_parser_init(&p, &s_io);
    s_outLen = 0;
    char longl[200];
    memset(longl, 'x', sizeof(longl));
    cmd_parser_feed(&p, (const uint8_t *)longl, sizeof(longl));
    cmd_parser_feed(&p, (const uint8_t *)"\r\n\r\ngarbage\r\n", 13);
    CHECK(s_outLen == 0);

    /* framing still works afterwards, split across chunks */
    const char *a = "cmd.mo";
    const char *b = "ve(1,1)\r\ncmd.left()\r\n";
    cmd_parser_feed(&p, (const uint8_t *)a, strlen(a));
    cmd_parser_feed(&p, (const uint8_t *)b, strlen(b));
    CHECK(strcmp(s_out, "cmd.move(1,1)\r\n>>> cmd.left()\r\n0\r\n>>> ") == 0);
}

/* Run n ticks; record per-tick steps; complete each USB transfer. */
static void run_ticks(int n, long *sx, long *sy, int32_t *stepsX)
{
    for (int i = 0; i < n; i++)
    {
        int32_t dx, dy;
        smooth_move_tick(&dx, &dy);
        if (stepsX != NULL)
        {
            stepsX[i] = dx;
        }
        *sx += dx;
        *sy += dy;
        hid_merge_on_tx_complete();
    }
}

static void test_smooth_move(void)
{
    printf("smooth move: off by default, exact totals, eased profile\n");
    reset_usb();

    /* default off: instant, unchanged behaviour */
    CHECK(smooth_move_get_duration() == 0);
    CHECK(strcmp(run_cmd("cmd.smooth()"), "cmd.smooth()\r\n0\r\n>>> ") == 0);
    run_cmd("cmd.move(10,0)");
    CHECK(s_nSent == 1 && s_sent[0].x == 10);
    hid_merge_on_tx_complete();
    int32_t dx, dy;
    smooth_move_tick(&dx, &dy);
    CHECK(dx == 0 && dy == 0); /* idle tick does nothing */

    /* 10 ms @ 8 kHz = 80 ticks */
    CHECK(strcmp(run_cmd("cmd.smooth(10)"), "cmd.smooth(10)\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.smooth()"), "cmd.smooth()\r\n10\r\n>>> ") == 0);
    reset_usb();
    smooth_move_set_duration(10);
    int n0 = s_nSent;
    run_cmd("cmd.move(1000,-370)");
    CHECK(s_nSent == n0); /* nothing sent until ticks run */
    CHECK(smooth_move_busy());

    static int32_t steps[200];
    long sx = 0, sy = 0;
    run_ticks(80, &sx, &sy, steps);
    CHECK(sx == 1000 && sy == -370);
    CHECK(s_sumX == 1000 && s_sumY == -370);
    CHECK(!smooth_move_busy());

    /* accelerate then decelerate: ends slow, middle fast, never backwards */
    int32_t maxStep = 0;
    int maxAt       = 0;
    for (int i = 0; i < 80; i++)
    {
        CHECK(steps[i] >= 0);
        if (steps[i] > maxStep)
        {
            maxStep = steps[i];
            maxAt   = i;
        }
    }
    CHECK(steps[0] <= 2 && steps[79] <= 2);
    CHECK(maxAt >= 30 && maxAt <= 50);
    CHECK(maxStep >= 15 && maxStep <= 25); /* peak ~1.5x average (12.5) */

    long zx = 0, zy = 0;
    run_ticks(10, &zx, &zy, NULL);
    CHECK(zx == 0 && zy == 0); /* finished: back to passthrough */

    /* tiny and huge moves stay exact */
    sx = sy = 0;
    smooth_move_start(1, -1);
    run_ticks(80, &sx, &sy, NULL);
    CHECK(sx == 1 && sy == -1);
    smooth_move_set_duration(1); /* 8 ticks */
    sx = sy = 0;
    smooth_move_start(70000, -3);
    run_ticks(8, &sx, &sy, NULL);
    CHECK(sx == 70000 && sy == -3);

    /* sequential moves run back to back */
    smooth_move_set_duration(5); /* 40 ticks */
    sx = sy = 0;
    smooth_move_start(100, 0);
    smooth_move_start(-30, 50);
    run_ticks(40, &sx, &sy, NULL);
    CHECK(sx == 100 && sy == 0);
    run_ticks(40, &sx, &sy, NULL);
    CHECK(sx == 70 && sy == 50 && !smooth_move_busy());

    /* queue overflow merges instead of dropping */
    sx = sy = 0;
    for (int i = 0; i < 40; i++)
    {
        smooth_move_start(3, 1);
    }
    run_ticks(40 * 40, &sx, &sy, NULL);
    CHECK(sx == 120 && sy == 40 && !smooth_move_busy());
}

static void test_smooth_disable_flush(void)
{
    printf("smooth move: disabling mid-move flushes the remainder\n");
    reset_usb();
    smooth_move_set_duration(10);
    smooth_move_start(1000, 200);
    smooth_move_start(50, 0);
    long sx = 0, sy = 0;
    run_ticks(20, &sx, &sy, NULL);
    CHECK(sx > 0 && sx < 1000);
    run_cmd("cmd.smooth(0)");
    for (int i = 0; i < 10; i++)
    {
        hid_merge_on_tx_complete();
    }
    CHECK(s_sumX == 1050 && s_sumY == 200);
    CHECK(!smooth_move_busy());

    /* out of range -> silently ignored, setting unchanged */
    s_outLen = 0;
    CHECK(cmd_execute_line("cmd.smooth(1001)", 16, &s_io) == 0 && s_outLen == 0);
    CHECK(cmd_execute_line("cmd.smooth(-1)", 14, &s_io) == 0);
    CHECK(smooth_move_get_duration() == 0);
}

/* The gaming mouse descriptor from test_parser_gaming(). */
static const uint8_t s_gamingDesc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31,
    0x81, 0x06,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
    0xC0, 0xC0};

static void test_status_command(void)
{
    printf("cmd.status(): links, mouse, layout, faults\n");
    reset_usb();
    app_status_init();
    s_nowMs = 1000U;

    /* fresh boot: nothing connected */
    CHECK(strcmp(run_cmd("cmd.status()"), "cmd.status()\r\n"
                                          "pc: not-configured reports=0\r\n"
                                          "mouse: none\r\n"
                                          "layout: -\r\n"
                                          "faults: none\r\n>>> ") == 0);

    /* PC enumerated us at high speed, a full-speed gaming mouse is streaming */
    app_status_pc_speed(APP_USB_SPEED_HIGH);
    app_status_pc_configured(true);
    app_status_pc_report_sent();
    app_status_pc_report_sent();
    app_status_mouse_attached(0x046DU, 0xC08BU, APP_USB_SPEED_FULL, 1U);
    CHECK(strstr(run_cmd("cmd.status()"), "mouse: enumerating full-speed vid=046d pid=c08b interface=1\r\n"
                                          "layout: -\r\n") != NULL);
    hid_mouse_layout_t l;
    CHECK(hid_parse_mouse_layout(s_gamingDesc, sizeof(s_gamingDesc), &l));
    app_status_mouse_ready(&l, false, 8U);
    app_status_mouse_report();
    app_status_mouse_report();
    app_status_mouse_report();
    CHECK(strcmp(run_cmd("cmd.status()"),
                 "cmd.status()\r\n"
                 "pc: configured high-speed reports=2\r\n"
                 "mouse: ready full-speed vid=046d pid=c08b interface=1 packet=8 reports=3\r\n"
                 "layout: report-id=2 buttons=8(0xff)@0 x=s12@16 y=s12@28 wheel=s8@40\r\n"
                 "faults: none\r\n>>> ") == 0);

    /* boot-protocol fallback layout, no report IDs, no wheel */
    hid_mouse_layout_boot(&l);
    app_status_mouse_ready(&l, true, 4U);
    CHECK(strstr(run_cmd("cmd.status()"), "layout: boot buttons=3(0x07)@0 x=s8@8 y=s8@16 wheel=none\r\n") !=
          NULL);

    /* failures */
    app_status_mouse_error("no mouse collection in report descriptor");
    CHECK(strstr(run_cmd("cmd.status()"),
                 "mouse: error (no mouse collection in report descriptor) full-speed vid=046d pid=c08b interface=1\r\n"
                 "layout: -\r\n"
                 "faults: mouse-error\r\n") != NULL);
    app_status_mouse_detached();
    app_status_mouse_unsupported(0x1234U, 0xABCDU, APP_USB_SPEED_HIGH);
    app_status_vbus_fault(500U);
    app_status_vbus_fault(900U);
    CHECK(strstr(run_cmd("cmd.status()"), "mouse: unsupported high-speed vid=1234 pid=abcd\r\n"
                                          "layout: -\r\n"
                                          "faults: vbus-overcurrent=2(active) unsupported-device\r\n>>> ") != NULL);
    s_nowMs = 900U + APP_STATUS_VBUS_FAULT_HOLD_MS;
    app_status_mouse_detached();
    app_status_host_init_failed();
    CHECK(strstr(run_cmd("cmd.status()"), "faults: host-init-failed vbus-overcurrent=2\r\n>>> ") != NULL);

    /* takes no arguments; no clock is fine */
    s_outLen = 0;
    CHECK(cmd_execute_line("cmd.status(1)", 13, &s_io) == 0 && s_outLen == 0);
    static const cmd_io_t noClock = {cap_write, cap_sleep, NULL};
    s_outLen = 0;
    CHECK(cmd_execute_line("cmd.status()", 12, &noClock) == 1);
    app_status_init();
}

/* Sample the LED every 10 ms over 2 s from t0; count lit samples and
 * off->on edges. */
static void led_profile(const app_status_t *st, uint32_t t0, int *lit, int *edges)
{
    bool prev = app_status_led_on(st, t0 - 10U);
    *lit      = 0;
    *edges    = 0;
    for (uint32_t t = t0; t < t0 + 2000U; t += 10U)
    {
        bool on = app_status_led_on(st, t);
        *lit += on ? 1 : 0;
        *edges += (on && !prev) ? 1 : 0;
        prev = on;
    }
}

static void test_status_led(void)
{
    printf("status LED patterns\n");
    app_status_init();
    const app_status_t *st = app_status_get();
    int lit, edges;
    hid_mouse_layout_t l;
    hid_mouse_layout_boot(&l);

    /* idle: one short blip per 2 s */
    led_profile(st, 10000U, &lit, &edges);
    CHECK(edges == 1 && lit == 6);

    /* mouse but no PC: two blips */
    app_status_mouse_attached(1U, 2U, APP_USB_SPEED_FULL, 0U);
    app_status_mouse_ready(&l, true, 4U);
    led_profile(st, 10000U, &lit, &edges);
    CHECK(edges == 2 && lit == 12);

    /* PC + mouse: solid */
    app_status_pc_configured(true);
    led_profile(st, 10000U, &lit, &edges);
    CHECK(edges == 0 && lit == 200);

    /* PC, no mouse: 1 Hz */
    app_status_mouse_detached();
    led_profile(st, 10000U, &lit, &edges);
    CHECK(edges == 2 && lit == 100);
    CHECK(!app_status_has_fault(st, 10000U));

    /* VBUS fault: 5 Hz for the hold time, then back to 1 Hz */
    app_status_vbus_fault(10000U);
    CHECK(app_status_has_fault(st, 10000U));
    led_profile(st, 10000U, &lit, &edges);
    CHECK(edges == 10 && lit == 100);
    CHECK(!app_status_has_fault(st, 10000U + APP_STATUS_VBUS_FAULT_HOLD_MS));
    led_profile(st, 10000U + APP_STATUS_VBUS_FAULT_HOLD_MS, &lit, &edges);
    CHECK(edges == 2);

    /* fault time near the 32-bit wrap */
    app_status_vbus_fault(0xFFFFFF00U);
    CHECK(app_status_has_fault(st, 0x00000100U));

    /* mouse errors are faults too */
    app_status_init();
    app_status_mouse_error("x");
    CHECK(app_status_has_fault(st, 0U));
    app_status_mouse_unsupported(1U, 2U, APP_USB_SPEED_LOW);
    CHECK(app_status_has_fault(st, 0U));
    app_status_mouse_attached(1U, 2U, APP_USB_SPEED_LOW, 0U); /* next device clears it */
    CHECK(!app_status_has_fault(st, 0U));
    app_status_init();
}

int main(void)
{
    test_button_locking();
    test_motion_merge();
    test_parser_simple();
    test_parser_gaming();
    test_parser_reject();
    test_commands();
    test_side_buttons();
    test_state_commands();
    test_utility_commands();
    test_silent_ignore();
    test_smooth_move();
    test_smooth_disable_flush();
    test_status_command();
    test_status_led();
    if (s_fail)
    {
        printf("%d check(s) FAILED\n", s_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
