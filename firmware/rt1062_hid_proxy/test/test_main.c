/*
 * PC unit tests for the hardware-independent modules:
 *   hid_merge.c          (button locking, accumulation, splitting)
 *   hid_report_parser.c  (dynamic report layout discovery)
 *   uart_parser.c        (command grammar + response protocol)
 *   app_status.c         (status LED patterns, cmd.status() text)
 *   settings.c, fw_update.c, crc32.c, watch.c (against a RAM "flash")
 *
 * Build & run:  make -C test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_status.h"
#include "crc32.h"
#include "fw_update.h"
#include "settings.h"
#include "watch.h"
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

static int s_wakeups;
void usb_device_request_wakeup(void)
{
    s_wakeups++;
}

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
static char s_out[2048];
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
static char s_outAtSleep[2048]; /* output already written when sleep began */
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
static uint32_t s_updSize, s_updCrc;
static int s_updCalls;
static void cap_update(uint32_t size, uint32_t crc)
{
    s_updCalls++;
    s_updSize = size;
    s_updCrc  = crc;
}
static const cmd_io_t s_io = {cap_write, cap_sleep, cap_now, cap_update};

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
    hid_merge_physical(HID_BTN_LEFT, 0xFFFFU, 0, 0, 0, 0);
    CHECK(s_nSent == 1 && s_sent[0].buttons == HID_BTN_LEFT);
    hid_merge_on_tx_complete();

    /* injected left press while physical already pressed -> NO report */
    hid_merge_inject_button(HID_BTN_LEFT, true);
    CHECK(s_nSent == 1);

    /* physical release while injected held -> logical unchanged -> NO report */
    hid_merge_physical(0, 0xFFFFU, 0, 0, 0, 0);
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
    hid_merge_physical(0, 0xFFFFU, 0, 0, 0, 0);
    CHECK(s_nSent == 3);
}

