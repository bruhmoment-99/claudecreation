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
#include "hid_merge.h"

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
    CMD_BUTTON,  /* left/right/middle/side1/side2: arg = bit mask in table */
    CMD_BUTTONS,
    CMD_RELEASE,
    CMD_SLEEP,
    CMD_VERSION,
    CMD_INFO,
} cmd_kind_t;

typedef struct
{
    const char *name;
    cmd_kind_t kind;
    uint8_t mask; /* CMD_BUTTON only */
} cmd_def_t;

static const cmd_def_t s_commands[] = {
    {"move", CMD_MOVE, 0U},
    {"wheel", CMD_WHEEL, 0U},
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

/* Parse a signed decimal integer occupying [s, e) (surrounding blanks allowed). */
static bool parse_int(const char *s, const char *e, int32_t *out)
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
        if (v > 2147483647LL)
        {
            return false;
        }
    }
    *out = (int32_t)(neg ? -v : v);
    return true;
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
static bool parse_command(const char *s, const char *e, const cmd_def_t **def, int32_t *argv, int *argc)
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
    int32_t v[MAX_ARGS] = {0, 0};
    int argc            = 0;

    /* What a valid command produces besides the echo: */
    const char *strResult = NULL;  /* version / info               */
    bool numResult        = false; /* button queries               */
    uint32_t numValue     = 0U;
    uint32_t sleepMs      = 0U;
    bool doSleep          = false;

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
            if (argc != 2)
            {
                return 0;
            }
            hid_merge_inject_move(v[0], v[1]);
            break;

        case CMD_WHEEL:
            if (argc != 1)
            {
                return 0;
            }
            hid_merge_inject_wheel(v[0]);
            break;

        case CMD_BUTTON:
            if (argc == 0)
            {
                numResult = true;
                numValue  = ((hid_merge_get_logical_buttons() & def->mask) != 0U) ? 1U : 0U;
            }
            else if ((argc == 1) && ((v[0] == 0) || (v[0] == 1)))
            {
                /* Button locking happens in hid_merge: no report unless
                 * (physical | injected) changes. */
                hid_merge_inject_button(def->mask, v[0] == 1);
            }
            else
            {
                return 0;
            }
            break;

        case CMD_BUTTONS:
            if (argc == 0)
            {
                numResult = true;
                numValue  = hid_merge_get_logical_buttons();
            }
            else if ((argc == 1) && (v[0] >= 0) && (v[0] <= 255))
            {
                hid_merge_set_injected_buttons((uint8_t)v[0]);
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
            /* Clears every injected button (left, right, middle, side1, side2
             * and buttons 6-8). Buttons held on the physical mouse stay
             * pressed, so a report is sent only if the logical state changes. */
            hid_merge_set_injected_buttons(0U);
            break;

        case CMD_SLEEP:
            if ((argc != 1) || (v[0] < 0) || ((uint32_t)v[0] > APP_CMD_SLEEP_MAX_MS))
            {
                return 0;
            }
            doSleep = true;
            sleepMs = (uint32_t)v[0];
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

#define APP_UART            LPUART3
#define APP_UART_IRQn       LPUART3_IRQn
#define APP_UART_DMA_SOURCE kDmaRequestMuxLPUART3Rx
#define RX_MASK             (APP_UART_RX_RING_SIZE - 1U)

#if (APP_UART_RX_RING_SIZE & RX_MASK) != 0
#error "APP_UART_RX_RING_SIZE must be a power of two"
#endif

/* DMA target must be non-cacheable (Cortex-M7 D-cache) - BOARD_ConfigMPU()
 * sets up the NonCacheable region this macro places the buffer into. */
AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t s_rxRing[APP_UART_RX_RING_SIZE], 32);

static TaskHandle_t s_uartTask;
static uint32_t s_rd;
static cmd_parser_t s_parser;

