/*
 * uart_parser.c - Command channel on LPUART3 @ 6 Mbaud.
 *
 * RX path (why DMA): at 6 Mbaud a byte arrives every ~1.67 us and the RT1062
 * LPUART FIFO is only 4 deep, i.e. a 6.7 us overrun window. The USB ISRs run
 * at higher priority, so a per-byte interrupt scheme could overrun. Instead an
 * eDMA channel streams every received byte into a circular buffer with no CPU
 * involvement; the LPUART IDLE-line interrupt (end of a command burst) plus a
 * 1 ms timeout wakes the parser task, which consumes up to the DMA write index.
 *
 *     CH343P --6Mbd--> LPUART3 RX --DMA req--> eDMA ch --> s_rxRing[4096]
 *                         |                                     ^
 *                      IDLE IRQ --notify--> uart_parser_task ----+ (reads)
 */
#include "uart_parser.h"
#include "app_status.h"
#include "hid_merge.h"
#include "smooth_move.h"

#include <stdbool.h>
#include <string.h>

/* ========================================================================= */
/* Pure command layer                                                        */
/* ========================================================================= */

#define MAX_ARGS 2

typedef enum
{
    CMD_MOVE,
    CMD_WHEEL,
    CMD_PAN,
    CMD_BUTTON,  /* left/right/middle/side1/side2: arg = bit mask in table */
    CMD_BUTTON_N, /* button(n[,v])                                          */
    CMD_BUTTONS,
    CMD_RELEASE,
    CMD_SLEEP,
    CMD_VERSION,
    CMD_INFO,
    CMD_SMOOTH,
    CMD_STATUS,
    CMD_RESET,
    CMD_STOP,
    CMD_UPDATE,
} cmd_kind_t;

typedef struct
{
    const char *name;
    cmd_kind_t kind;
    uint16_t mask; /* CMD_BUTTON only */
} cmd_def_t;

static const cmd_def_t s_commands[] = {
    {"move", CMD_MOVE, 0U},
    {"wheel", CMD_WHEEL, 0U},
    {"pan", CMD_PAN, 0U},
    {"button", CMD_BUTTON_N, 0U},
    {"left", CMD_BUTTON, HID_BTN_LEFT},
    {"right", CMD_BUTTON, HID_BTN_RIGHT},
    {"middle", CMD_BUTTON, HID_BTN_MIDDLE},
    {"side1", CMD_BUTTON, HID_BTN_SIDE1},
    {"side2", CMD_BUTTON, HID_BTN_SIDE2},
    {"buttons", CMD_BUTTONS, 0U},
    {"release", CMD_RELEASE, 0U},
    {"sleep", CMD_SLEEP, 0U},
    {"version", CMD_VERSION, 0U},
    {"info", CMD_INFO, 0U},
    {"smooth", CMD_SMOOTH, 0U},
    {"status", CMD_STATUS, 0U},
    {"reset", CMD_RESET, 0U},
    {"stop", CMD_STOP, 0U},
    {"update", CMD_UPDATE, 0U},
};

static void put_str(const cmd_io_t *io, const char *s)
{
    io->write(s, strlen(s));
}

static void put_uint(const cmd_io_t *io, uint32_t u)
{
    char buf[10];
    int i = (int)sizeof(buf);
    do
    {
        buf[--i] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);
    io->write(&buf[i], sizeof(buf) - (size_t)i);
}

static bool is_space(char c)
{
    return (c == ' ') || (c == '\t');
}

/* Parse a signed decimal integer occupying [s, e) (surrounding blanks
 * allowed). Accepts -2147483648..4294967295 so a CRC-32 fits; each command
 * checks its own range. */