static void test_motion_merge(void)
{
    printf("motion merge / accumulation\n");
    reset_usb();

    /* endpoint busy: physical + injected deltas accumulate */
    hid_merge_physical(0, 0xFFFFU, 5, 5, 0, 0); /* sent, tx now busy */
    CHECK(s_nSent == 1 && s_sent[0].x == 5);
    hid_merge_inject_move(100, -50);
    hid_merge_physical(0, 0xFFFFU, 3, 2, 1, 0);
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
    CHECK(s.buttons == 0x05 && s.x == -2 && s.y == 3 && s.wheel == -1 && s.buttonsMask != 0U);
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
    CHECK(l.usesReportId && l.buttons.reportId[0] == 2 && l.buttons.mask == 0xFFFF);
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

    hid_merge_physical(HID_BTN_MIDDLE, 0xFFFFU, 0, 0, 0, 0);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.middle()"), "cmd.middle()\r\n1\r\n>>> ") == 0);
    hid_merge_physical(0, 0xFFFFU, 0, 0, 0, 0);
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
    hid_merge_physical(HID_BTN_SIDE1, 0xFFFFU, 0, 0, 0, 0);
    hid_merge_on_tx_complete();
    int n = s_nSent;
    CHECK(strcmp(run_cmd("cmd.side1(1)"), "cmd.side1(1)\r\n>>> ") == 0);
    CHECK(s_nSent == n);
    CHECK(strcmp(run_cmd("cmd.side1(0)"), "cmd.side1(0)\r\n>>> ") == 0);
    CHECK(s_nSent == n); /* physical still holds it */
    hid_merge_physical(0, 0xFFFFU, 0, 0, 0, 0);
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
    hid_merge_physical(HID_BTN_LEFT, 0xFFFFU, 0, 0, 0, 0);
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

    CHECK(strcmp(run_cmd("cmd.version()"), "cmd.version()\r\n" APP_FW_VERSION "\r\n>>> ") == 0);
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
        "cmd.move(a,2)",  "cmd.jump(1)",    "hello",           "cmd.buttons(65536)",  "cmd.buttons(-1)",
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
                                          "uart: rx=0 overflows=0 lost=0\r\n"
                                          "settings: smooth=0 saves=0\r\n"
                                          "reset: unknown\r\n"
                                          "faults: none\r\n>>> ") == 0);
    app_status_reset("power-on", 0U, NULL);

    /* PC enumerated us at high speed, a full-speed gaming mouse is streaming */
    app_status_pc_speed(APP_USB_SPEED_HIGH);
    app_status_pc_configured(true);
    app_status_pc_report_sent();
    app_status_pc_report_sent();
    app_status_mouse_attached(0x046DU, 0xC08BU, APP_USB_SPEED_FULL, 1U, false);
    CHECK(strstr(run_cmd("cmd.status()"), "mouse: enumerating full-speed vid=046d pid=c08b interface=1\r\n"
                                          "layout: -\r\n") != NULL);
    hid_mouse_layout_t l;
    CHECK(hid_parse_mouse_layout(s_gamingDesc, sizeof(s_gamingDesc), &l));
    app_status_mouse_ready(&l, false, 8U);
    app_status_mouse_report();
    app_status_mouse_report();
    app_status_mouse_report();
    app_status_uart(1234U, 2U, 50U, 0U);
    app_status_settings(40U, 3U, 0U);
    CHECK(strcmp(run_cmd("cmd.status()"),
                 "cmd.status()\r\n"
                 "pc: configured high-speed reports=2\r\n"
                 "mouse: ready full-speed vid=046d pid=c08b interface=1 packet=8 reports=3\r\n"
                 "layout: report-id=2 buttons=16(0xffff)@0 x=s12@16 y=s12@28 wheel=s8@40 pan=none\r\n"
                 "uart: rx=1234 overflows=2 lost=50\r\n"
                 "settings: smooth=40 saves=3\r\n"
                 "reset: power-on\r\n"
                 "faults: none\r\n>>> ") == 0);

    /* PC asleep after selecting boot protocol and allowing wake-up; hub */
    app_status_pc_boot_protocol(true);
    app_status_pc_suspended(true);
    app_status_pc_remote_wake(true);
    app_status_pc_wakeup();
    app_status_mouse_attached(0x046DU, 0xC08BU, APP_USB_SPEED_FULL, 2U, true);
    app_status_reset("watchdog", 0xFA000002U, "uart_cmd");
    run_cmd("cmd.status()");
    CHECK(strstr(s_out, "pc: configured high-speed reports=2 boot-protocol suspended remote-wake wakeups=1\r\n"));
    CHECK(strstr(s_out, "mouse: enumerating full-speed vid=046d pid=c08b interface=2 via-hub\r\n"));
    CHECK(strstr(s_out, "reset: watchdog (task uart_cmd stopped responding) after-fatal=0xfa000002\r\n"));
    app_status_pc_boot_protocol(false);
    app_status_pc_suspended(false);
    app_status_pc_remote_wake(false);

    /* boot-protocol fallback layout, no report IDs, no wheel */
    hid_mouse_layout_boot(&l);
    app_status_mouse_ready(&l, true, 4U);
    CHECK(strstr(run_cmd("cmd.status()"), "layout: boot buttons=3(0x0007)@0 x=s8@8 y=s8@16 wheel=none pan=none\r\n") !=
          NULL);

    /* failures */
    app_status_mouse_error("no mouse collection in report descriptor");
    CHECK(strstr(run_cmd("cmd.status()"),
                 "mouse: error (no mouse collection in report descriptor) full-speed vid=046d pid=c08b interface=2 "
                 "via-hub\r\nlayout: -\r\n") != NULL);
    CHECK(strstr(s_out, "faults: mouse-error\r\n") != NULL);
    app_status_mouse_detached();
    app_status_mouse_unsupported(0x1234U, 0xABCDU, APP_USB_SPEED_HIGH);
    app_status_vbus_fault(500U);
    app_status_vbus_fault(900U);
    CHECK(strstr(run_cmd("cmd.status()"), "mouse: unsupported high-speed vid=1234 pid=abcd\r\n"
                                          "layout: -\r\n") != NULL);
    CHECK(strstr(s_out, "faults: vbus-overcurrent=2(active) unsupported-device\r\n>>> ") != NULL);
    s_nowMs = 900U + APP_STATUS_VBUS_FAULT_HOLD_MS;
    app_status_mouse_detached();
    app_status_host_init_failed();
    CHECK(strstr(run_cmd("cmd.status()"), "faults: host-init-failed vbus-overcurrent=2\r\n>>> ") != NULL);

    /* takes no arguments; no clock is fine */
    s_outLen = 0;
    CHECK(cmd_execute_line("cmd.status(1)", 13, &s_io) == 0 && s_outLen == 0);
    static const cmd_io_t noClock = {cap_write, cap_sleep, NULL, NULL};
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
    app_status_mouse_attached(1U, 2U, APP_USB_SPEED_FULL, 0U, false);
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
    app_status_mouse_attached(1U, 2U, APP_USB_SPEED_LOW, 0U, false); /* next device clears it */
    CHECK(!app_status_has_fault(st, 0U));
    app_status_init();
}

