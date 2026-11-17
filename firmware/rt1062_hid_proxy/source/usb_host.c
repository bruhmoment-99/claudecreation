/*
 * usb_host.c - USB2 host: enumerates a HID mouse, parses its report
 * descriptor and streams decoded reports into hid_merge.
 *
 * Controller : USB_OTG2 -> kUSB_ControllerEhci1, IRQ USB_OTG2_IRQn
 * Speed      : HS, FS and LS devices (the RT1062 EHCI has an embedded TT, so
 *              FS/LS mice work without a companion controller)
 * Topology   : a mouse on the port, or behind a hub (USB_HOST_CONFIG_HUB);
 *              one mouse at a time, the first one that works wins
 *
 * Task / context map
 * ------------------
 *   USB_OTG2_IRQHandler            ISR      -> USB_HostEhciIsrFunction(): only
 *                                              sets an OSA event
 *   usb_host_stack_task  (prio 5)  thread   -> USB_HostEhciTaskFunction(): runs
 *                                              ALL host callbacks, including
 *                                              mouse_in_callback() = REAL-TIME
 *                                              MERGE PATH, and the hub driver
 *   usb_host_app_task    (prio 4)  thread   -> VBUS sequencing, enumeration
 *                                              control requests, detach cleanup
 *
 * Choosing the interface
 * ----------------------
 * Wireless dongles and gaming mice often have several HID interfaces
 * (keyboard, mouse, vendor/macro). At attach every HID interface becomes a
 * candidate, in this order: boot-protocol mouse, then HID interfaces with
 * protocol 0, then the rest (some receivers put the mouse collection in the
 * "keyboard" interface). Each candidate's report descriptor is fetched and
 * parsed; the first one that contains a Mouse/Pointer collection is used.
 * A boot-protocol mouse whose descriptor cannot be parsed falls back to the
 * boot report format. Only if no candidate works is an error reported.
 *
 * Per candidate (after the stack has done GET_DESCRIPTOR/SET_ADDRESS/
 * SET_CONFIGURATION and reported kUSB_HostEventEnumerationDone):
 *   USB_HostHidInit -> SET_INTERFACE -> GET_DESCRIPTOR(Report) -> parse ->
 *   SET_IDLE(0) -> SET_PROTOCOL(Report) -> two USB_HostHidRecv() queued
 *
 * Required usb_host_config.h settings (config/usb_host_config.h):
 *   USB_HOST_CONFIG_EHCI                          1U
 *   USB_HOST_CONFIG_HID                           1U
 *   USB_HOST_CONFIG_HUB                           1U  (0 = direct only)
 *   USB_HOST_CONFIG_MAX_TRANSFERS                 16U
 *   USB_HOST_CONFIG_CONFIGURATION_MAX_INTERFACE   5U
 */
#include "usb_host_app.h"
#include "app_config.h"
#include "app_status.h"
#include "board_init.h"
#include "hid_merge.h"
#include "hid_report_parser.h"
#include "status_led.h"
#include "watch.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "fsl_device_registers.h"
#include "board.h"
#include "usb_host_config.h"
#include "usb.h"
#include "usb_host.h"
#include "usb_host_hid.h"
#include "usb_phy.h"
#if (defined(USB_HOST_CONFIG_HUB) && (USB_HOST_CONFIG_HUB > 0U))
#include "usb_host_hub.h" /* must precede usb_host_hub_app.h */
#include "usb_host_hub_app.h"
#endif

#define CONTROLLER_ID   kUSB_ControllerEhci1
#define USB_HOST_IRQn   USB_OTG2_IRQn

#ifndef BOARD_USB_PHY_D_CAL
#define BOARD_USB_PHY_D_CAL     (0x0CU)
#define BOARD_USB_PHY_TXCAL45DP (0x06U)
#define BOARD_USB_PHY_TXCAL45DM (0x06U)
#endif

