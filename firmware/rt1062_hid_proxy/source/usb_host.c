/*
 * usb_host.c - USB2 host: enumerates a HID mouse, parses its report
 * descriptor and streams decoded reports into hid_merge.
 *
 * Controller : USB_OTG2 -> kUSB_ControllerEhci1, IRQ USB_OTG2_IRQn
 * Speed      : HS, FS and LS devices (the RT1062 EHCI has an embedded TT, so
 *              FS/LS mice work without a companion controller)
 *
 * Task / context map
 * ------------------
 *   USB_OTG2_IRQHandler            ISR      -> USB_HostEhciIsrFunction(): only
 *                                              sets an OSA event
 *   usb_host_stack_task  (prio 5)  thread   -> USB_HostEhciTaskFunction(): runs
 *                                              ALL host callbacks, including
 *                                              mouse_in_callback() = REAL-TIME
 *                                              MERGE PATH
 *   usb_host_app_task    (prio 4)  thread   -> VBUS sequencing, enumeration
 *                                              control requests, detach cleanup
 *
 * Enumeration sequence (after the stack has done GET_DESCRIPTOR/SET_ADDRESS/
 * SET_CONFIGURATION and reported kUSB_HostEventEnumerationDone):
 *   USB_HostHidInit -> SET_INTERFACE -> GET_DESCRIPTOR(Report) -> parse ->
 *   SET_IDLE(0) -> SET_PROTOCOL(Report) -> USB_HostHidRecv() loop
 *
 * Required usb_host_config.h settings (copy the SDK template and set):
 *   USB_HOST_CONFIG_EHCI                          1U
 *   USB_HOST_CONFIG_HID                           1U
 *   USB_HOST_CONFIG_MAX_HOST                      1U
 *   USB_HOST_CONFIG_MAX_PIPES                     16U
 *   USB_HOST_CONFIG_MAX_TRANSFERS                 16U
 *   USB_HOST_CONFIG_INTERFACE_MAX_EP              4U
 *   USB_HOST_CONFIG_CONFIGURATION_MAX_INTERFACE   5U
 *   USB_HOST_CONFIG_EHCI_FRAME_LIST_SIZE          1024U
 *   USB_HOST_CONFIG_EHCI_MAX_QH / _QTD            8U / 8U
 */
#include "usb_host_app.h"
#include "app_config.h"
#include "app_status.h"
#include "board_init.h"
#include "hid_merge.h"
#include "hid_report_parser.h"
#include "status_led.h"

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

#define CONTROLLER_ID   kUSB_ControllerEhci1
#define USB_HOST_IRQn   USB_OTG2_IRQn

#ifndef BOARD_USB_PHY_D_CAL
#define BOARD_USB_PHY_D_CAL     (0x0CU)
#define BOARD_USB_PHY_TXCAL45DP (0x06U)
#define BOARD_USB_PHY_TXCAL45DM (0x06U)
#endif

#define HID_DESCRIPTOR_TYPE        0x21U
#define HID_REPORT_DESCRIPTOR_TYPE 0x22U

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
    kRun_Error,
} run_state_t;

typedef struct
{
    usb_device_handle deviceHandle;
    usb_host_class_handle classHandle;
    usb_host_interface_handle interfaceHandle;
    usb_host_configuration_handle configHandle;
    volatile dev_state_t devState;
    volatile run_state_t runState;
    bool bootCapable;           /* interface subclass == boot             */
    bool usingBoot;             /* layout = boot fallback, use BOOT proto */
    uint16_t reportDescLen;
    uint16_t inPacketSize;
    uint8_t rxIndex;            /* double buffer index                    */
    hid_mouse_layout_t layout;
} host_mouse_t;

static usb_host_handle s_hostHandle;
static host_mouse_t s_mouse;
static TaskHandle_t s_appTask;

/* Buffers the EHCI controller DMAs into -> non-cacheable. */
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_reportDesc[APP_HOST_REPORT_DESC_MAX];
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_rxBuf[2][APP_HOST_REPORT_BUF_SIZE];

static inline void notify_app(void)
{
    /* Host callbacks run in usb_host_stack_task (thread context), so the
     * plain (non-FromISR) API is correct here. */
    if (s_appTask != NULL)
    {
        xTaskNotifyGive(s_appTask);
    }
}

/* ========================================================================= */
/* REAL-TIME PATH: interrupt-IN completion                                   */
/* ========================================================================= */

