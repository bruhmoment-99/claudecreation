/*
 * app_status.c - Link/mouse status registry, LED pattern and the text
 * printed by cmd.status(). Pure C; see app_status.h.
 */
#include "app_status.h"

#include <string.h>

static app_status_t s_status;

void app_status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
}

const app_status_t *app_status_get(void)
{
    return &s_status;
}

/* ========================================================================= */
/* Writers                                                                   */
/* ========================================================================= */

void app_status_pc_speed(uint8_t speed)
{
    s_status.pcSpeed = speed;
}

void app_status_pc_configured(bool configured)
{
    s_status.pcConfigured = configured;
}

void app_status_pc_report_sent(void)
{
    s_status.pcReports++;
}

void app_status_pc_boot_protocol(bool boot)
{
    s_status.pcBootProtocol = boot;
}

void app_status_pc_suspended(bool suspended)
{
    s_status.pcSuspended = suspended;
}

void app_status_pc_remote_wake(bool enabled)
{
    s_status.pcRemoteWake = enabled;
}

void app_status_pc_wakeup(void)
{
    s_status.pcWakeups++;
}

void app_status_uart(uint32_t rxBytes, uint32_t overflows, uint32_t lostBytes, uint32_t hwOverruns)
{
    s_status.uartRxBytes    = rxBytes;
    s_status.uartOverflows  = overflows;
    s_status.uartLostBytes  = lostBytes;
    s_status.uartHwOverruns = hwOverruns;
}

void app_status_settings(uint16_t smoothMs, uint32_t saves, uint32_t errors)
{
    s_status.settingsSmooth = smoothMs;
    s_status.settingsSaves  = saves;
    s_status.settingsErrors = errors;
}

void app_status_reset(const char *cause, uint32_t fatalCode, const char *stalledTask)
{
    s_status.resetCause       = cause;
    s_status.resetFatalCode   = fatalCode;
    s_status.resetStalledTask = stalledTask;
}

void app_status_mouse_attached(uint16_t vid, uint16_t pid, uint8_t speed, uint8_t interfaceNumber, bool viaHub)
{
    s_status.mouseState        = APP_MOUSE_ENUMERATING;
    s_status.mouseError        = NULL;
    s_status.mouseVid          = vid;
    s_status.mousePid          = pid;
    s_status.mouseSpeed        = speed;
    s_status.mouseInterface    = interfaceNumber;
    s_status.mouseViaHub       = viaHub;
    s_status.mouseBootProtocol = false;
    s_status.mousePacketSize   = 0U;
    s_status.mouseReports      = 0U;
    memset(&s_status.layout, 0, sizeof(s_status.layout));
}

void app_status_mouse_unsupported(uint16_t vid, uint16_t pid, uint8_t speed)
{
    app_status_mouse_attached(vid, pid, speed, 0U, false);
    s_status.mouseState = APP_MOUSE_UNSUPPORTED;
}

void app_status_mouse_ready(const hid_mouse_layout_t *layout, bool bootProtocol, uint16_t packetSize)
{
    s_status.layout            = *layout;
    s_status.mouseBootProtocol = bootProtocol;
    s_status.mousePacketSize   = packetSize;
    s_status.mouseState        = APP_MOUSE_READY; /* last: readers check it */
}

void app_status_mouse_error(const char *reason)
{
    s_status.mouseError = reason;
    s_status.mouseState = APP_MOUSE_ERROR;
}

void app_status_mouse_detached(void)
{
    s_status.mouseState = APP_MOUSE_NONE;
    s_status.mouseError = NULL;
}

void app_status_mouse_report(void)
{
    s_status.mouseReports++;
}

void app_status_host_init_failed(void)
{
    s_status.hostInitFailed = true;
}

void app_status_vbus_fault(uint32_t nowMs)
{
    s_status.vbusFaultMs = nowMs;
    s_status.vbusFaults++;
}

/* ========================================================================= */
/* LED                                                                       */
/* ========================================================================= */

static bool vbus_fault_recent(const app_status_t *s, uint32_t nowMs)
{
    /* Unsigned subtraction handles the 49-day wrap of the ms counter. */
    return (s->vbusFaults != 0U) && ((uint32_t)(nowMs - s->vbusFaultMs) < APP_STATUS_VBUS_FAULT_HOLD_MS);
}

bool app_status_has_fault(const app_status_t *s, uint32_t nowMs)
{
    return s->hostInitFailed || vbus_fault_recent(s, nowMs) || (s->mouseState == APP_MOUSE_ERROR) ||
           (s->mouseState == APP_MOUSE_UNSUPPORTED);
}

#define LED_PERIOD_MS 2000U
#define LED_BLIP_MS   60U