#define HID_DESCRIPTOR_TYPE        0x21U
#define HID_REPORT_DESCRIPTOR_TYPE 0x22U
#define MAX_CANDIDATES             USB_HOST_CONFIG_CONFIGURATION_MAX_INTERFACE
#define RX_BUFFERS                 2U /* IN transfers kept queued */

typedef enum
{
    kDev_Idle = 0,     /* nothing attached / claimed                     */
    kDev_Attached,     /* enumeration done, app task must start the class */
    kDev_Running,      /* class running                                   */
    kDev_Detached,     /* app task must tear down                         */
} dev_state_t;

/*
 * "Wait" states: a request is in flight, the app task does nothing.
 * "Done" states: the callback finished, the app task issues the next request.
 * The state is always set to the Wait state BEFORE the request is submitted,
 * because the completion callback runs in the higher-priority stack task and
 * can preempt us right after submission.
 */
typedef enum
{
    kRun_Idle = 0,
    kRun_WaitSetInterface,
    kRun_SetInterfaceDone,
    kRun_WaitReportDesc,
    kRun_ReportDescDone,
    kRun_WaitSetIdle,
    kRun_SetIdleDone,
    kRun_WaitSetProtocol,
    kRun_SetProtocolDone,
    kRun_Streaming,
    kRun_NextCandidate, /* this interface is no mouse: try the next one */
    kRun_Error,
} run_state_t;

typedef struct
{
    usb_device_handle deviceHandle;
    usb_host_class_handle classHandle;
    usb_host_configuration_handle configHandle;
    usb_host_interface_t *candidates[MAX_CANDIDATES];
    uint8_t candidateCount;
    uint8_t candidate;          /* index being tried                       */
    volatile dev_state_t devState;
    volatile run_state_t runState;
    bool bootCapable;           /* current interface: boot subclass, mouse */
    bool usingBoot;             /* layout = boot fallback, use BOOT proto  */
    uint16_t reportDescLen;
    uint16_t inPacketSize;
    hid_mouse_layout_t layout;
} host_mouse_t;

static usb_host_handle s_hostHandle;
static host_mouse_t s_mouse;
static TaskHandle_t s_appTask;
static const char *s_lastError; /* why the last candidate was rejected */

/* Buffers the EHCI controller DMAs into -> non-cacheable. */
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_reportDesc[APP_HOST_REPORT_DESC_MAX];
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_rxBuf[RX_BUFFERS][APP_HOST_REPORT_BUF_SIZE];

static inline void notify_app(void)
{
    /* Host callbacks run in usb_host_stack_task (thread context), so the
     * plain (non-FromISR) API is correct here. */
    if (s_appTask != NULL)
    {
        xTaskNotifyGive(s_appTask);
    }
}

static usb_host_interface_t *current_interface(void)
{
    return s_mouse.candidates[s_mouse.candidate];
}

/* ========================================================================= */
/* REAL-TIME PATH: interrupt-IN completion                                   */
/* ========================================================================= */

/*
 * Two IN transfers are always queued on the interrupt pipe (two buffers).
 * When one completes, the other is already waiting in the controller, so a
 * 125 us (8 kHz) mouse is polled again on the next microframe even while
 * this report is being processed. The completed buffer is re-queued only
 * AFTER it has been decoded, so the controller never writes into a buffer
 * that is being read.
 *
 * Note: the SDK's USB_HostHidInPipeCallback() frees the completed transfer
 * only after this callback returns, so the transfer pool
 * (USB_HOST_CONFIG_MAX_TRANSFERS) must hold one more free entry than the
 * number of queued receives, plus those used by control requests.
 */
