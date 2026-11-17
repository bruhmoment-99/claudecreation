/*
 * watchdog.c - see watchdog.h.
 */
#include "watchdog.h"
#include "app_config.h"
#include "app_status.h"
#include "watch.h"

#include "fsl_common.h"
#include "fsl_wdog.h"

/* Survives a watchdog or software reset (section not zeroed by startup;
 * see the .noinit section in the linker script). Validated by a check word
 * because after power-on it holds garbage. */
#define NOTE_MAGIC 0x4E4F5445UL /* "NOTE" */

typedef struct
{
    uint32_t magic;
    uint32_t fatalCode;
    uint32_t stalledTask; /* watch_id_t + 1, 0 = none */
    uint32_t check;
} reset_note_t;

__attribute__((section(".noinit"))) static volatile reset_note_t s_note;

static volatile int s_running;
static volatile int s_stallNoted;

static uint32_t note_check(void)
{
    return ~(s_note.magic ^ s_note.fatalCode ^ (s_note.stalledTask << 8));
}

static void note_write(uint32_t fatalCode, uint32_t stalledTask)
{
    s_note.magic       = NOTE_MAGIC;
    s_note.fatalCode   = fatalCode;
    s_note.stalledTask = stalledTask;
    s_note.check       = note_check();
}

void watchdog_read_reset_cause(void)
{
    uint32_t srsr = SRC->SRSR;
    uint16_t wrsr = WDOG1->WRSR;
    const char *cause;

    /* SRSR bits accumulate until cleared, so the specific causes are checked
     * before the power-on bit. */
    if ((srsr & SRC_SRSR_WDOG_RST_B_MASK) != 0U)
    {
        cause = ((wrsr & WDOG_WRSR_TOUT_MASK) != 0U) ? "watchdog" : "software";
    }
    else if ((srsr & SRC_SRSR_LOCKUP_SYSRESETREQ_MASK) != 0U)
    {
        cause = "software-or-lockup";
    }
    else if ((srsr & (SRC_SRSR_JTAG_RST_B_MASK | SRC_SRSR_JTAG_SW_RST_MASK)) != 0U)
    {
        cause = "debugger";
    }
    else if ((srsr & SRC_SRSR_WDOG3_RST_B_MASK) != 0U)
    {
        cause = "watchdog3";
    }
    else if ((srsr & SRC_SRSR_TEMPSENSE_RST_B_MASK) != 0U)
    {
        cause = "over-temperature";
    }
    else if ((srsr & SRC_SRSR_IPP_USER_RESET_B_MASK) != 0U)
    {
        cause = "reset-button";
    }
    else if ((srsr & SRC_SRSR_IPP_RESET_B_MASK) != 0U)
    {
        cause = "power-on";
    }
    else
    {
        cause = "unknown";
    }
    SRC->SRSR = srsr; /* write-1-to-clear */

    uint32_t fatal         = 0U;
    const char *stalled    = NULL;
    bool powerOn           = ((srsr & SRC_SRSR_IPP_RESET_B_MASK) != 0U) && ((srsr & SRC_SRSR_WDOG_RST_B_MASK) == 0U);
    if (!powerOn && (s_note.magic == NOTE_MAGIC) && (s_note.check == note_check()))
    {
        fatal = s_note.fatalCode;
        if ((s_note.stalledTask != 0U) && (s_note.stalledTask <= (uint32_t)WATCH_COUNT))
        {
            stalled = watch_name((watch_id_t)(s_note.stalledTask - 1U));
        }
    }
    note_write(0U, 0U);
    app_status_reset(cause, fatal, stalled);
}

void watchdog_start(void)
{
#if APP_WATCHDOG_ENABLE
    wdog_config_t cfg;
    WDOG_GetDefaultConfig(&cfg);
    cfg.timeoutValue         = (uint16_t)((APP_WDOG_TIMEOUT_MS / 500U) - 1U); /* (WT + 1) * 0.5 s */
    cfg.workMode.enableDebug = false; /* paused while a debugger halts the core */
    cfg.enableInterrupt      = false;
    WDOG_Init(WDOG1, &cfg);
    s_running = 1;
#endif
}

int watchdog_running(void)
{
    return s_running;
}

void watchdog_feed(void)
{
    if (s_running != 0)
    {
        WDOG_Refresh(WDOG1);
    }
}

void watchdog_service(uint32_t nowMs)
{
    watch_id_t late;
    if (watch_all_alive(nowMs, &late))
    {
        watchdog_feed();
        return;
    }
    /* Stop feeding: WDOG1 resets the board within APP_WDOG_TIMEOUT_MS.
     * Leave a note so the next boot can report which task hung. */
    if (s_stallNoted == 0)
    {
        s_stallNoted = 1;
        note_write(s_note.fatalCode, (uint32_t)late + 1U);
    }
}

void watchdog_note_fatal(uint32_t code)
{
    note_write(code, s_note.stalledTask);
}
