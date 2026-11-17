/*
 * sim_board.c - The board's command channel on a pseudo-terminal, for
 * testing tools/fw_update.py (and any other PC-side script) without
 * hardware. It runs the real command parser and update protocol code with
 * a RAM "flash", and mimics the UART task's flow (flush before "ready",
 * prompt after the simulated reboot).
 *
 *   make -C test sim && ./test/sim_board    # prints the pty path
 *   python3 tools/fw_update.py /dev/pts/N build/hid_proxy.bin
 *
 * On install it copies the staging area over the "running" slot, prints
 * "sim: installed N bytes" to stderr and prompts again as after a reboot.
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "fw_update.h"
#include "hid_merge.h"
#include "smooth_move.h"
#include "uart_parser.h"

static int s_fd;

bool usb_device_send_report(const hid_mouse_report_t *r)
{
    (void)r;
    return false;
}
void usb_device_request_wakeup(void)
{
}

/* ---- RAM flash ---- */
#define FLASH_SIZE (APP_SETTINGS_OFFSET + 2U * APP_FLASH_SECTOR_SIZE)
static uint8_t s_flash[FLASH_SIZE];
static bool f_read(uint32_t off, void *dst, uint32_t len)
{
    memcpy(dst, &s_flash[off], len);
    return (off + len) <= FLASH_SIZE;
}
static bool f_erase(uint32_t off)
{
    memset(&s_flash[off], 0xFF, APP_FLASH_SECTOR_SIZE);
    usleep(2000); /* a little of the real erase time */
    return true;
}
static bool f_program(uint32_t off, const void *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        s_flash[off + i] &= ((const uint8_t *)src)[i];
    }
    return true;
}
static const flash_if_t s_flashIf = {f_read, f_erase, f_program};

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void out(const char *d, size_t n)
{
    while (n > 0)
    {
        ssize_t w = write(s_fd, d, n);
        if (w <= 0)
        {
            return;
        }
        d += w;
        n -= (size_t)w;
    }
}

static bool s_update, s_flush;
static void begin(uint32_t size, uint32_t crc)
{
    if (fw_update_begin(&s_flashIf, size, crc, now_ms(), out, NULL))
    {
        s_update = true;
        s_flush  = true;
    }
}

static const cmd_io_t s_io = {out, NULL, now_ms, begin};

int main(void)
{
    s_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if ((s_fd < 0) || grantpt(s_fd) || unlockpt(s_fd))
    {
        perror("pty");
        return 1;
    }
    struct termios t;
    tcgetattr(s_fd, &t);
    cfmakeraw(&t);
    tcsetattr(s_fd, TCSANOW, &t);
    printf("%s\n", ptsname(s_fd));
    fflush(stdout);

    memset(s_flash, 0xFF, sizeof(s_flash));
    hid_merge_init();
    smooth_move_init();
    cmd_parser_t p;
    cmd_parser_init(&p, &s_io);
    out(">>> ", 4);

    for (;;)
    {
        struct pollfd pfd = {s_fd, POLLIN, 0};
        uint8_t buf[4096];
        ssize_t n = 0;
        if (poll(&pfd, 1, 1) > 0)
        {
            n = read(s_fd, buf, sizeof(buf));
            if (n < 0)
            {
                usleep(1000); /* no client yet */
                n = 0;
            }
        }
        for (ssize_t i = 0; i < n;)
        {
            size_t chunk = (size_t)(n - i);
            if (s_update)
            {
                fwu_state_t st = fw_update_feed(&buf[i], chunk, now_ms());
                if (st == FWU_INSTALL)
                {
                    uint32_t size = fw_update_size();
                    memcpy(s_flash, &s_flash[APP_STAGING_OFFSET], size);
                    fprintf(stderr, "sim: installed %u bytes\n", size);
                    s_update = false;
                    usleep(300000); /* reboot */
                    cmd_parser_init(&p, &s_io);
                    out(">>> ", 4);
                    break; /* anything else in this read is gone with the reboot */
                }
                if (st != FWU_ACTIVE)
                {
                    s_update = false;
                }
                i += (ssize_t)chunk;
            }
            else
            {
                /* Feed one byte at a time so an update started mid-buffer
                 * switches mode at the right place, like the real ring. */
                cmd_parser_feed(&p, &buf[i], 1);
                i++;
                if (s_flush)
                {
                    s_flush = false;
                    tcflush(s_fd, TCIFLUSH);
                    i = n; /* discard the rest, as the board does */
                    out("ready\r\n", 7);
                }
            }
        }
        if (s_update && (fw_update_poll(now_ms()) != FWU_ACTIVE))
        {
            s_update = false;
        }
    }
}