#define ARG_MIN (-2147483648LL)
#define ARG_MAX 4294967295LL
static bool parse_int(const char *s, const char *e, int64_t *out)
{
    while ((s < e) && is_space(*s))
    {
        s++;
    }
    while ((e > s) && is_space(e[-1]))
    {
        e--;
    }
    if (s >= e)
    {
        return false;
    }
    bool neg = false;
    if ((*s == '+') || (*s == '-'))
    {
        neg = (*s == '-');
        s++;
    }
    if (s >= e)
    {
        return false;
    }
    int64_t v = 0;
    for (; s < e; s++)
    {
        if ((*s < '0') || (*s > '9'))
        {
            return false;
        }
        v = (v * 10) + (*s - '0');
        if (v > ARG_MAX)
        {
            return false;
        }
    }
    v = neg ? -v : v;
    if (v < ARG_MIN)
    {
        return false;
    }
    *out = v;
    return true;
}

static bool is_i32(int64_t v)
{
    return (v >= -2147483648LL) && (v <= 2147483647LL);
}

static const cmd_def_t *lookup(const char *name, size_t n)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++)
    {
        if ((strlen(s_commands[i].name) == n) && (memcmp(s_commands[i].name, name, n) == 0))
        {
            return &s_commands[i];
        }
    }
    return NULL;
}

/*
 * Split "cmd.<name>(<args>)" into a command definition and integer args.
 * Returns false for anything that is not syntactically a known command.
 */
static bool parse_command(const char *s, const char *e, const cmd_def_t **def, int64_t *argv, int *argc)
{
    if (((size_t)(e - s) < 6U) || (memcmp(s, "cmd.", 4) != 0) || (e[-1] != ')'))
    {
        return false;
    }
    const char *name = s + 4;
    const char *lp   = memchr(name, '(', (size_t)(e - name));
    const char *rp   = e - 1;
    if (lp == NULL)
    {
        return false;
    }
    *def = lookup(name, (size_t)(lp - name));
    if (*def == NULL)
    {
        return false;
    }

    /* Empty (or blank) parentheses -> zero arguments. */
    *argc         = 0;
    const char *a = lp + 1;
    bool blank    = true;
    for (const char *c = a; c < rp; c++)
    {
        if (!is_space(*c))
        {
            blank = false;
            break;
        }
    }
    if (blank)
    {
        return true;
    }

    for (const char *c = a;; c++)
    {
        if ((c == rp) || (*c == ','))
        {
            if ((*argc == MAX_ARGS) || !parse_int(a, c, &argv[*argc]))
            {
                return false; /* too many args, or not an integer */
            }
            (*argc)++;
            a = c + 1;
            if (c == rp)
            {
                return true;
            }
        }
        else if (*c == '(' || *c == ')')
        {
            return false; /* nested / stray parentheses */
        }
    }
}