static void uart_write(const char *data, size_t len);
static void uart_sleep(uint32_t ms);

static const cmd_io_t s_io = {uart_write, uart_sleep};

static void uart_sleep(uint32_t ms)
{
    /*
     * cmd.sleep(): blocks only this (medium priority) task. The USB host stack
     * task and both USB ISRs keep forwarding the physical mouse, and the eDMA
     * keeps filling the RX ring. Commands that arrive meanwhile run, in order,
     * once the sleep ends. The ring holds APP_UART_RX_RING_SIZE bytes (roughly
     * 200 typical commands); anything beyond that during one sleep is lost.
     * APP_CMD_SLEEP_MAX_MS keeps ms * configTICK_RATE_HZ within 32 bits.
     */
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void uart_write(const char *data, size_t len)
{
    /*
     * Blocking TX is acceptable here: responses are ~20-40 bytes, i.e. 33-67 us
     * at 6 Mbaud, and this task is preempted by the USB host task/ISRs anyway.
     * Switch to LPUART_SendEDMA() on a second DMA channel if responses grow.
     */
    (void)LPUART_WriteBlocking(APP_UART, (const uint8_t *)data, len);
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
        for (;;)
        {
        } /* baud rate not achievable - check clock root */
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
     * and keep the hardware request enabled (DREQ = 0) so it never stops. */
    DMA0->TCD[APP_UART_DMA_CHANNEL].DLAST_SGA = -(int32_t)APP_UART_RX_RING_SIZE;
    DMA0->TCD[APP_UART_DMA_CHANNEL].CSR &= (uint16_t)~DMA_CSR_DREQ_MASK;
    EDMA_EnableChannelRequest(DMA0, APP_UART_DMA_CHANNEL);

    LPUART_EnableRxDMA(APP_UART, true);

    /* IDLE-line IRQ marks the end of a command burst. */
    LPUART_EnableInterrupts(APP_UART, kLPUART_IdleLineInterruptEnable | kLPUART_RxOverrunInterruptEnable);
    NVIC_SetPriority(APP_UART_IRQn, APP_NVIC_PRIO_UART);
    EnableIRQ(APP_UART_IRQn);

    s_rd = 0U;
    cmd_parser_init(&s_parser, &s_io);
}

static inline uint32_t dma_write_index(void)
{
    /* CITER counts down from RING_SIZE; it reloads to RING_SIZE on wrap. */
    uint32_t remaining = EDMA_GetRemainingMajorLoopCount(DMA0, APP_UART_DMA_CHANNEL);
    return (APP_UART_RX_RING_SIZE - remaining) & RX_MASK;
}

void uart_parser_task(void *arg)
{
    (void)arg;
    s_uartTask = xTaskGetCurrentTaskHandle();

    uart_write(">>> ", 4U);

    for (;;)
    {
        /* Woken by the IDLE interrupt; the 1 ms timeout bounds latency when the
         * host streams commands back-to-back with no idle gap. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));

        uint32_t wr = dma_write_index();
        while (s_rd != wr)
        {
            /* Feed contiguous chunks (handles the wrap at the ring end). */
            uint32_t end   = (wr > s_rd) ? wr : APP_UART_RX_RING_SIZE;
            uint32_t chunk = end - s_rd;
            cmd_parser_feed(&s_parser, &s_rxRing[s_rd], chunk);
            s_rd = (s_rd + chunk) & RX_MASK;
        }
    }
}

void LPUART3_IRQHandler(void)
{
    uint32_t flags          = LPUART_GetStatusFlags(APP_UART);
    BaseType_t woken        = pdFALSE;

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
        /* Should never happen with DMA; clear so reception continues. */
        (void)LPUART_ClearStatusFlags(APP_UART, kLPUART_RxOverrunFlag);
    }
    portYIELD_FROM_ISR(woken);
    SDK_ISR_EXIT_BARRIER;
}

#endif /* UNIT_TEST */