bool app_status_led_on(const app_status_t *s, uint32_t nowMs)
{
    uint32_t t     = nowMs % LED_PERIOD_MS;
    bool mouseOk   = (s->mouseState == APP_MOUSE_READY);

    if (app_status_has_fault(s, nowMs))
    {
        return (nowMs % 200U) < 100U; /* 5 Hz */
    }
    if (s->pcConfigured && s->pcSuspended)
    {
        return false; /* PC asleep */
    }
    if (s->pcConfigured)
    {
        return mouseOk ? true : ((nowMs % 1000U) < 500U);
    }
    if (mouseOk)
    {
        /* two blips: 0..60 ms and 240..300 ms */
        return (t < LED_BLIP_MS) || ((t >= 240U) && (t < 240U + LED_BLIP_MS));
    }
    return t < LED_BLIP_MS;
}

/* ========================================================================= */
/* cmd.status() text                                                         */
/* ========================================================================= */

typedef void (*write_fn)(const char *data, size_t len);

static void put(write_fn w, const char *s)
{
    w(s, strlen(s));
}

static void put_dec(write_fn w, uint32_t u)
{
    char buf[10];
    size_t i = sizeof(buf);
    do
    {
        buf[--i] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);
    w(&buf[i], sizeof(buf) - i);
}

static void put_hex(write_fn w, uint32_t u, unsigned digits)
{
    static const char hex[] = "0123456789abcdef";
    char buf[8];
    for (unsigned i = 0U; i < digits; i++)
    {
        buf[digits - 1U - i] = hex[u & 0xFU];
        u >>= 4;
    }
    w(buf, digits);
}

static const char *speed_name(uint8_t speed)
{
    switch (speed)
    {
        case APP_USB_SPEED_HIGH:
            return "high-speed";
        case APP_USB_SPEED_LOW:
            return "low-speed";
        default:
            return "full-speed";
    }
}

/* " x=s16@8" (+ "#<id>" when the field's report ID differs from refId). */
static void put_field(write_fn w, const char *name, const hid_field_t *f, bool usesId, uint8_t refId)
{
    put(w, " ");
    put(w, name);
    if (!f->present)
    {
        put(w, "=none");
        return;
    }
    put(w, f->isSigned ? "=s" : "=u");
    put_dec(w, f->bitSize);
    put(w, "@");
    put_dec(w, f->bitOffset);
    if (usesId && (f->reportId != refId))
    {
        put(w, "#");
        put_dec(w, f->reportId);
    }
}

static void print_layout(write_fn w, const app_status_t *s)
{
    const hid_mouse_layout_t *l = &s->layout;

    put(w, "layout:");
    if (s->mouseState != APP_MOUSE_READY)
    {
        put(w, " -");
        return;
    }
    uint8_t refId = l->x.reportId;
    uint16_t arrMask = 0U;
    for (uint32_t k = 0U; k < HID_MAX_BUTTON_ARRAYS; k++)
    {
        arrMask |= hid_button_array_mask(&l->arrays[k]);
    }
    if (l->usesReportId)
    {
        put(w, " report-id=");
        put_dec(w, refId);
    }
    if (s->mouseBootProtocol)
    {
        put(w, " boot");
    }

    /* buttons=5(0x001f)@0 : count, mask, bit offset of the lowest button */
    uint16_t mask = l->buttons.mask;
    put(w, " buttons=");
    if ((mask == 0U) && (arrMask == 0U))
    {
        put(w, "none");
    }
    else if (mask != 0U)
    {
        uint32_t count = 0U;
        uint8_t first  = 0U;
        for (uint8_t i = HID_MAX_MAPPED_BUTTONS; i > 0U; i--)
        {
            if ((mask & (1U << (i - 1U))) != 0U)
            {
                count++;
                first = (uint8_t)(i - 1U);
            }
        }
        put_dec(w, count);
        put(w, "(0x");
        put_hex(w, mask, 4U);
        put(w, ")@");
        put_dec(w, l->buttons.bitOffset[first]);
        if (l->usesReportId && (l->buttons.reportId[first] != refId))
        {
            put(w, "#");
            put_dec(w, l->buttons.reportId[first]);
        }
    }
    /* array=1-16:6x8@8 : button range, count x bits, bit offset */
    for (uint32_t k = 0U; k < HID_MAX_BUTTON_ARRAYS; k++)
    {
        const hid_button_array_t *a = &l->arrays[k];
        if (!a->present)
        {
            continue;
        }
        put(w, (mask == 0U) && (k == 0U) ? "array=" : " array=");
        put_dec(w, a->usageMin);
        put(w, "-");
        put_dec(w, a->usageMax);
        put(w, ":");
        put_dec(w, a->count);
        put(w, "x");
        put_dec(w, a->bitSize);
        put(w, "@");
        put_dec(w, a->bitOffset);
        if (l->usesReportId && (a->reportId != refId))
        {
            put(w, "#");
            put_dec(w, a->reportId);
        }
    }
    put_field(w, "x", &l->x, l->usesReportId, refId);
    put_field(w, "y", &l->y, l->usesReportId, refId);
    put_field(w, "wheel", &l->wheel, l->usesReportId, refId);
    put_field(w, "pan", &l->pan, l->usesReportId, refId);
}