/* ========================================================================= */
/* Items added with the robustness / compatibility / command-channel work    */
/* ========================================================================= */

static void test_parser_arrays_pan_split(void)
{
    printf("report parser: button array, buttons split over report IDs, AC Pan\n");
    static const uint8_t d[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,             /* Mouse application     */
        /* report 1: 3 bit buttons, X/Y 8-bit, wheel, AC Pan */
        0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
        0x95, 0x05, 0x81, 0x03,                         /* padding               */
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
        0x81, 0x06,
        0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, /* AC Pan s8         */
        0xC0,
        /* report 3: extra buttons 4..12 as an array of two 8-bit slots,
         * value 0 = none, 1 = button 4, ... */
        0x85, 0x03, 0x05, 0x09, 0x19, 0x04, 0x29, 0x0C, 0x15, 0x01, 0x25, 0x09, 0x75, 0x08, 0x95, 0x02,
        0x81, 0x00,                                     /* Input (Data,Array)    */
        0xC0};
    hid_mouse_layout_t l;
    CHECK(hid_parse_mouse_layout(d, sizeof(d), &l));
    CHECK(l.buttons.mask == 0x0007U && l.buttons.reportId[0] == 1);
    CHECK(l.arrays[0].present && l.arrays[0].reportId == 3 && l.arrays[0].count == 2);
    CHECK(l.arrays[0].usageMin == 4 && l.arrays[0].usageMax == 12);
    CHECK(hid_button_array_mask(&l.arrays[0]) == 0x0FF8U); /* usage 4..12 */
    CHECK(l.pan.present && l.pan.reportId == 1 && l.pan.bitOffset == 32 && l.pan.isSigned);

    hid_mouse_sample_t s;
    uint8_t r1[] = {0x01, 0x05, 0x02, 0xFE, 0x01, 0xFF};
    CHECK(hid_extract_mouse(&l, r1, sizeof(r1), &s));
    CHECK(s.buttonsMask == 0x0007U && s.buttons == 0x0005U);
    CHECK(s.x == 2 && s.y == -2 && s.wheel == 1 && s.pan == -1);

    /* array: logical 1 -> usage 4 (button 4), logical 9 -> usage 12 */
    uint8_t r3[] = {0x03, 0x01, 0x09};
    CHECK(hid_extract_mouse(&l, r3, sizeof(r3), &s));
    CHECK(s.buttonsMask == 0x0FF8U && s.buttons == ((1U << 3) | (1U << 11)));
    CHECK(s.x == 0 && s.y == 0);
    uint8_t r3none[] = {0x03, 0x00, 0x00};
    CHECK(hid_extract_mouse(&l, r3none, sizeof(r3none), &s) && s.buttons == 0U);
}

