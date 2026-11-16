/*
 * PC unit tests for the hardware-independent modules:
 *   hid_merge.c          (button locking, accumulation, splitting)
 *   hid_report_parser.c  (dynamic report layout discovery)
 *   uart_parser.c        (command grammar + response protocol)
 *
 * Build & run:  make -C test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_merge.h"
#include "hid_report_parser.h"
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

bool usb_device_send_report(const hid_mouse_report_t *r)
{
    s_sent[s_nSent++ % 64] = *r;
    return true;
}

static void reset_usb(void)
{
    s_nSent = 0;
    hid_merge_init();
    hid_merge_set_device_ready(true);
}

/* ---- UART output capture ------------------------------------------------ */
static char s_out[512];
static size_t s_outLen;
static void cap_write(const char *d, size_t n)
{
    memcpy(&s_out[s_outLen], d, n);
    s_outLen += n;
    s_out[s_outLen] = 0;
}
static const char *run_cmd(const char *line)
{
    s_outLen = 0;
    s_out[0] = 0;
    cmd_execute_line(line, strlen(line), cap_write);
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
    printf("command parser\n");
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

    /* query reflects the logical (physical | injected) state */
    hid_merge_physical(HID_BTN_MIDDLE, true, 0, 0, 0);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.middle()"), "cmd.middle()\r\n1\r\n>>> ") == 0);

    CHECK(strcmp(run_cmd("cmd.wheel(-2)"), "cmd.wheel(-2)\r\n>>> ") == 0);
    hid_merge_on_tx_complete();
    CHECK(strcmp(run_cmd("cmd.right(1)"), "cmd.right(1)\r\n>>> ") == 0);
    hid_merge_on_tx_complete();

    /* errors */
    CHECK(strcmp(run_cmd("cmd.left(2)"), "cmd.left(2)\r\nERR\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.move(1)"), "cmd.move(1)\r\nERR\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.move(1,2,3)"), "cmd.move(1,2,3)\r\nERR\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.move(a,2)"), "cmd.move(a,2)\r\nERR\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("cmd.jump(1)"), "cmd.jump(1)\r\nERR\r\n>>> ") == 0);
    CHECK(strcmp(run_cmd("hello"), "hello\r\nERR\r\n>>> ") == 0);

    /* streaming feed: CRLF framing, split across chunks, two commands */
    cmd_parser_t p;
    cmd_parser_init(&p, cap_write);
    s_outLen = 0;
    const char *a = "cmd.mo";
    const char *b = "ve(1,1)\r\ncmd.left()\r\n";
    cmd_parser_feed(&p, (const uint8_t *)a, strlen(a));
    cmd_parser_feed(&p, (const uint8_t *)b, strlen(b));
    CHECK(strcmp(s_out, "cmd.move(1,1)\r\n>>> cmd.left()\r\n0\r\n>>> ") == 0);

    /* overlong line */
    s_outLen = 0;
    char longl[200];
    memset(longl, 'x', sizeof(longl));
    cmd_parser_feed(&p, (const uint8_t *)longl, sizeof(longl));
    cmd_parser_feed(&p, (const uint8_t *)"\r\n", 2);
    CHECK(strcmp(s_out, "ERR\r\n>>> ") == 0);
}

int main(void)
{
    test_button_locking();
    test_motion_merge();
    test_parser_simple();
    test_parser_gaming();
    test_parser_reject();
    test_commands();
    if (s_fail)
    {
        printf("%d check(s) FAILED\n", s_fail);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