static APP_HOT void mouse_in_callback(void *param, uint8_t *data, uint32_t dataLength, usb_status_t status)
{
    host_mouse_t *m = (host_mouse_t *)param;

    if ((status == kStatus_USB_TransferCancel) || (m->devState != kDev_Running) ||
        (m->runState != kRun_Streaming))
    {
        return; /* detach or interface change in progress - do not re-arm */
    }

    if ((status == kStatus_USB_Success) && (dataLength != 0U))
    {
        hid_mouse_sample_t smp;
        app_status_mouse_report();
        if (hid_extract_mouse(&m->layout, data, dataLength, &smp))
        {
            hid_merge_physical(smp.buttons, smp.buttonsMask, smp.x, smp.y, smp.wheel, smp.pan);
        }
    }

    /* Re-queue the same buffer; the other one is already in the controller. */
    (void)USB_HostHidRecv(m->classHandle, data, m->inPacketSize, mouse_in_callback, m);
}

/* ========================================================================= */
/* Control-request completion (also runs in the stack task)                  */
/* ========================================================================= */

static void control_callback(void *param, uint8_t *data, uint32_t dataLength, usb_status_t status)
{
    host_mouse_t *m = (host_mouse_t *)param;
    (void)data;

    switch (m->runState)
    {
        case kRun_WaitSetInterface:
            if (status == kStatus_USB_Success)
            {
                m->runState = kRun_SetInterfaceDone;
            }
            else
            {
                s_lastError = "SET_INTERFACE failed";
                m->runState = kRun_NextCandidate;
            }
            break;
        case kRun_WaitReportDesc:
            m->reportDescLen = (uint16_t)dataLength;
            if (status == kStatus_USB_Success)
            {
                m->runState = kRun_ReportDescDone;
            }
            else
            {
                s_lastError = "report descriptor request failed";
                m->runState = kRun_NextCandidate;
            }
            break;
        case kRun_WaitSetIdle:
            /* Many mice STALL SET_IDLE - that is allowed (HID 1.11 7.2.4). */
            m->runState = kRun_SetIdleDone;
            break;
        case kRun_WaitSetProtocol:
            /* Non-boot interfaces may STALL SET_PROTOCOL; they are always in
             * report protocol anyway. */
            m->runState = kRun_SetProtocolDone;
            break;
        default:
            break;
    }
    notify_app();
}

/* ========================================================================= */
/* Host stack event callback (stack task context)                            */
/* ========================================================================= */

/* VID, PID, speed or hub number of an attached device; 0 if unavailable. */
static uint16_t device_info(usb_device_handle deviceHandle, uint32_t what)
{
    uint32_t v = 0U;
    if ((deviceHandle == NULL) ||
        (USB_HostHelperGetPeripheralInformation(deviceHandle, what, &v) != kStatus_USB_Success))
    {
        return 0U;
    }
    return (uint16_t)v;
}

/* Rank of an interface as a mouse candidate; 0 = not HID. */
static uint8_t candidate_rank(const usb_host_interface_t *itf)
{
    uint8_t cls   = itf->interfaceDesc->bInterfaceClass;
    uint8_t proto = itf->interfaceDesc->bInterfaceProtocol;

    if (cls != USB_HOST_HID_CLASS_CODE)
    {
        return 0U;
    }
    if (proto == USB_HOST_HID_PROTOCOL_MOUSE)
    {
        return 3U;
    }
    return (proto == 0U) ? 2U : 1U; /* vendor/report-only, then keyboard */
}