static void test_merge_split_buttons_pan(void)
{
    printf("merge: buttons from several reports, pan, 16 buttons\n");
    reset_usb();
    /* report A carries buttons 1-3, report B carries 4-16 */
    hid_merge_physical(0x0001U, 0x0007U, 0, 0, 0, 0);
    CHECK(s_nSent == 1 && s_sent[0].buttons == 0x0001U);
    hid_merge_on_tx_complete();
    hid_merge_physical(0x0800U, 0xFFF8U, 0, 0, 0, 0); /* button 12 */
    CHECK(s_nSent == 2 && s_sent[1].buttons == 0x0801U); /* button 1 kept */
    hid_merge_on_tx_complete();
    hid_merge_physical(0x0000U, 0x0007U, 0, 0, 0, 3);
    CHECK(s_nSent == 3 && s_sent[2].buttons == 0x0800U && s_sent[2].pan == 3);
    hid_merge_on_tx_complete();

    CHECK(strcmp(run_cmd("cmd.pan(-200)"), "cmd.pan(-200)\r\n>>> ") == 0);
    CHECK(s_sent[3].pan == -127);
    hid_merge_on_tx_complete();
    CHECK(s_sent[4].pan == -73);
    hid_merge_on_tx_complete();

    CHECK(strcmp(run_cmd("cmd.button(16,1)"), "cmd.button(16,1)\r\n>>> ") == 0);
    CHECK(s_sent[5].buttons == 0x8800U);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.button(16)"), "cmd.button(16)\r\n1\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.button(12)"), "cmd.button(12)\r\n1\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.button(2)"), "cmd.button(2)\r\n0\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.buttons()"), "cmd.buttons()\r\n34816\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.buttons(65535)"), "cmd.buttons(65535)\r\n>>> ") == 0);
    CHECK(s_sent[6].buttons == 0xFFFFU);
    hid_merge_on_tx_complete();
    run_cmd("cmd.release()");
    CHECK(s_sent[7].buttons == 0x0800U); /* physical button 12 still held */

    static const char *bad[] = {"cmd.button(0,1)", "cmd.button(17,1)", "cmd.button(1,2)", "cmd.button()",
                                "cmd.button(1,1,1)", "cmd.pan()", "cmd.pan(1,2)", "cmd.move(2147483648,0)",
                                "cmd.wheel(-2147483649)", "cmd.reset(1)", "cmd.stop(0)", "cmd.update(1)",
                                "cmd.update(0,1)", "cmd.update(1048577,1)", "cmd.update(10,-1)",
                                "cmd.update(10,4294967296)"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        s_outLen = 0;
        int ok   = cmd_execute_line(bad[i], strlen(bad[i]), &s_io);
        if (ok != 0 || s_outLen != 0)
        {
            printf("  not ignored: \"%s\"\n", bad[i]);
        }
        CHECK(ok == 0 && s_outLen == 0);
    }
}

static void test_boot_protocol(void)
{
    printf("merge: boot protocol limits X/Y to +-127 and drops wheel/pan\n");
    reset_usb();
    hid_merge_set_boot_protocol(true);
    hid_merge_physical(0, 0xFFFFU, 300, -5, 4, 2);
    CHECK(s_nSent == 1 && s_sent[0].x == 127 && s_sent[0].y == -5 && s_sent[0].wheel == 0 && s_sent[0].pan == 0);
    hid_merge_on_tx_complete();
    CHECK(s_nSent == 2 && s_sent[1].x == 127 && s_sent[1].y == 0);
    hid_merge_on_tx_complete();
    CHECK(s_nSent == 3 && s_sent[2].x == 46);
    hid_merge_on_tx_complete();
    CHECK(s_nSent == 3); /* nothing left: wheel/pan were dropped */
    hid_merge_set_boot_protocol(false);
    hid_merge_physical(0, 0xFFFFU, 300, 0, 0, 0);
    CHECK(s_nSent == 4 && s_sent[3].x == 300);
}