int cmd_execute_line(const char *line, size_t len, const cmd_io_t *io)
{
    const char *s = line;
    const char *e = line + len;
    const cmd_def_t *def;
    int64_t v[MAX_ARGS] = {0, 0};
    int argc            = 0;

    /* What a valid command produces besides the echo: */
    const char *strResult = NULL;  /* version / info               */
    bool numResult        = false; /* button queries               */
    uint32_t numValue     = 0U;
    uint32_t sleepMs      = 0U;
    bool doSleep          = false;
    bool doStatus         = false; /* status                     */
    bool doUpdate         = false; /* update: hand over to io    */

    while ((s < e) && is_space(*s))
    {
        s++;
    }
    while ((e > s) && is_space(e[-1]))
    {
        e--;
    }
    if (!parse_command(s, e, &def, v, &argc))
    {
        return 0; /* not a command: silently ignored */
    }

    /*
     * Validate arguments FIRST and only then act, so an invalid command has
     * no side effects at all (it is silently ignored like an unknown one).
     */
    switch (def->kind)
    {
        case CMD_MOVE:
            if ((argc != 2) || !is_i32(v[0]) || !is_i32(v[1]))
            {
                return 0;
            }
            /* Instant when smoothing is off (default), else queued and
             * rendered over cmd.smooth(ms) by the tick. */
            smooth_move_start((int32_t)v[0], (int32_t)v[1]);
            break;

        case CMD_WHEEL:
        case CMD_PAN:
            if ((argc != 1) || !is_i32(v[0]))
            {
                return 0;
            }
            if (def->kind == CMD_WHEEL)
            {
                hid_merge_inject_wheel((int32_t)v[0]);
            }
            else
            {
                hid_merge_inject_pan((int32_t)v[0]);
            }
            break;

        case CMD_BUTTON:
        case CMD_BUTTON_N:
        {
            /* left(v) ... side2(v) have a fixed button; button(n, v) names it. */
            uint16_t mask = def->mask;
            int64_t val   = 0;
            int nargs     = argc;
            if (def->kind == CMD_BUTTON_N)
            {
                if ((argc < 1) || (v[0] < 1) || (v[0] > (int64_t)HID_BUTTON_COUNT))
                {
                    return 0;
                }
                mask  = (uint16_t)(1U << (uint32_t)(v[0] - 1));
                val   = v[1];
                nargs = argc - 1;
            }
            else
            {
                val = v[0];
            }
            if (nargs == 0)
            {
                numResult = true;
                numValue  = ((hid_merge_get_logical_buttons() & mask) != 0U) ? 1U : 0U;
            }
            else if ((nargs == 1) && ((val == 0) || (val == 1)))
            {
                /* Button locking happens in hid_merge: no report unless
                 * (physical | injected) changes. */
                hid_merge_inject_button(mask, val == 1);
            }
            else
            {
                return 0;
            }
            break;
        }

        case CMD_BUTTONS:
            if (argc == 0)
            {
                numResult = true;
                numValue  = hid_merge_get_logical_buttons();
            }
            else if ((argc == 1) && (v[0] >= 0) && (v[0] <= 0xFFFF))
            {
                hid_merge_set_injected_buttons((uint16_t)v[0]);
            }
            else
            {
                return 0;
            }
            break;

        case CMD_RELEASE:
            if (argc != 0)
            {
                return 0;
            }
            /* Clears every injected button (1-16). Buttons held on the
             * physical mouse stay pressed, so a report is sent only if the
             * logical state changes. */
            hid_merge_set_injected_buttons(0U);
            break;

        case CMD_STOP:
            if (argc != 0)
            {
                return 0;
            }
            /* Cancel the smoothed move in flight and any queued ones. */
            smooth_move_cancel();
            break;

        case CMD_RESET:
            if (argc != 0)
            {
                return 0;
            }
            /* Back to "nothing injected": stop moves, release injected
             * buttons, drop undelivered motion. Settings (cmd.smooth) stay. */
            smooth_move_cancel();
            hid_merge_reset_injected();
            break;

        case CMD_UPDATE:
            /* update(size, crc32): the firmware image follows in binary. */
            if ((argc != 2) || (v[0] <= 0) || (v[0] > (int64_t)APP_SLOT_SIZE) || (v[1] < 0) ||
                (io->update_begin == NULL))
            {
                return 0;
            }
            doUpdate = true;
            break;

        case CMD_SLEEP:
            if ((argc != 1) || (v[0] < 0) || (v[0] > (int64_t)APP_CMD_SLEEP_MAX_MS))
            {
                return 0;
            }
            doSleep = true;
            sleepMs = (uint32_t)v[0];
            break;

        case CMD_SMOOTH:
            if (argc == 0)
            {
                numResult = true;
                numValue  = smooth_move_get_duration();
            }
            else if ((argc == 1) && (v[0] >= 0) && (v[0] <= (int64_t)APP_SMOOTH_MAX_MS))
            {
                smooth_move_set_duration((uint32_t)v[0]); /* 0 = off */
            }
            else
            {
                return 0;
            }
            break;

        case CMD_STATUS:
            if (argc != 0)
            {
                return 0;
            }
            doStatus = true;
            break;

        case CMD_VERSION:
        case CMD_INFO:
            if (argc != 0)
            {
                return 0;
            }
            strResult = (def->kind == CMD_VERSION) ? APP_FW_VERSION : APP_DEVICE_INFO;
            break;

        default:
            return 0;
    }

    /* Echo exactly what was received (trimmed), optional result, prompt. */
    io->write(s, (size_t)(e - s));
    if (strResult != NULL)
    {
        put_str(io, "\r\n");
        put_str(io, strResult);
    }
    else if (numResult)
    {
        put_str(io, "\r\n");
        put_uint(io, numValue);
    }
    else if (doStatus)
    {
        put_str(io, "\r\n");
        app_status_print(app_status_get(), (io->now_ms != NULL) ? io->now_ms() : 0U, io->write);
    }
    if (doUpdate)
    {
        /* No prompt: the update protocol takes over the channel and prints
         * its own progress, then ">>> " when it is back in command mode. */
        put_str(io, "\r\n");
        io->update_begin((uint32_t)v[0], (uint32_t)v[1]);
        return 1;
    }
    put_str(io, "\r\n>>> ");

    /* cmd.sleep: the prompt is already out; now block only this task. */
    if (doSleep && (io->sleep_ms != NULL) && (sleepMs > 0U))
    {
        io->sleep_ms(sleepMs);
    }
    return 1;
}