static usb_status_t mouse_event(usb_device_handle deviceHandle, usb_host_configuration_handle configurationHandle,
                                uint32_t eventCode)
{
    usb_status_t status = kStatus_USB_Success;

    switch (eventCode & 0x0000FFFFU)
    {
        case kUSB_HostEventAttach:
        {
            /* Offered one configuration: collect its HID interfaces, best
             * candidates first. */
            usb_host_configuration_t *cfg = (usb_host_configuration_t *)configurationHandle;
            uint8_t n                     = 0U;

            if (s_mouse.devState != kDev_Idle)
            {
                return kStatus_USB_NotSupported; /* one mouse at a time */
            }
            for (uint8_t rank = 3U; rank > 0U; rank--)
            {
                for (uint8_t i = 0U; (i < cfg->interfaceCount) && (n < MAX_CANDIDATES); i++)
                {
                    if (candidate_rank(&cfg->interfaceList[i]) == rank)
                    {
                        s_mouse.candidates[n++] = &cfg->interfaceList[i];
                    }
                }
            }
            if (n == 0U)
            {
                status = kStatus_USB_NotSupported;
                break;
            }
            s_mouse.deviceHandle   = deviceHandle;
            s_mouse.configHandle   = configurationHandle;
            s_mouse.candidateCount = n;
            s_mouse.candidate      = 0U;
            break;
        }

        case kUSB_HostEventEnumerationDone:
            if ((s_mouse.devState == kDev_Idle) && (s_mouse.configHandle == configurationHandle) &&
                (s_mouse.candidateCount > 0U))
            {
                app_status_mouse_attached(device_info(deviceHandle, kUSB_HostGetDeviceVID),
                                          device_info(deviceHandle, kUSB_HostGetDevicePID),
                                          (uint8_t)device_info(deviceHandle, kUSB_HostGetDeviceSpeed),
                                          s_mouse.candidates[0]->interfaceDesc->bInterfaceNumber,
                                          device_info(deviceHandle, kUSB_HostGetDeviceHubNumber) != 0U);
                s_mouse.devState = kDev_Attached;
                notify_app();
            }
            else
            {
                status = kStatus_USB_NotSupported;
            }
            break;

        case kUSB_HostEventDetach:
            if ((s_mouse.devState != kDev_Idle) && (s_mouse.configHandle == configurationHandle))
            {
                s_mouse.devState = kDev_Detached;
                notify_app();
            }
            else
            {
                status = kStatus_USB_NotSupported;
            }
            break;

        case kUSB_HostEventNotSupported:
            /* No configuration offered a HID interface (and no hub). */
            if (s_mouse.devState == kDev_Idle)
            {
                app_status_mouse_unsupported(device_info(deviceHandle, kUSB_HostGetDeviceVID),
                                             device_info(deviceHandle, kUSB_HostGetDevicePID),
                                             (uint8_t)device_info(deviceHandle, kUSB_HostGetDeviceSpeed));
            }
            break;

        case kUSB_HostEventEnumerationFail:
            /* Descriptor requests or SET_ADDRESS failed: cable, power or a
             * device that does not answer. The stack retries on re-plug. */
            if (s_mouse.devState == kDev_Idle)
            {
                app_status_mouse_error("enumeration failed");
            }
            break;

        default:
            break;
    }
    return status;
}

/*
 * The stack has one callback; with hub support it is shared between the hub
 * driver and the mouse. A device/configuration is supported if either of
 * them takes it.
 */
static usb_status_t usb_host_event(usb_device_handle deviceHandle, usb_host_configuration_handle configurationHandle,
                                   uint32_t eventCode)
{
    usb_status_t mouse = mouse_event(deviceHandle, configurationHandle, eventCode);
#if (defined(USB_HOST_CONFIG_HUB) && (USB_HOST_CONFIG_HUB > 0U))
    usb_status_t hub = USB_HostHubDeviceEvent(s_hostHandle, deviceHandle, configurationHandle, eventCode);
    switch (eventCode & 0x0000FFFFU)
    {
        case kUSB_HostEventAttach:
        case kUSB_HostEventEnumerationDone:
        case kUSB_HostEventDetach:
            return ((mouse == kStatus_USB_Success) || (hub == kStatus_USB_Success)) ? kStatus_USB_Success
                                                                                      : kStatus_USB_NotSupported;
        default:
            return kStatus_USB_Success;
    }
#else
    return mouse;
#endif
}

/* ========================================================================= */
/* Enumeration state machine (app task)                                      */
/* ========================================================================= */