static void test_suspend_wake(void)
{
    printf("merge: suspend drops motion, a press requests wake-up, resume resends buttons\n");
    reset_usb();
    s_wakeups = 0;
    hid_merge_set_suspended(true);
    hid_merge_physical(0, 0xFFFFU, 50, 50, 1, 0);
    run_cmd("cmd.move(10,10)");
    CHECK(s_nSent == 0 && s_wakeups == 0); /* motion does not wake */
    hid_merge_physical(HID_BTN_LEFT, 0xFFFFU, 0, 0, 0, 0);
    CHECK(s_nSent == 0 && s_wakeups == 1);
    hid_merge_physical(HID_BTN_LEFT, 0xFFFFU, 0, 0, 0, 0); /* still held: no new press */
    CHECK(s_wakeups == 1);
    run_cmd("cmd.right(1)"); /* injected press wakes too */
    CHECK(s_wakeups == 2);
    hid_merge_set_suspended(false);
    CHECK(s_nSent == 1 && s_sent[0].buttons == (HID_BTN_LEFT | HID_BTN_RIGHT) && s_sent[0].x == 0);
}

static void test_reset_stop(void)
{
    printf("cmd.stop() and cmd.reset()\n");
    reset_usb();
    smooth_move_set_duration(10);
    run_cmd("cmd.move(1000,0)");
    long sx = 0, sy = 0;
    run_ticks(20, &sx, &sy, NULL);
    CHECK(sx > 0 && sx < 1000);
    CHECK(strcmp(run_cmd("cmd.stop()"), "cmd.stop()\r\n>>> ") == 0);
    CHECK(!smooth_move_busy());
    long rx = 0, ry = 0;
    run_ticks(80, &rx, &ry, NULL);
    CHECK(rx == 0 && s_sumX == sx); /* the rest was discarded */
    CHECK(smooth_move_get_duration() == 10); /* setting kept */

    reset_usb();
    smooth_move_set_duration(10);
    hid_merge_physical(HID_BTN_MIDDLE, 0xFFFFU, 0, 0, 0, 0);
    hid_merge_on_tx_complete();
    run_cmd("cmd.buttons(3)");
    hid_merge_on_tx_complete();
    run_cmd("cmd.move(500,500)");
    int n = s_nSent;
    CHECK(strcmp(run_cmd("cmd.reset()"), "cmd.reset()\r\n>>> ") == 0);
    CHECK(s_nSent == n + 1 && s_sent[n].buttons == HID_BTN_MIDDLE); /* physical stays */
    CHECK(!smooth_move_busy() && smooth_move_get_duration() == 10);
    smooth_move_set_duration(0);
}

static void test_drop_line(void)
{
    printf("command parser: a line cut by an RX overflow is dropped, the next one runs\n");
    reset_usb();
    cmd_parser_t p;
    cmd_parser_init(&p, &s_io);
    s_outLen = 0;
    s_out[0] = 0;
    const char *a = "cmd.le";
    cmd_parser_feed(&p, (const uint8_t *)a, strlen(a));
    cmd_parser_drop_line(&p); /* bytes in between were lost */
    const char *b = "ft(1)\r\ncmd.right(1)\r\n";
    cmd_parser_feed(&p, (const uint8_t *)b, strlen(b));
    CHECK(strcmp(s_out, "cmd.right(1)\r\n>>> ") == 0);
    CHECK(hid_merge_get_logical_buttons() == HID_BTN_RIGHT);
}

static void test_update_command(void)
{
    printf("cmd.update(size,crc) hands the channel to the updater\n");
    s_updCalls = 0;
    CHECK(strcmp(run_cmd("cmd.update(65536,4294967295)"), "cmd.update(65536,4294967295)\r\n") == 0);
    CHECK(s_updCalls == 1 && s_updSize == 65536U && s_updCrc == 0xFFFFFFFFU);
    /* not available without the hook */
    static const cmd_io_t noUpd = {cap_write, cap_sleep, NULL, NULL};
    s_outLen = 0;
    CHECK(cmd_execute_line("cmd.update(10,1)", 16, &noUpd) == 0 && s_outLen == 0);
}