void cmd_parser_init(cmd_parser_t *p, const cmd_io_t *io)
{
    memset(p, 0, sizeof(*p));
    p->io = io;
}

void cmd_parser_drop_line(cmd_parser_t *p)
{
    /* Discard everything up to and including the next line end. */
    p->len      = 0U;
    p->overflow = 1U;
}

void cmd_parser_feed(cmd_parser_t *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = (char)data[i];
        if ((c == '\r') || (c == '\n'))
        {
            /* "\r\n": '\r' completes the line, the '\n' then sees an empty
             * buffer and is ignored. Overlong lines are dropped silently. */
            if (!p->overflow && (p->len > 0U))
            {
                (void)cmd_execute_line(p->line, p->len, p->io);
            }
            p->len      = 0U;
            p->overflow = 0U;
        }
        else if (p->overflow)
        {
            /* drop until end of line */
        }
        else if (p->len < APP_CMD_MAX_LINE)
        {
            p->line[p->len++] = c;
        }
        else
        {
            p->overflow = 1U;
        }
    }
}

/* ========================================================================= */
/* Hardware layer - excluded from PC unit tests                              */
/* ========================================================================= */
#ifndef UNIT_TEST

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_lpuart.h"
#include "fsl_edma.h"
#include "fsl_dmamux.h"
#include "board_init.h"
#include "flash_ops.h"
#include "fw_update.h"
#include "status_led.h"
#include "watch.h"
#include "watchdog.h"

#define APP_UART            LPUART3
#define APP_UART_IRQn       LPUART3_IRQn
#define APP_UART_DMA_SOURCE kDmaRequestMuxLPUART3Rx
#define APP_UART_DMA_IRQn   DMA0_DMA16_IRQn /* channel 0 (and 16) */
#define RX_MASK             (APP_UART_RX_RING_SIZE - 1U)

#if (APP_UART_RX_RING_SIZE & RX_MASK) != 0
#error "APP_UART_RX_RING_SIZE must be a power of two"
#endif
#if APP_UART_DMA_CHANNEL != 0U
#error "APP_UART_DMA_IRQn / DMA0_DMA16_IRQHandler assume eDMA channel 0"
#endif

/* DMA target must be non-cacheable (Cortex-M7 D-cache): AT_NONCACHEABLE_SECTION
 * places it in DTCM with this project's linker script. */
AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t s_rxRing[APP_UART_RX_RING_SIZE], 32);

static TaskHandle_t s_uartTask;
static cmd_parser_t s_parser;

/*
 * Overflow detection
 * ------------------
 * The DMA writes the ring endlessly; the task reads it. Both positions are
 * kept as running byte totals (mod 2^32): the DMA's total is
 * wraps * RING_SIZE + index, where wraps is counted by the major-loop
 * interrupt (once per lap). If the DMA is more than RING_SIZE bytes ahead of
 * the reader, it has overwritten unread data: that is an overflow. The
 * reader then skips to the newest data and drops the partial line.
 */
static volatile uint32_t s_wraps;       /* DMA laps, from the major-loop IRQ */
static uint32_t s_rdTotal;              /* bytes consumed by the task        */
static uint32_t s_overflows;
static uint32_t s_lostBytes;
static volatile uint32_t s_hwOverruns;  /* LPUART OR flag (should stay 0)    */