/* Find wDescriptorLength of the report descriptor in the HID class descriptor
 * that follows the interface descriptor. */
static uint16_t find_report_desc_len(usb_host_interface_t *itf)
{
    const uint8_t *p = itf->interfaceExtension;
    uint32_t len     = itf->interfaceExtensionLength;
    uint32_t i       = 0U;

    while ((i + 2U) <= len)
    {
        uint8_t bLen  = p[i];
        uint8_t bType = p[i + 1U];
        if (bLen < 2U)
        {
            break;
        }
        if ((bType == HID_DESCRIPTOR_TYPE) && (bLen >= 9U) && ((i + bLen) <= len))
        {
            /* bNumDescriptors at [5], then {bType, wLength} triples from [6]. */
            uint8_t n = p[i + 5U];
            for (uint8_t k = 0U; k < n; k++)
            {
                uint32_t o = i + 6U + 3U * k;
                if ((o + 3U) > (i + bLen))
                {
                    break;
                }
                if (p[o] == HID_REPORT_DESCRIPTOR_TYPE)
                {
                    return (uint16_t)(p[o + 1U] | (p[o + 2U] << 8));
                }
            }
        }
        i += bLen;
    }
    return 0U;
}

static void release_class(void)
{
    if (s_mouse.classHandle != NULL)
    {
        (void)USB_HostHidDeinit(s_mouse.deviceHandle, s_mouse.classHandle);
        s_mouse.classHandle = NULL;
    }
}

static void mouse_teardown(void)
{
    release_class();
    memset(&s_mouse, 0, sizeof(s_mouse));
    s_mouse.devState = kDev_Idle;
    hid_merge_physical_disconnected(); /* release physical-only buttons */
    app_status_mouse_detached();
}

/* reason: string literal shown by cmd.status(). */
static void mouse_fail(const char *reason)
{
    /* Keep the device claimed but idle; it is cleaned up on detach. */
    s_mouse.runState = kRun_Error;
    app_status_mouse_error(reason);
}

/* Bind the HID class to the current candidate and select alternate 0. */
static void start_candidate(void)
{
    host_mouse_t *m          = &s_mouse;
    usb_host_interface_t *it = current_interface();

    m->bootCapable = (it->interfaceDesc->bInterfaceSubClass == USB_HOST_HID_SUBCLASS_CODE_BOOT) &&
                     (it->interfaceDesc->bInterfaceProtocol == USB_HOST_HID_PROTOCOL_MOUSE);
    m->usingBoot   = false;

    if (USB_HostHidInit(m->deviceHandle, &m->classHandle) != kStatus_USB_Success)
    {
        mouse_fail("HID class init failed");
        return;
    }
    /* SET_INTERFACE(alt 0) completes synchronously inside this call. */
    m->runState = kRun_WaitSetInterface;
    if (USB_HostHidSetInterface(m->classHandle, it, 0U, control_callback, m) != kStatus_USB_Success)
    {
        s_lastError = "SET_INTERFACE failed";
        m->runState = kRun_NextCandidate;
        notify_app();
    }
}

/* Current candidate is not a usable mouse: move on, or give up. */
static void next_candidate(void)
{
    host_mouse_t *m = &s_mouse;

    release_class();
    m->candidate++;
    if (m->candidate >= m->candidateCount)
    {
        /* Not a mouse (e.g. a keyboard behind the same hub): report it and
         * let go, so a mouse attached later can still be claimed. */
        const char *why = (m->candidateCount > 1U) ? "no interface has a mouse collection" : s_lastError;
        memset(&s_mouse, 0, sizeof(s_mouse));
        s_mouse.devState = kDev_Idle;
        app_status_mouse_error(why);
        return;
    }
    start_candidate();
}