static void test_crc32(void)
{
    printf("crc32: standard check value\n");
    CHECK(crc32_update(0U, "123456789", 9U) == 0xCBF43926U);
    uint32_t c = crc32_update(0U, "1234", 4U);
    CHECK(crc32_update(c, "56789", 5U) == 0xCBF43926U);
    CHECK(crc32_update(0U, "", 0U) == 0U);
}

/* ---- fake NOR flash ------------------------------------------------------ */
#define FAKE_FLASH_SIZE (APP_SETTINGS_OFFSET + 2U * APP_FLASH_SECTOR_SIZE)
static uint8_t s_flash[FAKE_FLASH_SIZE];
static int s_erases, s_programs, s_failProgramAt = -1;

static bool fake_read(uint32_t off, void *dst, uint32_t len)
{
    if ((off + len) > FAKE_FLASH_SIZE)
    {
        return false;
    }
    memcpy(dst, &s_flash[off], len);
    return true;
}
static bool fake_erase(uint32_t off)
{
    if (((off % APP_FLASH_SECTOR_SIZE) != 0U) || (off + APP_FLASH_SECTOR_SIZE > FAKE_FLASH_SIZE))
    {
        return false;
    }
    memset(&s_flash[off], 0xFF, APP_FLASH_SECTOR_SIZE);
    s_erases++;
    return true;
}
static bool fake_program(uint32_t off, const void *src, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    if ((len == 0U) || ((off / APP_FLASH_PAGE_SIZE) != ((off + len - 1U) / APP_FLASH_PAGE_SIZE)) ||
        (off + len > FAKE_FLASH_SIZE))
    {
        return false; /* crosses a page */
    }
    if (s_programs++ == s_failProgramAt)
    {
        return false;
    }
    for (uint32_t i = 0; i < len; i++)
    {
        s_flash[off + i] &= p[i]; /* NOR: 1 -> 0 only */
    }
    return true;
}
static const flash_if_t s_fakeFlash = {fake_read, fake_erase, fake_program};

static void test_settings(void)
{
    printf("settings: log in two sectors, newest wins, debounced auto-save\n");
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_erases = s_programs = 0;

    CHECK(!settings_init(&s_fakeFlash));
    CHECK(settings_saved()->smoothMs == 0);

    app_settings_t v = {.smoothMs = 25};
    CHECK(settings_save(&v) && settings_save_count() == 1);
    CHECK(s_erases == 0); /* blank sector: no erase needed */
    CHECK(settings_init(&s_fakeFlash) && settings_saved()->smoothMs == 25);

    /* equal value: nothing written */
    int p0 = s_programs;
    CHECK(settings_save(&v) && s_programs == p0);

    /* fill sector 0 and spill into sector 1, then back into sector 0 */
    for (uint32_t i = 0; i < SETTINGS_SLOTS + 5U; i++)
    {
        v.smoothMs = (uint16_t)(i % 900U + 1U);
        CHECK(settings_save(&v));
    }
    CHECK(s_erases == 1);
    CHECK(settings_init(&s_fakeFlash) && settings_saved()->smoothMs == v.smoothMs);
    for (uint32_t i = 0; i < SETTINGS_SLOTS; i++)
    {
        v.smoothMs = (uint16_t)(i % 7U + 100U);
        CHECK(settings_save(&v));
    }
    CHECK(s_erases == 2);
    CHECK(settings_init(&s_fakeFlash) && settings_saved()->smoothMs == v.smoothMs);

    /* a torn write (garbage record) is skipped; the previous value survives */
    uint32_t off = APP_SETTINGS_OFFSET;
    for (uint32_t i = 0; i < 2U * SETTINGS_SLOTS; i++, off += SETTINGS_RECORD_SIZE)
    {
        if (s_flash[off] == 0xFF && s_flash[off + 1] == 0xFF && s_flash[off + 2] == 0xFF)
        {
            s_flash[off] = 0x12; /* half-written slot */
            break;
        }
    }
    uint16_t before = v.smoothMs;
    CHECK(settings_init(&s_fakeFlash) && settings_saved()->smoothMs == before);
    v.smoothMs = 7;
    CHECK(settings_save(&v));
    CHECK(settings_init(&s_fakeFlash) && settings_saved()->smoothMs == 7);

    /* failed program: reported, old value stays saved */
    s_failProgramAt = s_programs;
    v.smoothMs      = 8;
    CHECK(!settings_save(&v) && settings_saved()->smoothMs == 7 && settings_error_count() == 1);
    s_failProgramAt = -1;

    /* debounce: saved only after the value has been stable for the delay */
    CHECK(settings_init(&s_fakeFlash));
    app_settings_t live = {.smoothMs = 300};
    CHECK(!settings_poll(&live, 1000U));
    live.smoothMs = 301;
    CHECK(!settings_poll(&live, 1500U));
    CHECK(!settings_poll(&live, 1500U + APP_SETTINGS_SAVE_DELAY_MS - 1U));
    CHECK(settings_poll(&live, 1500U + APP_SETTINGS_SAVE_DELAY_MS));
    CHECK(settings_saved()->smoothMs == 301);
    CHECK(!settings_poll(&live, 99999U)); /* nothing new */
}