static APP_HOT void mouse_in_callback(void *param, uint8_t *data, uint32_t dataLength, usb_status_t status)
{
    host_mouse_t *m = (host_mouse_t *)param;

    if ((status == kStatus_USB_TransferCancel) || (m->devState != kDev_Running))
    {
        return; /* detach in progress - do not re-arm */
    }

    /*
     * 1) Re-arm FIRST, into the other buffer, so the next poll of the mouse is
     *    already queued while we process this report (minimises the gap at
     *    8 kHz HS mice: the next interval is only 125 us away).
     *    Note: the SDK's USB_HostHidInPipeCallback() frees the completed
     *    transfer only AFTER this callback returns, so the transfer pool
     *    (USB_HOST_CONFIG_MAX_TRANSFERS) must hold at least 2 free entries
     *    beyond those used for control requests.
     */
    uint8_t next = (uint8_t)(m->rxIndex ^ 1U);
    m->rxIndex   = next;
    (void)USB_HostHidRecv(m->classHandle, s_rxBuf[next], m->inPacketSize, mouse_in_callback, m);

    if ((status != kStatus_USB_Success) || (dataLength == 0U))
    {
        return;
    }
    app_status_mouse_report();

    /*
     * 2) Decode using the layout derived from the report descriptor, then
     * 3) merge with injection state and send to the PC (hid_merge does steps
     *    1-4 of the data-flow spec: add pending dx/dy/wheel, OR the buttons,
     *    USB_DeviceHidSend on USB1, clear delivered motion).
     */
    hid_mouse_sample_t smp;
    if (hid_extract_mouse(&m->layout, data, dataLength, &smp))
    {
        hid_merge_physical(smp.buttons, smp.buttonsValid, smp.x, smp.y, smp.wheel);
    }
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
            m->runState = (status == kStatus_USB_Success) ? kRun_SetInterfaceDone : kRun_Error;
            if (m->runState == kRun_Error)
            {
                app_status_mouse_error("SET_INTERFACE failed");
            }
            break;
        case kRun_WaitReportDesc:
            m->reportDescLen = (uint16_t)dataLength;
            m->runState      = (status == kStatus_USB_Success) ? kRun_ReportDescDone : kRun_Error;
            if (m->runState == kRun_Error)
            {
                app_status_mouse_error("report descriptor request failed");
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

/* VID, PID or speed of an attached device; 0 if unavailable. */
static uint16_t device_info(usb_device_handle deviceHandle, uint32_t what)
{
    uint32_t v = 0U;
    if ((deviceHandle == NULL) || (USB_HostHelperGetPeripheralInformation(deviceHandle, what, &v) != kStatus_USB_Success))
    {
        return 0U;
    }
    return (uint16_t)v;
}

static usb_status_t usb_host_event(usb_device_handle deviceHandle, usb_host_configuration_handle configurationHandle,
                                   uint32_t eventCode)
{
    usb_status_t status = kStatus_USB_Success;

    switch (eventCode & 0x0000FFFFU)
    {
        case kUSB_HostEventAttach:
        {
            /* Offered one configuration: claim a HID mouse interface.
             * Preference: protocol == mouse; otherwise the first HID interface
             * with protocol 0 (many gaming mice) - its report descriptor is
             * verified to contain a mouse collection later. */
            usb_host_configuration_t *cfg = (usb_host_configuration_t *)configurationHandle;
            usb_host_interface_t *pick    = NULL;
            bool boot                     = false;

            if (s_mouse.devState != kDev_Idle)
            {
                return kStatus_USB_NotSupported; /* one mouse at a time */
            }
            for (uint8_t i = 0U; i < cfg->interfaceCount; i++)
            {
                usb_host_interface_t *itf = &cfg->interfaceList[i];
                uint8_t cls               = itf->interfaceDesc->bInterfaceClass;
                uint8_t sub               = itf->interfaceDesc->bInterfaceSubClass;
                uint8_t proto             = itf->interfaceDesc->bInterfaceProtocol;

                if (cls != USB_HOST_HID_CLASS_CODE)
                {
                    continue;
                }
                if (proto == USB_HOST_HID_PROTOCOL_MOUSE)
                {
                    pick = itf;
                    boot = (sub == USB_HOST_HID_SUBCLASS_CODE_BOOT);
                    break;
                }
                if ((pick == NULL) && (sub == USB_HOST_HID_SUBCLASS_CODE_NONE) && (proto == 0U))
                {
                    pick = itf;
                }
            }
            if (pick == NULL)
            {
                status = kStatus_USB_NotSupported;
                break;
            }
            s_mouse.deviceHandle    = deviceHandle;
            s_mouse.configHandle    = configurationHandle;
            s_mouse.interfaceHandle = pick;
            s_mouse.bootCapable     = boot;
            app_status_mouse_attached(device_info(deviceHandle, kUSB_HostGetDeviceVID),
                                      device_info(deviceHandle, kUSB_HostGetDevicePID),
                                      (uint8_t)device_info(deviceHandle, kUSB_HostGetDeviceSpeed),
                                      pick->interfaceDesc->bInterfaceNumber);
            break;
        }

        case kUSB_HostEventEnumerationDone:
            if ((s_mouse.devState == kDev_Idle) && (s_mouse.configHandle == configurationHandle))
            {
                s_mouse.devState = kDev_Attached;
                notify_app();
            }
            break;

        case kUSB_HostEventDetach:
            if ((s_mouse.devState != kDev_Idle) && (s_mouse.configHandle == configurationHandle))
            {
                s_mouse.devState = kDev_Detached;
                notify_app();
            }
            break;

        case kUSB_HostEventNotSupported:
            /* No configuration offered a HID interface we can use. */
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

static void mouse_teardown(void)
{
    if (s_mouse.classHandle != NULL)
    {
        (void)USB_HostHidDeinit(s_mouse.deviceHandle, s_mouse.classHandle);
    }
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
        /* Bind the SDK HID class driver to the claimed interface. */
        if (USB_HostHidInit(m->deviceHandle, &m->classHandle) != kStatus_USB_Success)
        {
            mouse_fail("HID class init failed");
            return;
        }
        m->devState = kDev_Running;
        m->runState = kRun_WaitSetInterface;
        if (USB_HostHidSetInterface(m->classHandle, m->interfaceHandle, 0U, control_callback, m) !=
            kStatus_USB_Success)
        {
            mouse_fail("SET_INTERFACE failed");
        }
        return;
    }
    if (m->devState != kDev_Running)
    {
        return;
    }

    switch (m->runState)
    {
        case kRun_SetInterfaceDone:
        {
            uint16_t len = find_report_desc_len((usb_host_interface_t *)m->interfaceHandle);
            if ((len == 0U) || (len > sizeof(s_reportDesc)))
            {
                len = sizeof(s_reportDesc); /* device returns what it has */
            }
            /* GET_DESCRIPTOR(Report) -> s_reportDesc */
            m->runState = kRun_WaitReportDesc;
            if (USB_HostHidGetReportDescriptor(m->classHandle, s_reportDesc, len, control_callback, m) !=
                kStatus_USB_Success)
            {
                mouse_fail("report descriptor request failed");
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
                    mouse_fail("no mouse collection in report descriptor");
                    break;
                }
                /* Unparseable descriptor on a boot-capable interface: switch the
                 * mouse to the spec-defined boot protocol format instead. */
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
            m->runState   = kRun_WaitSetProtocol;
            if (USB_HostHidSetProtocol(m->classHandle, proto, control_callback, m) != kStatus_USB_Success)
            {
                m->runState = kRun_SetProtocolDone;
                notify_app();
            }
            break;
        }

        case kRun_SetProtocolDone:
            /* Start streaming. The pipe polls at the mouse's own bInterval
             * (e.g. 1 ms FS, or 125 us for an 8 kHz HS mouse). */
            m->inPacketSize = USB_HostHidGetPacketsize(m->classHandle, USB_ENDPOINT_INTERRUPT, USB_IN);
            if ((m->inPacketSize == 0U) || (m->inPacketSize > APP_HOST_REPORT_BUF_SIZE))
            {
                m->inPacketSize = APP_HOST_REPORT_BUF_SIZE;
            }
            m->rxIndex  = 0U;
            m->runState = kRun_Streaming;
            /* Publish before the first report can arrive. */
            app_status_mouse_ready(&m->layout, m->usingBoot, m->inPacketSize);
            if (USB_HostHidRecv(m->classHandle, s_rxBuf[0], m->inPacketSize, mouse_in_callback, m) !=
                kStatus_USB_Success)
            {
                mouse_fail("interrupt IN transfer failed");
            }
            break;

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

    /* 1) VBUS must be HIGH before the host stack is initialised. */
    vbus_power_sequence();

    /* 2) Clocks/PHY and host stack. */
    usb_host_clock_init();
    if (USB_HostInit(CONTROLLER_ID, &s_hostHandle, usb_host_event) != kStatus_USB_Success)
    {
        /* USB1 and the command channel keep working; the LED shows a fault. */
        app_status_host_init_failed();
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