static void mouse_process(void)
{
    host_mouse_t *m = &s_mouse;

    if (m->devState == kDev_Detached)
    {
        mouse_teardown();
        return;
    }
    if (m->devState == kDev_Attached)
    {
        m->devState = kDev_Running;
        s_lastError = "no mouse collection in report descriptor";
        start_candidate();
        return;
    }
    if (m->devState != kDev_Running)
    {
        return;
    }

    switch (m->runState)
    {
        case kRun_NextCandidate:
            next_candidate();
            break;

        case kRun_SetInterfaceDone:
        {
            uint16_t len = find_report_desc_len(current_interface());
            if ((len == 0U) || (len > sizeof(s_reportDesc)))
            {
                len = sizeof(s_reportDesc); /* device returns what it has */
            }
            /* GET_DESCRIPTOR(Report) -> s_reportDesc */
            m->runState = kRun_WaitReportDesc;
            if (USB_HostHidGetReportDescriptor(m->classHandle, s_reportDesc, len, control_callback, m) !=
                kStatus_USB_Success)
            {
                s_lastError = "report descriptor request failed";
                m->runState = kRun_NextCandidate;
                notify_app();
            }
            break;
        }

        case kRun_ReportDescDone:
            /* Dynamic layout discovery - no hardcoded report format. */
            m->usingBoot = false;
            if (!hid_parse_mouse_layout(s_reportDesc, m->reportDescLen, &m->layout))
            {
                if (!m->bootCapable)
                {
                    s_lastError = "no mouse collection in report descriptor";
                    m->runState = kRun_NextCandidate;
                    notify_app();
                    break;
                }
                /* Unparseable descriptor on a boot mouse interface: switch it
                 * to the spec-defined boot protocol format instead. */
                hid_mouse_layout_boot(&m->layout);
                m->usingBoot = true;
            }
            /* SET_IDLE(duration 0 = report only on change, all report IDs). */
            m->runState = kRun_WaitSetIdle;
            if (USB_HostHidSetIdle(m->classHandle, 0U, 0U, control_callback, m) != kStatus_USB_Success)
            {
                m->runState = kRun_SetIdleDone; /* optional request */
                notify_app();
            }
            break;

        case kRun_SetIdleDone:
        {
            uint8_t proto =
                (uint8_t)(m->usingBoot ? USB_HOST_HID_REQUEST_PROTOCOL_BOOT : USB_HOST_HID_REQUEST_PROTOCOL_REPORT);
            m->runState = kRun_WaitSetProtocol;
            if (USB_HostHidSetProtocol(m->classHandle, proto, control_callback, m) != kStatus_USB_Success)
            {
                m->runState = kRun_SetProtocolDone;
                notify_app();
            }
            break;
        }

        case kRun_SetProtocolDone:
        {
            /* Start streaming. The pipe polls at the mouse's own bInterval
             * (e.g. 1 ms FS, or 125 us for an 8 kHz HS mouse). */
            m->inPacketSize = USB_HostHidGetPacketsize(m->classHandle, USB_ENDPOINT_INTERRUPT, USB_IN);
            if ((m->inPacketSize == 0U) || (m->inPacketSize > APP_HOST_REPORT_BUF_SIZE))
            {
                m->inPacketSize = APP_HOST_REPORT_BUF_SIZE;
            }
            /* Publish before the first report can arrive. */
            app_status_mouse_attached(device_info(m->deviceHandle, kUSB_HostGetDeviceVID),
                                      device_info(m->deviceHandle, kUSB_HostGetDevicePID),
                                      (uint8_t)device_info(m->deviceHandle, kUSB_HostGetDeviceSpeed),
                                      current_interface()->interfaceDesc->bInterfaceNumber,
                                      device_info(m->deviceHandle, kUSB_HostGetDeviceHubNumber) != 0U);
            app_status_mouse_ready(&m->layout, m->usingBoot, m->inPacketSize);
            m->runState = kRun_Streaming;

            uint8_t queued = 0U;
            for (uint8_t b = 0U; b < RX_BUFFERS; b++)
            {
                if (USB_HostHidRecv(m->classHandle, s_rxBuf[b], m->inPacketSize, mouse_in_callback, m) ==
                    kStatus_USB_Success)
                {
                    queued++;
                }
            }
            if (queued == 0U)
            {
                mouse_fail("interrupt IN transfer failed");
            }
            break;
        }

        default:
            break; /* waiting for a callback, streaming, or error */
    }
}