/* Firmware update mode (fw_update.h). */
static volatile bool s_updateMode;
static volatile bool s_flushBeforeReady;
/* After an aborted update, image bytes may still be arriving: they are
 * discarded until the line has been quiet for DISCARD_QUIET_MS, so none of
 * them is ever parsed as a command. */
#define DISCARD_QUIET_MS 20U
static bool s_discarding;
static uint32_t s_discardSeenTotal;
static uint32_t s_discardQuietSinceMs;

static void uart_write(const char *data, size_t len);
static void uart_sleep(uint32_t ms);
static void uart_update_begin(uint32_t size, uint32_t crc);

static const cmd_io_t s_io = {uart_write, uart_sleep, app_now_ms, uart_update_begin};

static void uart_sleep(uint32_t ms)
{
    /*
     * cmd.sleep(): blocks only this (medium priority) task. The USB host stack
     * task and both USB ISRs keep forwarding the physical mouse, and the eDMA
     * keeps filling the RX ring. Commands that arrive meanwhile run, in order,
     * once the sleep ends. The ring holds APP_UART_RX_RING_SIZE bytes (roughly
     * 200 typical commands); anything beyond that during one sleep is lost and
     * counted as an overflow. APP_CMD_SLEEP_MAX_MS keeps ms * configTICK_RATE_HZ
     * within 32 bits. The watchdog is told this task is parked, not hung.
     */
    watch_park(WATCH_UART);
    vTaskDelay(pdMS_TO_TICKS(ms));
    watch_checkin(WATCH_UART, app_now_ms());
}

static void uart_write(const char *data, size_t len)
{
    /*
     * Blocking TX is acceptable here: responses are ~20-400 bytes, i.e. up to
     * ~0.7 ms at 6 Mbaud, and this task is preempted by the USB host task/ISRs
     * anyway. Switch to LPUART_SendEDMA() on a second DMA channel if responses
     * grow.
     */
    (void)LPUART_WriteBlocking(APP_UART, (const uint8_t *)data, len);
}

/* Between staging-sector erases (each ~45 ms): stay responsive to the
 * watchdog and let lower-priority tasks run. */
static void update_yield(void)
{
    watch_checkin(WATCH_UART, app_now_ms());
    vTaskDelay(1);
}

static void uart_update_begin(uint32_t size, uint32_t crc)
{
    if (fw_update_begin(flash_ops_if(), size, crc, app_now_ms(), uart_write, update_yield))
    {
        /* The task discards whatever arrived during the erase (at least the
         * '\n' of "\r\n"), then prints "ready" and routes bytes to the
         * updater. */
        s_updateMode       = true;
        s_flushBeforeReady = true;
    }
}

