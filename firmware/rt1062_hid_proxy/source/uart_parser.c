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

static void put_str(cmd_write_fn_t w, const char *s)
{
    w(s, strlen(s));
}

static void put_int(cmd_write_fn_t w, int32_t v)
{
    char buf[12];
    int i      = (int)sizeof(buf);
    uint32_t u = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do
    {
        buf[--i] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);
    if (v < 0)
    {
        buf[--i] = '-';
    }
    w(&buf[i], sizeof(buf) - (size_t)i);
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

typedef enum
{
    CMD_MOVE,
    CMD_LEFT,
    CMD_RIGHT,
    CMD_MIDDLE,
    CMD_WHEEL,
    CMD_UNKNOWN
} cmd_id_t;

static cmd_id_t lookup(const char *name, size_t n)
{
    static const struct
    {
        const char *name;
        cmd_id_t id;
    } table[] = {
        {"move", CMD_MOVE}, {"left", CMD_LEFT}, {"right", CMD_RIGHT}, {"middle", CMD_MIDDLE}, {"wheel", CMD_WHEEL},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        if ((strlen(table[i].name) == n) && (memcmp(table[i].name, name, n) == 0))
        {
            return table[i].id;
        }
    }
    return CMD_UNKNOWN;
}

/*
 * Execute one command line (without the line terminator).
 * Returns via write(): echo + optional result + prompt.
 */
void cmd_execute_line(const char *line, size_t len, cmd_write_fn_t write)
{
    const char *s = line;
    const char *e = line + len;
    bool ok       = false;
    bool isQuery  = false;
    int32_t queryValue = 0;

    /* trim */
    while ((s < e) && is_space(*s))
    {
        s++;
    }
    while ((e > s) && is_space(e[-1]))
    {
        e--;
    }
    if (s == e)
    {
        put_str(write, ">>> "); /* empty line: just re-prompt */
        return;
    }

    do
    {
        if (((size_t)(e - s) < 4U) || (memcmp(s, "cmd.", 4) != 0))
        {
            break;
        }
        const char *name = s + 4;
        const char *lp   = memchr(name, '(', (size_t)(e - name));
        if ((lp == NULL) || (e[-1] != ')'))
        {
            break;
        }
        const char *rp = e - 1;
        cmd_id_t id    = lookup(name, (size_t)(lp - name));

        /* split args on ',' */
        const char *argS[MAX_ARGS];
        const char *argE[MAX_ARGS];
        int argc        = 0;
        const char *a   = lp + 1;
        bool tooMany    = false;
        bool blank      = true;
        for (const char *c = a; c < rp; c++)
        {
            if (!is_space(*c))
            {
                blank = false;
                break;
            }
        }
        if (!blank)
        {
            for (const char *c = a;; c++)
            {
                if ((c == rp) || (*c == ','))
                {
                    if (argc == MAX_ARGS)
                    {
                        tooMany = true;
                        break;
                    }
                    argS[argc] = a;
                    argE[argc] = c;
                    argc++;
                    a = c + 1;
                    if (c == rp)
                    {
                        break;
                    }
                }
            }
        }
        if (tooMany)
        {
            break;
        }

        int32_t v[MAX_ARGS] = {0, 0};
        bool argsOk         = true;
        for (int k = 0; k < argc; k++)
        {
            argsOk = argsOk && parse_int(argS[k], argE[k], &v[k]);
        }
        if (!argsOk)
        {
            break;
        }

        switch (id)
        {
            case CMD_MOVE:
                if (argc == 2)
                {
                    hid_merge_inject_move(v[0], v[1]);
                    ok = true;
                }
                break;

            case CMD_WHEEL:
                if (argc == 1)
                {
                    hid_merge_inject_wheel(v[0]);
                    ok = true;
                }
                break;

            case CMD_LEFT:
            case CMD_RIGHT:
            case CMD_MIDDLE:
            {
                uint8_t mask = (id == CMD_LEFT) ? HID_BTN_LEFT : ((id == CMD_RIGHT) ? HID_BTN_RIGHT : HID_BTN_MIDDLE);
                if (argc == 0)
                {
                    isQuery    = true;
                    queryValue = ((hid_merge_get_logical_buttons() & mask) != 0U) ? 1 : 0;
                    ok         = true;
                }
                else if ((argc == 1) && ((v[0] == 0) || (v[0] == 1)))
                {
                    /* Button locking is enforced inside hid_merge: a report is
                     * only sent if (physical | injected) actually changes. */
                    hid_merge_inject_button(mask, v[0] == 1);
                    ok = true;
                }
                break;
            }

            default:
                break;
        }
    } while (0);

    /* Echo exactly what was received (trimmed), then result and prompt. */
    write(s, (size_t)(e - s));
    if (!ok)
    {
        put_str(write, "\r\nERR");
    }
    else if (isQuery)
    {
        put_str(write, "\r\n");
        put_int(write, queryValue);
    }
    put_str(write, "\r\n>>> ");
}

void cmd_parser_init(cmd_parser_t *p, cmd_write_fn_t write)
{
    memset(p, 0, sizeof(*p));
    p->write = write;
}

void cmd_parser_feed(cmd_parser_t *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = (char)data[i];
        if ((c == '\r') || (c == '\n'))
        {
            /* "\r\n": the '\r' completes the line, the '\n' then sees an empty
             * buffer and is ignored (so no extra prompt is printed). */
            if (p->overflow)
            {
                put_str(p->write, "ERR\r\n>>> ");
            }
            else if (p->len > 0U)
            {
                cmd_execute_line(p->line, p->len, p->write);
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
    cmd_parser_init(&s_parser, uart_write);
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