/* ========================================================================= */
/* Tasks, ISR, init                                                          */
/* ========================================================================= */

void USB_OTG2_IRQHandler(void)
{
    USB_HostEhciIsrFunction(s_hostHandle);
    SDK_ISR_EXIT_BARRIER;
}

static void usb_host_stack_task(void *arg)
{
    /* Blocks on the OSA event set by the ISR, then processes completed qTDs
     * and invokes callbacks (incl. mouse_in_callback) in this context. This is
     * the "high priority host IN task" of the architecture. */
    for (;;)
    {
        USB_HostEhciTaskFunction(arg);
    }
}

static void usb_host_clock_init(void)
{
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };

    /* USB2 PLL (PLL7) + controller clock, then PHY power-up/calibration.
     * Identical to USB_HostClockInit() in the SDK examples for EHCI1. */
    CLOCK_EnableUsbhs1PhyPllClock(kCLOCK_Usbphy480M, 480000000U);
    CLOCK_EnableUsbhs1Clock(kCLOCK_Usb480M, 480000000U);
    USB_EhciPhyInit(CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}

static void vbus_power_sequence(void)
{
    /* GPIO was initialised LOW in board_init(). Keep it low long enough for a
     * device left powered by a previous run/reset to fully discharge and reset,
     * then enable the AP2553 and let VBUS settle before the stack starts
     * looking for a connect. The stack additionally debounces the attach. */
    board_vbus_enable(false);
    vTaskDelay(pdMS_TO_TICKS(APP_VBUS_OFF_TIME_MS));
    board_vbus_enable(true);
    vTaskDelay(pdMS_TO_TICKS(APP_VBUS_SETTLE_MS));
}

void usb_host_app_task(void *arg)
{
    (void)arg;
    s_appTask = xTaskGetCurrentTaskHandle();
    memset(&s_mouse, 0, sizeof(s_mouse));
    watch_checkin(WATCH_USB_HOST, app_now_ms());

    /* 1) VBUS must be HIGH before the host stack is initialised. */
    vbus_power_sequence();

    /* 2) Clocks/PHY and host stack. */
    usb_host_clock_init();
    if (USB_HostInit(CONTROLLER_ID, &s_hostHandle, usb_host_event) != kStatus_USB_Success)
    {
        /* USB1 and the command channel keep working; the LED shows a fault. */
        app_status_host_init_failed();
        watch_park(WATCH_USB_HOST);
        for (;;)
        {
            vTaskDelay(portMAX_DELAY);
        }
    }

    NVIC_SetPriority(USB_HOST_IRQn, APP_NVIC_PRIO_USB_HOST);
    EnableIRQ(USB_HOST_IRQn);

    /* 3) High-priority task running the host controller driver. */
    (void)xTaskCreate(usb_host_stack_task, "usbh_stack", APP_STACK_USB_HOST_STACK, s_hostHandle,
                      APP_PRIO_USB_HOST_STACK, NULL);

    /* 4) Enumeration state machine + VBUS fault supervision. */
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        watch_checkin(WATCH_USB_HOST, app_now_ms());

        mouse_process();

        if (board_vbus_fault())
        {
            /* AP2553 already limits current and deglitches FLG. Power-cycle
             * the port; the stack will see a detach and re-enumerate. */
            app_status_vbus_fault(app_now_ms());
            board_vbus_enable(false);
            vTaskDelay(pdMS_TO_TICKS(500));
            board_vbus_enable(true);
        }
    }
}