void uart_parser_hw_init(void)
{
    lpuart_config_t cfg;

    /*
     * Baud generation: board_init.c switches the UART clock root to the 24 MHz
     * oscillator. LPUART_Init() then finds OSR=4, SBR=1 -> exactly 6.000 Mbaud
     * (and sets BAUD[BOTHEDGE], required for OSR 4..7). With the default
     * 80 MHz (PLL3/6) root the best fit is 6.15 Mbaud (+2.6 %), too marginal.
     */
    LPUART_GetDefaultConfig(&cfg);
    cfg.baudRate_Bps = APP_UART_BAUD;
    cfg.enableTx     = true;
    cfg.enableRx     = true;
    cfg.rxIdleType   = kLPUART_IdleTypeStopBit;
    cfg.rxIdleConfig = kLPUART_IdleCharacter2; /* 2 idle chars = 3.3 us */
    if (LPUART_Init(APP_UART, &cfg, board_uart_clock_hz()) != kStatus_Success)
    {
        app_fatal(APP_FATAL_UART_INIT); /* baud rate not achievable - check clock root */
    }

    /* --- eDMA circular RX -------------------------------------------------- */
    DMAMUX_Init(DMAMUX);
    DMAMUX_SetSource(DMAMUX, APP_UART_DMA_CHANNEL, APP_UART_DMA_SOURCE);
    DMAMUX_EnableChannel(DMAMUX, APP_UART_DMA_CHANNEL);

    edma_config_t ecfg;
    EDMA_GetDefaultConfig(&ecfg);
    EDMA_Init(DMA0, &ecfg);
    EDMA_ResetChannel(DMA0, APP_UART_DMA_CHANNEL);

    edma_transfer_config_t xfer;
    EDMA_PrepareTransfer(&xfer, (void *)LPUART_GetDataRegisterAddress(APP_UART), 1U, s_rxRing, 1U,
                         1U /* bytes per request */, APP_UART_RX_RING_SIZE, kEDMA_PeripheralToMemory);
    EDMA_SetTransferConfig(DMA0, APP_UART_DMA_CHANNEL, &xfer, NULL);

    /* Make it a ring: after the major loop, rewind DADDR to the buffer start
     * and keep the hardware request enabled (DREQ = 0) so it never stops.
     * INTMAJOR counts the laps for overflow detection. */
    DMA0->TCD[APP_UART_DMA_CHANNEL].DLAST_SGA = -(int32_t)APP_UART_RX_RING_SIZE;
    DMA0->TCD[APP_UART_DMA_CHANNEL].CSR =
        (uint16_t)((DMA0->TCD[APP_UART_DMA_CHANNEL].CSR & (uint16_t)~DMA_CSR_DREQ_MASK) | DMA_CSR_INTMAJOR_MASK);
    NVIC_SetPriority(APP_UART_DMA_IRQn, APP_NVIC_PRIO_UART);
    EnableIRQ(APP_UART_DMA_IRQn);
    EDMA_EnableChannelRequest(DMA0, APP_UART_DMA_CHANNEL);

    LPUART_EnableRxDMA(APP_UART, true);

    /* IDLE-line IRQ marks the end of a command burst. */
    LPUART_EnableInterrupts(APP_UART, kLPUART_IdleLineInterruptEnable | kLPUART_RxOverrunInterruptEnable);
    NVIC_SetPriority(APP_UART_IRQn, APP_NVIC_PRIO_UART);
    EnableIRQ(APP_UART_IRQn);

    s_rdTotal = 0U;
    s_wraps   = 0U;
    cmd_parser_init(&s_parser, &s_io);
}

/* Bytes the DMA has written since start (mod 2^32). */
static uint32_t dma_write_total(void)
{
    for (;;)
    {
        uint32_t m        = DisableGlobalIRQ();
        uint32_t chMask   = 1UL << APP_UART_DMA_CHANNEL;
        bool pend1        = (DMA0->INT & chMask) != 0U; /* lap not yet counted */
        uint32_t remaining = EDMA_GetRemainingMajorLoopCount(DMA0, APP_UART_DMA_CHANNEL);
        bool pend2        = (DMA0->INT & chMask) != 0U;
        uint32_t wraps    = s_wraps;
        EnableGlobalIRQ(m);

        if (pend1 == pend2)
        {
            /* CITER counts down from RING_SIZE and reloads on each lap. */
            uint32_t idx = (APP_UART_RX_RING_SIZE - remaining) & RX_MASK;
            return ((wraps + (pend1 ? 1U : 0U)) * APP_UART_RX_RING_SIZE) + idx;
        }
        /* A lap completed between the two reads: sample again. */
    }
}

static void publish_counters(void)
{
    app_status_uart(s_rdTotal, s_overflows, s_lostBytes, s_hwOverruns);
}

static void leave_update_mode(void)
{
    s_updateMode          = false;
    s_discarding          = true;
    s_discardSeenTotal    = dma_write_total();
    s_discardQuietSinceMs = app_now_ms();
}

/* Hand [rd, rd+n) of the ring to the parser or the updater. */
static void consume(const uint8_t *data, uint32_t n)
{
    if (s_updateMode)
    {
        if (fw_update_feed(data, n, app_now_ms()) == FWU_INSTALL)
        {
            /* Let "installing" leave the UART, then replace the firmware. */
            while ((LPUART_GetStatusFlags(APP_UART) & (uint32_t)kLPUART_TransmissionCompleteFlag) == 0U)
            {
            }
            flash_ops_install(fw_update_size());
        }
        if (fw_update_state() != FWU_ACTIVE)
        {
            leave_update_mode(); /* aborted: back in command mode */
        }
    }
    else
    {
        cmd_parser_feed(&s_parser, data, n);
    }
}