/* ---- firmware update ----------------------------------------------------- */
static uint8_t s_image[70000];

static void make_image(uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
    {
        s_image[i] = (uint8_t)(i * 7U + 3U);
    }
    memcpy(&s_image[0], "FCFB", 4);
    s_image[0x1000] = 0xD1;
    uint32_t entry = APP_FLASH_BASE + 0x2000U, start = APP_FLASH_BASE;
    memcpy(&s_image[0x1004], &entry, 4);
    memcpy(&s_image[0x1020], &start, 4);
}

static fwu_state_t send_image(uint32_t size, uint32_t step)
{
    fwu_state_t st = FWU_ACTIVE;
    for (uint32_t off = 0; (off < size) && (st == FWU_ACTIVE); off += step)
    {
        uint32_t n = (size - off < step) ? (size - off) : step;
        st         = fw_update_feed(&s_image[off], n, 100U);
    }
    return st;
}

static int s_yields;
static void count_yield(void)
{
    s_yields++;
}

static void test_fw_update(void)
{
    printf("firmware update: staging, acks, CRC, image check, timeout\n");
    memset(s_flash, 0xFF, sizeof(s_flash));
    uint32_t size = 65000U;
    make_image(size);
    uint32_t crc = crc32_update(0U, s_image, size);

    s_outLen = 0;
    s_yields = 0;
    CHECK(fw_update_begin(&s_fakeFlash, size, crc, 0U, cap_write, count_yield));
    CHECK(strcmp(s_out, "erasing\r\n") == 0);
    CHECK(s_yields == (int)((size + APP_FLASH_SECTOR_SIZE - 1U) / APP_FLASH_SECTOR_SIZE));
    CHECK(fw_update_state() == FWU_ACTIVE);
    s_outLen = 0;
    CHECK(send_image(size, 333U) == FWU_INSTALL);
    CHECK(strncmp(s_out, "ok 1024\r\nok 2048\r\n", 18) == 0);
    CHECK(strstr(s_out, "ok 64512\r\nok 65000\r\ninstalling\r\n") != NULL);
    CHECK(memcmp(&s_flash[APP_STAGING_OFFSET], s_image, size) == 0);
    CHECK(s_flash[APP_STAGING_OFFSET + size] == 0xFF); /* nothing past the end */
    CHECK(memcmp(s_flash, "\xFF\xFF\xFF\xFF", 4) == 0); /* running slot untouched */

    /* wrong CRC */
    CHECK(fw_update_begin(&s_fakeFlash, size, crc ^ 1U, 0U, cap_write, NULL));
    s_outLen = 0;
    CHECK(send_image(size, 1024U) == FWU_ABORTED);
    CHECK(strstr(s_out, "error crc\r\n>>> ") != NULL);

    /* not a bootable image */
    s_image[0] = 'X';
    crc        = crc32_update(0U, s_image, size);
    CHECK(fw_update_begin(&s_fakeFlash, size, crc, 0U, cap_write, NULL));
    s_outLen = 0;
    CHECK(send_image(size, 4096U) == FWU_ABORTED && strstr(s_out, "error image\r\n") != NULL);
    make_image(size);
    uint32_t entry = APP_FLASH_BASE + size + 4U; /* entry point past the end */
    memcpy(&s_image[0x1004], &entry, 4);
    crc = crc32_update(0U, s_image, size);
    CHECK(fw_update_begin(&s_fakeFlash, size, crc, 0U, cap_write, NULL));
    CHECK(send_image(size, 4096U) == FWU_ABORTED);

    /* program failure */
    make_image(size);
    crc = crc32_update(0U, s_image, size);
    CHECK(fw_update_begin(&s_fakeFlash, size, crc, 0U, cap_write, NULL));
    s_failProgramAt = s_programs + 3;
    s_outLen        = 0;
    CHECK(send_image(size, 1024U) == FWU_ABORTED && strstr(s_out, "error program\r\n>>> ") != NULL);
    s_failProgramAt = -1;

    /* timeout */
    CHECK(fw_update_begin(&s_fakeFlash, size, crc, 1000U, cap_write, NULL));
    CHECK(fw_update_feed(s_image, 10U, 2000U) == FWU_ACTIVE);
    CHECK(fw_update_poll(2000U + APP_UPDATE_TIMEOUT_MS - 1U) == FWU_ACTIVE);
    s_outLen = 0;
    CHECK(fw_update_poll(2000U + APP_UPDATE_TIMEOUT_MS) == FWU_ABORTED);
    CHECK(strcmp(s_out, "error timeout\r\n>>> ") == 0);
}