static void print_mouse(write_fn w, const app_status_t *s)
{
    static const char *const names[] = {"none", "enumerating", "ready", "unsupported", "error"};
    app_mouse_state_t st = s->mouseState;

    put(w, "mouse: ");
    put(w, ((unsigned)st < (sizeof(names) / sizeof(names[0]))) ? names[st] : "?");
    if (st == APP_MOUSE_NONE)
    {
        return;
    }
    if ((st == APP_MOUSE_ERROR) && (s->mouseError != NULL))
    {
        put(w, " (");
        put(w, s->mouseError);
        put(w, ")");
    }
    put(w, " ");
    put(w, speed_name(s->mouseSpeed));
    put(w, " vid=");
    put_hex(w, s->mouseVid, 4U);
    put(w, " pid=");
    put_hex(w, s->mousePid, 4U);
    if (st == APP_MOUSE_UNSUPPORTED)
    {
        return;
    }
    put(w, " interface=");
    put_dec(w, s->mouseInterface);
    if (s->mouseViaHub)
    {
        put(w, " via-hub");
    }
    if (st == APP_MOUSE_READY)
    {
        put(w, " packet=");
        put_dec(w, s->mousePacketSize);
        put(w, " reports=");
        put_dec(w, s->mouseReports);
    }
}

static void print_faults(write_fn w, const app_status_t *s, uint32_t nowMs)
{
    bool any = false;

    put(w, "faults:");
    if (s->hostInitFailed)
    {
        put(w, " host-init-failed");
        any = true;
    }
    if (s->vbusFaults != 0U)
    {
        put(w, " vbus-overcurrent=");
        put_dec(w, s->vbusFaults);
        if (vbus_fault_recent(s, nowMs))
        {
            put(w, "(active)");
        }
        any = true;
    }
    if (s->mouseState == APP_MOUSE_UNSUPPORTED)
    {
        put(w, " unsupported-device");
        any = true;
    }
    if (s->mouseState == APP_MOUSE_ERROR)
    {
        put(w, " mouse-error");
        any = true;
    }
    if (!any)
    {
        put(w, " none");
    }
}

void app_status_print(const app_status_t *s, uint32_t nowMs, void (*write)(const char *data, size_t len))
{
    put(write, "pc: ");
    if (s->pcConfigured)
    {
        put(write, "configured ");
        put(write, speed_name(s->pcSpeed));
    }
    else
    {
        put(write, "not-configured");
    }
    put(write, " reports=");
    put_dec(write, s->pcReports);
    if (s->pcBootProtocol)
    {
        put(write, " boot-protocol");
    }
    if (s->pcSuspended)
    {
        put(write, " suspended");
    }
    if (s->pcRemoteWake)
    {
        put(write, " remote-wake");
    }
    if (s->pcWakeups != 0U)
    {
        put(write, " wakeups=");
        put_dec(write, s->pcWakeups);
    }
    put(write, "\r\n");

    print_mouse(write, s);
    put(write, "\r\n");

    print_layout(write, s);
    put(write, "\r\n");

    put(write, "uart: rx=");
    put_dec(write, s->uartRxBytes);
    put(write, " overflows=");
    put_dec(write, s->uartOverflows);
    put(write, " lost=");
    put_dec(write, s->uartLostBytes);
    if (s->uartHwOverruns != 0U)
    {
        put(write, " fifo-overruns=");
        put_dec(write, s->uartHwOverruns);
    }
    put(write, "\r\n");

    put(write, "settings: smooth=");
    put_dec(write, s->settingsSmooth);
    put(write, " saves=");
    put_dec(write, s->settingsSaves);
    if (s->settingsErrors != 0U)
    {
        put(write, " write-errors=");
        put_dec(write, s->settingsErrors);
    }
    put(write, "\r\n");

    put(write, "reset: ");
    put(write, (s->resetCause != NULL) ? s->resetCause : "unknown");
    if (s->resetStalledTask != NULL)
    {
        put(write, " (task ");
        put(write, s->resetStalledTask);
        put(write, " stopped responding)");
    }
    if (s->resetFatalCode != 0U)
    {
        put(write, " after-fatal=0x");
        put_hex(write, s->resetFatalCode, 8U);
    }
    put(write, "\r\n");

    print_faults(write, s, nowMs);
}