void uart_parser_task(void *arg)
{
    (void)arg;
    s_uartTask = xTaskGetCurrentTaskHandle();
    watch_checkin(WATCH_UART, app_now_ms());

    uart_write(">>> ", 4U);

    for (;;)
    {
        /* Woken by the IDLE interrupt; the 1 ms timeout bounds latency when the
         * host streams commands back-to-back with no idle gap. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        watch_checkin(WATCH_UART, app_now_ms());

        uint32_t wrTotal = dma_write_total();
        uint32_t pending = wrTotal - s_rdTotal;
        if (pending > APP_UART_RX_RING_SIZE)
        {
            /* Unread data was overwritten: skip to the newest ring content and
             * drop the line in progress (its start is gone). */
            s_overflows++;
            s_lostBytes += pending - APP_UART_RX_RING_SIZE;
            s_rdTotal = wrTotal - APP_UART_RX_RING_SIZE;
            cmd_parser_drop_line(&s_parser);
        }

        if (s_discarding)
        {
            uint32_t now = app_now_ms();
            if (wrTotal != s_discardSeenTotal)
            {
                s_discardSeenTotal    = wrTotal; /* still arriving */
                s_discardQuietSinceMs = now;
            }
            else if ((uint32_t)(now - s_discardQuietSinceMs) >= DISCARD_QUIET_MS)
            {
                s_discarding = false;
                cmd_parser_init(&s_parser, &s_io); /* start from an empty line */
            }
            s_rdTotal = wrTotal;
        }

        while (s_rdTotal != wrTotal)
        {
            /* Feed contiguous chunks (handles the wrap at the ring end). */
            uint32_t rd    = s_rdTotal & RX_MASK;
            uint32_t chunk = wrTotal - s_rdTotal;
            if (chunk > (APP_UART_RX_RING_SIZE - rd))
            {
                chunk = APP_UART_RX_RING_SIZE - rd;
            }
            consume(&s_rxRing[rd], chunk);
            s_rdTotal += chunk;

            if (s_flushBeforeReady)
            {
                /* cmd.update just erased the staging area: discard what came in
                 * meanwhile, then invite the image. */
                s_flushBeforeReady = false;
                s_rdTotal          = dma_write_total();
                wrTotal            = s_rdTotal;
                uart_write("ready\r\n", 7U);
            }
        }

        if (s_updateMode && (fw_update_poll(app_now_ms()) != FWU_ACTIVE))
        {
            leave_update_mode(); /* timed out: back in command mode */
        }
        publish_counters();
    }
}

void LPUART3_IRQHandler(void)
{
    uint32_t flags   = LPUART_GetStatusFlags(APP_UART);
    BaseType_t woken = pdFALSE;

    if (flags & (uint32_t)kLPUART_IdleLineFlag)
    {
        (void)LPUART_ClearStatusFlags(APP_UART, kLPUART_IdleLineFlag);
        if (s_uartTask != NULL)
        {
            vTaskNotifyGiveFromISR(s_uartTask, &woken);
        }
    }
    if (flags & (uint32_t)kLPUART_RxOverrunFlag)
    {
        /* Should never happen with DMA; count it and clear so reception
         * continues. */
        s_hwOverruns++;
        (void)LPUART_ClearStatusFlags(APP_UART, kLPUART_RxOverrunFlag);
    }
    portYIELD_FROM_ISR(woken);
    SDK_ISR_EXIT_BARRIER;
}

/* eDMA channel 0 major loop complete = the RX ring wrapped once. */
void DMA0_DMA16_IRQHandler(void)
{
    if ((DMA0->INT & (1UL << APP_UART_DMA_CHANNEL)) != 0U)
    {
        DMA0->CINT = (uint8_t)APP_UART_DMA_CHANNEL;
        s_wraps++;
    }
    SDK_ISR_EXIT_BARRIER;
}

#endif /* UNIT_TEST */