static void test_watch(void)
{
    printf("watchdog watch list: deadline and parking\n");
    watch_init(0U);
    watch_id_t late;
    CHECK(watch_all_alive(APP_WDOG_TASK_DEADLINE_MS, &late));
    watch_checkin(WATCH_UART, 500U);
    CHECK(!watch_all_alive(APP_WDOG_TASK_DEADLINE_MS + 1U, &late) && late == WATCH_USB_HOST);
    watch_checkin(WATCH_USB_HOST, 900U);
    CHECK(watch_all_alive(1400U, NULL));
    CHECK(!watch_all_alive(1501U, &late) && late == WATCH_UART);
    watch_park(WATCH_UART); /* e.g. cmd.sleep(600000) */
    CHECK(watch_all_alive(1800U, NULL));
    CHECK(!watch_all_alive(1901U, &late) && late == WATCH_USB_HOST);
    watch_checkin(WATCH_USB_HOST, 1900U);
    CHECK(watch_all_alive(100000U - 1U, NULL) == false); /* host late again */
    watch_checkin(WATCH_USB_HOST, 99000U);
    CHECK(watch_all_alive(99500U, NULL)); /* uart still parked */
    watch_checkin(WATCH_UART, 99600U);    /* un-parks */
    CHECK(!watch_all_alive(101000U, &late));
    CHECK(strcmp(watch_name(WATCH_UART), "uart_cmd") == 0);
}

static void test_status_led_suspend(void)
{
    printf("status LED: off while the PC sleeps\n");
    app_status_init();
    const app_status_t *st = app_status_get();
    app_status_pc_configured(true);
    app_status_pc_suspended(true);
    int lit, edges;
    led_profile(st, 10000U, &lit, &edges);
    CHECK(lit == 0);
    app_status_pc_suspended(false);
    led_profile(st, 10000U, &lit, &edges);
    CHECK(lit == 100);
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
    test_parser_arrays_pan_split();
    test_merge_split_buttons_pan();
    test_boot_protocol();
    test_suspend_wake();
    test_reset_stop();
    test_update_command();
    test_drop_line();
    test_crc32();
    test_settings();
    test_fw_update();
    test_watch();
    test_status_led_suspend();
    if (s_fail)
    {
        printf("%d check(s) FAILED\n", s_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
